#include "hardware/v4l2_camera/V4L2Camera.h"
#include "core/thread/ThreadPool.h"
#include "core/ipc/SafeQueue.h"
#include "core/memory/MemoryPool.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <cstdlib>
#include <atomic>

// 图像尺寸常量
const int WIDTH = 640;
const int HEIGHT = 480;
const int CHANNELS = 3;
const size_t FRAME_SIZE = WIDTH * HEIGHT * CHANNELS;

// 获取全局内存池实例
MemoryPool& pool = MemoryPool::getInstance(FRAME_SIZE, 4);

// 包装一个带内存池指针的 Mat 结构，方便归还
struct PooledFrame {
    cv::Mat mat;
    void* data_ptr;
};

// 全局安全队列
SafeQueue<PooledFrame> frame_queue;
std::atomic<bool> is_running(true);

// 生产者线程
void capture_thread_func(V4L2Camera* cam) {
    unsigned char* frame_data;
    int frame_size;
    while (is_running) {
        if (cam->getFrame(&frame_data, frame_size)) {
            // 1. 从内存池申请内存
            void* mem_ptr = pool.allocate();
            
            // 2. 将 V4L2 的 YUYV 数据转 BGR，直接写入内存池的块中
            cv::Mat yuyv(HEIGHT, WIDTH, CV_8UC2, frame_data);
            cv::Mat bgr(HEIGHT, WIDTH, CV_8UC3, mem_ptr); // 指向内存池的 Mat
            cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
            
            cam->releaseFrame();
            
            // 3. 包装入队
            PooledFrame pf;
            pf.data_ptr = mem_ptr;
            pf.mat = bgr; // 浅拷贝，共享底层数据
            frame_queue.push(pf);
        }
    }
}

int main() {
    setenv("DISPLAY", ":0", 1);

    V4L2Camera cam("/dev/video10", WIDTH, HEIGHT);
    if (!cam.open() || !cam.start()) {
        std::cerr << "Camera init failed!" << std::endl;
        return -1;
    }

    std::thread capture_thread(capture_thread_func, &cam);
    cv::namedWindow("EdgeVision-Agent", cv::WINDOW_AUTOSIZE);
    std::cout << "Memory Pool Pipeline started. Press 'q' to quit." << std::endl;

    // 消费者主循环
    while (is_running) {
        PooledFrame pf;
        if (frame_queue.pop(pf, 1000)) {
            cv::imshow("EdgeVision-Agent", pf.mat);
            if (cv::waitKey(1) == 'q') {
                is_running = false;
                break;
            }
            // 4. 重要：显示完毕后，将内存归还给内存池！
            pool.deallocate(pf.data_ptr);
        }
    }

    capture_thread.join();
    cam.stop();
    return 0;
}