#include "hardware/v4l2_camera/V4L2Camera.h"
#include "core/thread/ThreadPool.h"
#include "core/ipc/SafeQueue.h"
#include "core/memory/MemoryPool.h"
#include "hardware/rknpu_infer/RKNNEngine.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <cstdlib>
#include <atomic>

const int CHANNELS = 3;
const int WIDTH = 640;
const int HEIGHT = 480;
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
            std::cout << "[DEBUG] GetFrame SUCCESS! Pushing to queue..." << std::endl;
            void* mem_ptr = pool.allocate();
            cv::Mat yuyv(HEIGHT, WIDTH, CV_8UC2, frame_data);
            cv::Mat bgr(HEIGHT, WIDTH, CV_8UC3, mem_ptr);
            cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
            
            // ⚠️ 确保这里调用了 releaseFrame！否则下一帧拿不到
            cam->releaseFrame(); 
            
            PooledFrame pf;
            pf.data_ptr = mem_ptr;
            pf.mat = bgr;
            frame_queue.push(pf);
        } else {
            // 这一行之前没加，现在必须加上！
            std::cout << "[DEBUG] GetFrame FAILED! No data yet." << std::endl;
        }
    }
}

int main() {
    setenv("DISPLAY", ":0", 1);

    // 1. 初始化 NPU 引擎
    RKNNEngine engine;
    if (!engine.loadModel("models/yolov5s-640-640.rknn")) {
        std::cerr << "Failed to load RKNN model!" << std::endl;
        return -1;
    }

    // 2. 初始化摄像头
    V4L2Camera cam("/dev/video10", WIDTH, HEIGHT);
    if (!cam.open() || !cam.start()) {
        std::cerr << "Camera init failed!" << std::endl;
        return -1;
    }

    // 3. 启动采集线程
    std::thread capture_thread(capture_thread_func, &cam);
    // cv::namedWindow("EdgeVision-Agent", cv::WINDOW_AUTOSIZE);
    std::cout << "NPU Pipeline started. Press 'q' to quit." << std::endl;

    // 4. 消费者主循环
    while (is_running) {
        PooledFrame pf;
        if (frame_queue.pop(pf, 1000)) {
            std::cout << "[DEBUG] Consumer got frame! Inferring..." << std::endl;
            
            // 调用 NPU 进行推理
            float infer_time = engine.infer(pf.mat);
            
            // 打印推理耗时到终端，替代在图像上画字
            std::cout << "[RESULT] NPU Infer time: " << infer_time << " ms" << std::endl;

            cv::putText(pf.mat, text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
            cv::imshow("EdgeVision-Agent", pf.mat);
            
            // 用终端输入 'q' 代替 cv::waitKey
            if (cv::waitKey(1) == 'q') {
                is_running = false;
                break;
            }
            
            pool.deallocate(pf.data_ptr);
        } else {
            std::cout << "[DEBUG] Consumer timeout, no frame." << std::endl;
        }
    }
    capture_thread.join();
    cam.stop();
    return 0;
}