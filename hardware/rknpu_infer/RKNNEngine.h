#ifndef RKNN_ENGINE_H
#define RKNN_ENGINE_H

#include "rknn_api.h"
#include <opencv2/opencv.hpp>
#include <string>

class RKNNEngine {
public:
    RKNNEngine();
    ~RKNNEngine();

    // 加载 RKNN 模型
    bool loadModel(const std::string& modelPath);

    // 执行推理 (输入 BGR 格式的 cv::Mat)
    // 返回推理耗时 (毫秒)
    float infer(const cv::Mat& input);

private:
    rknn_context ctx_;
    rknn_input_output_num io_num_;
    bool initialized_;
    
    // 预处理缩放用的 pad 值
    int letterbox_pad_;
};

#endif // RKNN_ENGINE_H