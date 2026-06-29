#include "hardware/v4l2_camera/V4L2Camera.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    // 强制 GUI 输出到板子的物理屏幕
    setenv("DISPLAY", ":0", 1); 

    V4L2Camera cam("/dev/video10", 640, 480);
    if (!cam.open() || !cam.start()) {
        std::cerr << "Camera init failed!" << std::endl;
        return -1;
    }

    unsigned char* frame_data;
    int frame_size;

    cv::namedWindow("EdgeVision-Agent", cv::WINDOW_AUTOSIZE);
    std::cout << "Camera streaming started. Press 'q' on the screen to quit." << std::endl;

    while (true) {
        if (cam.getFrame(&frame_data, frame_size)) {
            cv::Mat yuyv(480, 640, CV_8UC2, frame_data);
            cv::Mat bgr;
            cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);

            if (!bgr.empty()) {
                cv::imshow("EdgeVision-Agent", bgr);
                // 等待1毫秒，检测键盘输入 'q' 退出
                if (cv::waitKey(1) == 'q') {
                    break;
                }
            }
            cam.releaseFrame();
        }
    }

    cam.stop();
    return 0;
}