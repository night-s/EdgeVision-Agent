#pragma once
#include <string>
#include <vector>
#include <cstdint>
class V4L2Camera {
public:
 enum class Result { Frame, Timeout, Error };
 V4L2Camera(std::string device,int width,int height,int fps=30);
 ~V4L2Camera();
 V4L2Camera(const V4L2Camera&)=delete;
 V4L2Camera& operator=(const V4L2Camera&)=delete;
 bool open();
 bool start();
 void close();
 void stop();
 Result copyFrame(uint8_t* dst,size_t capacity,int timeout_ms=200);
 const std::string& error() const { return error_; }
 int fps() const { return fps_; }
private:
 bool fail(const std::string& text);
 struct Buffer { void* data=nullptr; size_t size=0; };
 std::string device_,error_;
 int width_,height_,fps_,fd_=-1;
 size_t stride_=0; bool streaming_=false;
 std::vector<Buffer> buffers_;
};
