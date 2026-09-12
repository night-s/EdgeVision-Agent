#pragma once
#include "third_party/nlohmann_json.hpp"
#include <fstream>
#include <string>
#include <stdexcept>
#include <cmath>
using json=nlohmann::json;
struct Config {
 std::string device="/dev/video10",model="models/yolov5s-640-640.rknn",preprocess="cpu";
 std::string output="output",output_activation="logits",replay_yuyv="";
 int width=640,height=480,fps=30,port=9000,rtsp_port=8554,bitrate=2000000;
 int queue=2,pool=12,run_seconds=0,infer_delay_ms=0;
 int infer_slow_ms=150,infer_stall_ms=2000,infer_max_age_ms=500,infer_degraded_interval_ms=200;
 bool allow_fault_injection=false;
 int event_confirm=3,event_tail=5,event_pre=3;
 bool video=true,synthetic=false,native_outputs=true;
 float threshold=0.35f,nms=0.45f;
 std::vector<float> roi{0,0,1,1};
 static Config load(const std::string& path) {
  Config c;std::ifstream in(path);
  if(!in)throw std::runtime_error("cannot open config: "+path);
  json j;in>>j;
  if(!j.is_object())throw std::runtime_error("config must be a JSON object");
#define READ(k) if(j.contains(#k)) c.k=j.at(#k).get<decltype(c.k)>()
  READ(device);READ(model);READ(preprocess);READ(output);READ(output_activation);READ(replay_yuyv);
  READ(width);READ(height);READ(fps);READ(port);READ(rtsp_port);READ(bitrate);
  READ(queue);READ(pool);READ(run_seconds);READ(infer_delay_ms);
  READ(infer_slow_ms);READ(infer_stall_ms);READ(infer_max_age_ms);READ(infer_degraded_interval_ms);READ(allow_fault_injection);
  READ(event_confirm);READ(event_tail);READ(event_pre);
  READ(video);READ(synthetic);READ(native_outputs);READ(threshold);READ(nms);READ(roi);
#undef READ
  if(c.width<96 || c.width>1920 || c.width%16 || c.height<64 || c.height>1080 || c.height%2 ||
     c.fps<1 || c.fps>60 || c.queue<1 || c.queue>8 || c.pool<c.queue+6 || c.pool>64 ||
     c.port<1024 || c.port>65535 || c.rtsp_port<1024 || c.rtsp_port>65535 ||
     c.port==c.rtsp_port || c.threshold<=0 || c.threshold>=1 || c.nms<=0 || c.nms>=1 ||
     c.event_confirm<1 || c.event_tail<1 || c.event_tail>60 || c.event_pre<0 || c.event_pre>15 ||
     c.infer_slow_ms<10 || c.infer_slow_ms>5000 || c.infer_stall_ms<=c.infer_slow_ms || c.infer_stall_ms>60000 ||
     c.infer_max_age_ms<50 || c.infer_max_age_ms>10000 || c.infer_degraded_interval_ms<0 || c.infer_degraded_interval_ms>5000 ||
     c.run_seconds<0 || c.infer_delay_ms<0 || c.infer_delay_ms>5000 ||
     c.bitrate<100000 || c.bitrate>20000000)
    throw std::runtime_error("config values outside supported limits");
  if(c.preprocess!="cpu" && c.preprocess!="rga")throw std::runtime_error("preprocess must be cpu/rga");
  if(c.output_activation!="logits" && c.output_activation!="probabilities")
    throw std::runtime_error("output_activation must be logits/probabilities");
  for(float v:c.roi)if(!std::isfinite(v))throw std::runtime_error("nonfinite ROI");
  if(c.roi.size()!=4 || c.roi[0]<0 || c.roi[1]<0 || c.roi[2]>1 || c.roi[3]>1 ||
     c.roi[0]>=c.roi[2] || c.roi[1]>=c.roi[3])throw std::runtime_error("invalid normalized ROI");
  if(!c.replay_yuyv.empty())c.synthetic=true;
  return c;
 }
};
