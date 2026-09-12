#pragma once
#include <cstdint>
#include <string>
#include <vector>
class V4L2Camera {
  public:
    enum class Result { Frame, Timeout, Error };
    V4L2Camera(std::string device, int width, int height, int fps = 30);
    ~V4L2Camera();
    V4L2Camera(const V4L2Camera &) = delete;
    V4L2Camera &operator=(const V4L2Camera &) = delete;
    bool open();
    bool start();
    void close();
    void stop();
    Result copyFrame(uint8_t *dst, size_t capacity, int timeout_ms = 200);
    const std::string &error() const {
        return error_;
    }
    double fps() const {
        return negotiated_fps_;
    }
    struct Timing {
        double poll_ms = 0, copy_ms = 0, driver_interval_ms = 0;
        uint64_t sequence_gaps = 0;
    };
    const Timing &timing() const {
        return timing_;
    }

  private:
    bool fail(const std::string &text);
    struct Buffer {
        void *data = nullptr;
        size_t size = 0;
    };
    std::string device_, error_;
    int width_, height_, fps_, fd_ = -1;
    double negotiated_fps_ = 0;
    Timing timing_;
    uint64_t last_driver_us_ = 0;
    uint32_t last_sequence_ = 0;
    bool have_sequence_ = false;
    size_t stride_ = 0;
    bool streaming_ = false;
    std::vector<Buffer> buffers_;
};
