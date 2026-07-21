#include "hardware/v4l2_camera/V4L2Camera.h"
#include "core/thread/ThreadPool.h"
#include "core/ipc/SafeQueue.h"
#include "core/memory/MemoryPool.h"
#include "core/reactor/EventLoop.h"
#include "core/reactor/TcpServer.h"
#include "hardware/rknpu_infer/RKNNEngine.h"
#include "hardware/rknpu_infer/Yolov5PostProcess.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <cstdlib>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <agent/SkillManager.h>
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

// MemoryPool: 先无参获取单例，再两阶段初始化
MemoryPool& pool = MemoryPool::getInstance();

struct PooledFrame {
    cv::Mat mat;
    void* data_ptr;
};

SafeQueue<PooledFrame> frame_queue;
std::atomic<bool> is_running(true);

// TCP 命令接收队列（EventLoop 线程 → Main 线程）
SafeQueue<std::string> cmd_queue;

void capture_thread_func(V4L2Camera* cam) {
    unsigned char* frame_data;
    int frame_size;
    while (is_running) {
        if (cam->getFrame(&frame_data, frame_size)) {
            void* mem_ptr = pool.allocate();
            if (!mem_ptr) {
                // 池未初始化或 OOM
                cam->releaseFrame();
                continue;
            }
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
    // === 初始化 MemoryPool ===
    pool.init(FRAME_SIZE, 4);

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

    // === 创建 EventLoop + TcpServer ===
    EventLoop event_loop;
    TcpServer tcp_server(event_loop);

    // TCP 消息回调：收到 JSON RPC 命令，推入 cmd_queue
    tcp_server.setMessageCallback([&](int client_fd, const std::string& msg) {
        try {
            json j = json::parse(msg);
            if (j.contains("skill_name")) {
                std::string skill_name = j["skill_name"];
                cmd_queue.push(skill_name);
                std::cout << "[TCP] Received command: " << skill_name << std::endl;

                // 回复确认
                json resp;
                resp["status"] = "ok";
                resp["command"] = skill_name;
                tcp_server.sendResponse(client_fd, resp.dump() + "\n");
            }
        } catch (const json::parse_error& e) {
            std::cerr << "[TCP] JSON parse error: " << e.what() << std::endl;
            json err;
            err["status"] = "error";
            err["reason"] = "invalid json";
            tcp_server.sendResponse(client_fd, err.dump() + "\n");
        }
    });

    tcp_server.setConnectionCallback([](int client_fd) {
        std::cout << "[TCP] Client connected fd=" << client_fd << std::endl;
    });

    tcp_server.setCloseCallback([](int client_fd) {
        std::cout << "[TCP] Client disconnected fd=" << client_fd << std::endl;
    });

    // 启动 TCP 服务（端口 9000）
    if (!tcp_server.start(9000)) {
        std::cerr << "Failed to start TcpServer on port 9000" << std::endl;
        return -1;
    }

    // === 添加健康检查定时器（每 5 秒）===
    // 使用 EventLoop 的 runInLoop 保证定时器操作在 EventLoop 线程中执行
    event_loop.runInLoop([&]() {
        event_loop.timerMgr().addTimer(5000, 5000, [&]() {
            std::cout << "[Health] running, queue="
                      << frame_queue.size()
                      << ", connections="
                      << tcp_server.connectionCount()
                      << std::endl;
        });
    });

    // === 启动 EventLoop 线程 ===
    std::thread event_thread([&]() {
        std::cout << "[EventLoop] Started on thread" << std::endl;
        event_loop.run();
        std::cout << "[EventLoop] Exited." << std::endl;
    });

    // === 启动采集线程 ===
    std::thread capture_thread(capture_thread_func, &cam);
    std::cout << "NPU Pipeline started. Saving result image every 30 frames. Press 'q' to quit." << std::endl;

    // 设置标准输入为非阻塞模式，让主循环可以检测按键
    int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);

    Yolov5PostProcess post_process;
    int frame_count = 0;

    // === 消费者主循环 ===
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

            // === 执行网络命令（非阻塞检查 cmd_queue） ===
            std::string cmd;
            if (cmd_queue.pop(cmd, 0)) {
                if (cmd == "CaptureSkill") {
                    std::cout << "[EXEC] Executing remote command: CaptureSkill" << std::endl;
                    bool executed = skill_manager.executeSkillByName(cmd, dets, pf.mat);
                    if (executed) {
                        std::cout << "[EXEC] Command executed successfully." << std::endl;
                    }
                } else {
                    std::cout << "[EXEC] Unknown command: " << cmd << std::endl;
                }
            }

            // 归还内存池
            pool.deallocate(pf.data_ptr);

            // 6. 检测键盘 'q'，实现优雅退出
            char ch;
            if (read(STDIN_FILENO, &ch, 1) > 0 && ch == 'q') {
                is_running = false;
                break;
            }
        }
    }

    // === 恢复终端设置 ===
    fcntl(STDIN_FILENO, F_SETFL, old_flags);

    // === 优雅关闭 ===
    std::cout << "[Shutdown] Stopping event loop..." << std::endl;
    event_loop.stop();
    event_thread.join();

    std::cout << "[Shutdown] Stopping capture thread..." << std::endl;
    capture_thread.join();

    cam.stop();
    std::cout << "EdgeVision-Agent cleanly exited." << std::endl;
    return 0;
}
