#ifndef RKNN_ENGINE_H
#define RKNN_ENGINE_H

#include "rknn_api.h"
#include <opencv2/opencv.hpp>
#include <string>

class RKNNEngine {
public:
    RKNNEngine();
    ~RKNNEngine();

    bool loadModel(const std::string& modelPath);
    float infer(const cv::Mat& input, rknn_output* outputs);

    rknn_context getCtx() { return ctx_; }

private:
    rknn_context ctx_;
    rknn_input_output_num io_num_;
    bool initialized_;
    int letterbox_pad_;
};

#endif // RKNN_ENGINE_H