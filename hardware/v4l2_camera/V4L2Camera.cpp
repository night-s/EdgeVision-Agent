#include "V4L2Camera.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <iostream>
#include <errno.h>

V4L2Camera::V4L2Camera(const std::string& device, int width, int height)
    : device_(device), width_(width), height_(height), fd_(-1), buffers_(nullptr), n_buffers_(0), current_buf_index_(-1) {}

V4L2Camera::~V4L2Camera() {
    stop();
    if (buffers_) {
        for (int i = 0; i < n_buffers_; ++i) {
            if (buffers_[i].start != MAP_FAILED && buffers_[i].start != nullptr) {
                munmap(buffers_[i].start, buffers_[i].length);
            }
        }
        delete[] buffers_;
    }
    if (fd_ != -1) close(fd_);
}

bool V4L2Camera::open() {
    fd_ = ::open(device_.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "[V4L2] Cannot open " << device_ << std::endl;
        return false;
    }

    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        std::cerr << "[V4L2] VIDIOC_QUERYCAP failed" << std::endl;
        return false;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // video10 支持 YUYV
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "[V4L2] VIDIOC_S_FMT failed: " << strerror(errno) << std::endl;
        return false;
    }

    return initMmap();
}

bool V4L2Camera::initMmap() {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "[V4L2] VIDIOC_REQBUFS failed" << std::endl;
        return false;
    }

    buffers_ = new buffer[req.count];
    n_buffers_ = req.count;

    for (int i = 0; i < n_buffers_; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "[V4L2] VIDIOC_QUERYBUF failed" << std::endl;
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);

        if (buffers_[i].start == MAP_FAILED) {
            std::cerr << "[V4L2] mmap failed" << std::endl;
            return false;
        }
    }
    return true;
}

bool V4L2Camera::start() {
    for (int i = 0; i < n_buffers_; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "[V4L2] VIDIOC_QBUF failed: " << strerror(errno) << std::endl;
            return false;
        }
    }
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[V4L2] VIDIOC_STREAMON failed: " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool V4L2Camera::stop() {
    if (fd_ != -1) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    return true;
}

bool V4L2Camera::getFrame(unsigned char** buffer, int& size) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        return false;
    }

    current_buf_index_ = buf.index;
    *buffer = (unsigned char*)buffers_[buf.index].start;
    size = buf.bytesused;
    return true;
}

void V4L2Camera::releaseFrame() {
    if (current_buf_index_ != -1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = current_buf_index_;
        ioctl(fd_, VIDIOC_QBUF, &buf);
        current_buf_index_ = -1;
    }
}