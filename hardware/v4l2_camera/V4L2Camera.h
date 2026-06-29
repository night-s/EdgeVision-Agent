#ifndef V4L2_CAMERA_H
#define V4L2_CAMERA_H

#include <string>
#include <linux/videodev2.h>

class V4L2Camera {
public:
    V4L2Camera(const std::string& device = "/dev/video0", int width = 640, int height = 480);
    ~V4L2Camera();

    bool open();        // 打开设备并初始化MMAP
    bool start();       // 开启流
    bool stop();        // 停止流
    bool getFrame(unsigned char** buffer, int& size); // 获取一帧数据 (零拷贝)
    void releaseFrame(); // 释放帧缓冲区，还给内核

private:
    bool initMmap();

    std::string device_;
    int width_;
    int height_;
    int fd_;
    
    // MMAP 缓冲区结构
    struct buffer {
        void* start;
        size_t length;
    } *buffers_;
    
    int n_buffers_;
    int current_buf_index_;
};

#endif // V4L2_CAMERA_H