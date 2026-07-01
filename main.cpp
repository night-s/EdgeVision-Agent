#include "hardware/v4l2_camera/V4L2Camera.h"
#include "core/thread/ThreadPool.h"
#include "core/ipc/SafeQueue.h"
#include "core/memory/MemoryPool.h"
#include "hardware/rknpu_infer/RKNNEngine.h"
#include "hardware/rknpu_infer/Yolov5PostProcess.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <cstdlib>
#include <atomic>
#include <unistd.h>  
#include <fcntl.h>   
#include<agent/SkillManager.h>
#include "agent/actions/CaptureSkill.h" 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "third_party/nlohmann_json.hpp"
using json = nlohmann::json;

const int WIDTH = 640;
const int HEIGHT = 480;
const int CHANNELS = 3;
const size_t FRAME_SIZE = WIDTH * HEIGHT * CHANNELS;

MemoryPool& pool = MemoryPool::getInstance(FRAME_SIZE, 4);

struct PooledFrame {
    cv::Mat mat;
    void* data_ptr;
};

SafeQueue<PooledFrame> frame_queue;
std::atomic<bool> is_running(true);

void capture_thread_func(V4L2Camera* cam) {
    unsigned char* frame_data;
    int frame_size;
    while (is_running) {
        if (cam->getFrame(&frame_data, frame_size)) {
            void* mem_ptr = pool.allocate();
            cv::Mat yuyv(HEIGHT, WIDTH, CV_8UC2, frame_data);
            cv::Mat bgr(HEIGHT, WIDTH, CV_8UC3, mem_ptr);
            cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
            cam->releaseFrame(); 
            
            PooledFrame pf;
            pf.data_ptr = mem_ptr;
            pf.mat = bgr;
            frame_queue.push(pf);
        }
    }
}

int main() {
    RKNNEngine engine;
    if (!engine.loadModel("models/yolov5s-640-640.rknn")) return -1;

    V4L2Camera cam("/dev/video10", WIDTH, HEIGHT);
    if (!cam.open() || !cam.start()) return -1;

    // 初始化 SkillManager 并注册技能
    SkillManager skill_manager;
    skill_manager.registerSkill(std::make_unique<CaptureSkill>());

    // 注册队列丢帧回调，防止内存池泄漏
    frame_queue.setDropCallback([](const PooledFrame& dropped_pf) {
        pool.deallocate(dropped_pf.data_ptr);
    });

    std::thread capture_thread(capture_thread_func, &cam);
    std::cout << "NPU Pipeline started. Saving result image every 30 frames. Press 'q' to quit." << std::endl;

    // 设置标准输入为非阻塞模式，让主循环可以检测按键
    int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);

    Yolov5PostProcess post_process;
    int frame_count = 0;

    // === TCP 命令接收队列 ===
    SafeQueue<std::string> cmd_queue;
    std::atomic<bool> tcp_running(true);

    // === TCP 服务端线程 ===
    std::thread tcp_thread([&]() {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "[TCP Error] Socket creation failed!" << std::endl;
            return;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡接口
        address.sin_port = htons(9000);

        // 绑定端口，并检查是否失败！
        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "[TCP Error] Bind failed on port 9000! (可能是端口被占用)" << std::endl;
            close(server_fd);
            return;
        }

        // 开始监听
        if (listen(server_fd, 3) < 0) {
            std::cerr << "[TCP Error] Listen failed!" << std::endl;
            close(server_fd);
            return;
        }

        std::cout << "[TCP] Listening on port 9000 (All interfaces). Ready for LLM commands!" << std::endl;

        while (tcp_running) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;

            char buffer[1024] = {0};
            if (read(client_fd, buffer, 1024) > 0) {
                try {
                    std::string raw_cmd(buffer);
                    json j = json::parse(raw_cmd);
                    if (j.contains("skill_name")) {
                        std::string skill_name = j["skill_name"];
                        cmd_queue.push(skill_name);
                        std::cout << "[TCP] Received command: " << skill_name << std::endl;
                    }
                } catch (...) {
                    std::cerr << "[TCP] Invalid JSON received." << std::endl;
                }
            }
            close(client_fd);
        }
        close(server_fd);
    });

    // 消费者主循环
    while (is_running) {
        PooledFrame pf;
        if (frame_queue.pop(pf, 1000)) {
            // 1. 准备接收 NPU 输出
            rknn_output outputs[3]; 
            float infer_time = engine.infer(pf.mat, outputs);
            
            // 2. 后处理解析检测框
            std::vector<Detection> dets = post_process.process(outputs, WIDTH, HEIGHT);
            
            // 防爆保护
            if (dets.size() > 200) dets.clear();
            
            // 3. 释放 NPU 输出内存
            rknn_outputs_release(engine.getCtx(), 3, outputs);

            // 4. Agent 决策逻辑
            skill_manager.execute(dets, pf.mat); 

            // 5. 每隔 30 帧保存一次展示图片
            if (frame_count % 30 == 0) {
                post_process.draw(pf.mat, dets);
                std::string text = "Infer: " + std::to_string(infer_time) + "ms, Dets: " + std::to_string(dets.size());
                cv::putText(pf.mat, text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
                cv::imwrite("result_output.jpg", pf.mat);
                std::cout << "[RESULT] Saved result_output.jpg | Infer: " << infer_time << "ms | Dets: " << dets.size() << std::endl;
            }
            frame_count++;

            // === 执行网络命令 ===
            std::string cmd;
            if (cmd_queue.pop(cmd, 0)) { 
                if (cmd == "CaptureSkill") {
                    std::cout << "[EXEC] Executing remote command: CaptureSkill" << std::endl;
                    bool executed = skill_manager.executeSkillByName(cmd, dets, pf.mat);
                    if (executed) {
                        std::cout << "[EXEC] Command executed successfully." << std::endl;
                    }
                }
            }

            // 归还内存池
            pool.deallocate(pf.data_ptr);

            
            // 5. 检测键盘 'q'，实现优雅退出
            char ch;
            if (read(STDIN_FILENO, &ch, 1) > 0 && ch == 'q') {
                is_running = false;
                break;
            }
        }
    }

    // 恢复终端设置
    fcntl(STDIN_FILENO, F_SETFL, old_flags);
    
    capture_thread.join();
    cam.stop();
    std::cout << "EdgeVision-Agent cleanly exited." << std::endl;
    return 0;
}