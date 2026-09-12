#include "V4L2Camera.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
namespace {
int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}
} // namespace
V4L2Camera::V4L2Camera(std::string device, int w, int h, int fps)
    : device_(std::move(device)), width_(w), height_(h), fps_(fps) {}
V4L2Camera::~V4L2Camera() {
    close();
}
bool V4L2Camera::fail(const std::string &text) {
    error_ = text + ": " + std::strerror(errno);
    return false;
}
bool V4L2Camera::open() {
    close();
    fd_ = ::open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0)
        return fail("open camera");
    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
        return fail("QUERYCAP");
    auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        error_ = "camera requires single-planar capture + streaming";
        return false;
    }
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
        return fail("S_FMT");
    if (fmt.fmt.pix.width != unsigned(width_) || fmt.fmt.pix.height != unsigned(height_) ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        error_ = "camera changed requested size/format; use a supported YUYV mode";
        return false;
    }
    stride_ = fmt.fmt.pix.bytesperline;
    if (stride_ < size_t(width_) * 2) {
        error_ = "invalid camera stride";
        return false;
    }
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps_;
    if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
        return fail("S_PARM");
    v4l2_streamparm actual{};
    actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_PARM, &actual) == 0)
        parm = actual;
    negotiated_fps_ = parm.parm.capture.timeperframe.numerator
                          ? double(parm.parm.capture.timeperframe.denominator) /
                                parm.parm.capture.timeperframe.numerator
                          : 0;
    last_driver_us_ = 0;
    have_sequence_ = false;
    timing_ = {};
    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
        return fail("REQBUFS");
    if (req.count < 2) {
        error_ = "insufficient camera buffers";
        return false;
    }
    buffers_.resize(req.count);
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_buffer b{};
        b.type = req.type;
        b.memory = req.memory;
        b.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0)
            return fail("QUERYBUF");
        auto ptr = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
        if (ptr == MAP_FAILED)
            return fail("mmap");
        buffers_[i] = {ptr, b.length};
    }
    error_.clear();
    return true;
}
bool V4L2Camera::start() {
    for (unsigned i = 0; i < buffers_.size(); ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd_, VIDIOC_QBUF, &b) < 0)
            return fail("QBUF");
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
        return fail("STREAMON");
    streaming_ = true;
    return true;
}
void V4L2Camera::stop() {
    if (streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    streaming_ = false;
}
void V4L2Camera::close() {
    stop();
    for (auto &b : buffers_)
        if (b.data)
            munmap(b.data, b.size);
    buffers_.clear();
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}
V4L2Camera::Result V4L2Camera::copyFrame(uint8_t *dst, size_t capacity, int timeout) {
    auto before = std::chrono::steady_clock::now();
    timing_ = {};
    pollfd p{fd_, POLLIN, 0};
    int ready = poll(&p, 1, timeout);
    timing_.poll_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before)
            .count();
    if (ready == 0 || (ready < 0 && errno == EINTR))
        return Result::Timeout;
    if (ready < 0) {
        fail("camera poll");
        return Result::Error;
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        error_ = "camera disconnected or poll device error";
        return Result::Error;
    }
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN)
            return Result::Timeout;
        fail("DQBUF");
        return Result::Error;
    }
    bool valid = b.index < buffers_.size() && !(b.flags & V4L2_BUF_FLAG_ERROR);
    size_t needed = stride_ * (height_ - 1) + size_t(width_) * 2;
    valid = valid && b.bytesused >= needed && b.bytesused <= buffers_[b.index].size &&
            capacity >= size_t(width_) * height_ * 2;
    if (valid) {
        uint64_t driver_us = uint64_t(b.timestamp.tv_sec) * 1000000 + b.timestamp.tv_usec;
        if (last_driver_us_ && driver_us > last_driver_us_)
            timing_.driver_interval_ms = (driver_us - last_driver_us_) / 1000.;
        last_driver_us_ = driver_us;
        if (have_sequence_ && b.sequence > last_sequence_)
            timing_.sequence_gaps = b.sequence - last_sequence_ - 1;
        last_sequence_ = b.sequence;
        have_sequence_ = true;
        auto copied = std::chrono::steady_clock::now();
        auto src = static_cast<uint8_t *>(buffers_[b.index].data);
        for (int row = 0; row < height_; ++row)
            std::memcpy(dst + size_t(row) * width_ * 2, src + size_t(row) * stride_, width_ * 2);
        timing_.copy_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copied)
                .count();
    }
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) {
        fail("return camera buffer");
        return Result::Error;
    }
    if (!valid) {
        error_ = "invalid or truncated camera frame";
        return Result::Error;
    }
    return Result::Frame;
}
