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

            // 🆕 4. Agent 决策逻辑：检测到"人"或"车"时的行为
            bool target_detected = false;
            for (auto& d : dets) {
                if (d.class_id == 0 || d.class_id == 1 || d.class_id == 2) { // 人, 自行车, 车
                    target_detected = true;
                    break;
                }
            }

            if (target_detected) {
                std::cout << "[AGENT ACTION] Target detected! Triggering alarm..." << std::endl;
                post_process.draw(pf.mat, dets);
                cv::imwrite("event_capture.jpg", pf.mat); // 抓拍事件现场
            }

            // 5. 每隔 30 帧保存一次展示图片
            if (frame_count % 30 == 0) {
                post_process.draw(pf.mat, dets);
                std::string text = "Infer: " + std::to_string(infer_time) + "ms, Dets: " + std::to_string(dets.size());
                cv::putText(pf.mat, text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
                cv::imwrite("result_output.jpg", pf.mat);
                std::cout << "[RESULT] Saved result_output.jpg | Infer: " << infer_time << "ms | Dets: " << dets.size() << std::endl;
            }
            frame_count++;
            
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