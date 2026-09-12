#pragma once
#include "Yolov5PostProcess.h"
#include <array>
#include <string>
struct InferenceResult {
 std::vector<Detection> detections;
 double input_ms=0,run_ms=0,output_ms=0,post_ms=0;
};
class RKNNEngine {
public:
 RKNNEngine()=default;
 ~RKNNEngine();
 RKNNEngine(const RKNNEngine&)=delete;
 RKNNEngine& operator=(const RKNNEngine&)=delete;
 void loadModel(const std::string& path);
 InferenceResult infer(const cv::Mat& rgb,const Letterbox& box,float threshold,float nms,bool logits,bool native_outputs=true);
private:
 rknn_context ctx_=0;bool initialized_=false;
 std::array<rknn_tensor_attr,3> attrs_{};
};
