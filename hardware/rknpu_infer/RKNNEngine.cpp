#include "RKNNEngine.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>

RKNNEngine::RKNNEngine() : initialized_(false), letterbox_pad_(0) {}

RKNNEngine::~RKNNEngine() {
    if (initialized_) {
        rknn_destroy(ctx_);
    }
}

bool RKNNEngine::loadModel(const std::string& modelPath) {
    // 1. 读取模型文件
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[RKNN] Failed to open model file: " << modelPath << std::endl;
        return false;
    }
    size_t modelSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> modelData(modelSize);
    file.read(modelData.data(), modelSize);
    file.close();

    // 2. 初始化 RKNN
    int ret = rknn_init(&ctx_, modelData.data(), modelSize, 0, nullptr);
    if (ret < 0) {
        std::cerr << "[RKNN] rknn_init failed! ret=" << ret << std::endl;
        return false;
    }

    // 3. 查询输入输出节点数量
    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
    if (ret < 0) {
        std::cerr << "[RKNN] rknn_query failed! ret=" << ret << std::endl;
        return false;
    }

    std::cout << "[RKNN] Model loaded. Input num: " << io_num_.n_input 
              << ", Output num: " << io_num_.n_output << std::endl;
              
    initialized_ = true;
    return true;
}

float RKNNEngine::infer(const cv::Mat& input) {
    if (!initialized_) return -1.0f;

    // 1. 预处理: YOLOv5 需要 640x640 的正方形输入，采用 Letterbox 填充
    cv::Mat resized;
    float scale = std::min(640.0f / input.cols, 640.0f / input.rows);
    int new_w = input.cols * scale;
    int new_h = input.rows * scale;
    cv::resize(input, resized, cv::Size(new_w, new_h));
    
    // 计算上下左右的 padding
    int top = (640 - new_h) / 2;
    int bottom = 640 - new_h - top;
    int left = (640 - new_w) / 2;
    int right = 640 - new_w - left;
    letterbox_pad_ = left; // 记录用于后处理还原坐标
    
    cv::copyMakeBorder(resized, resized, top, bottom, left, right, cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));

    // BGR 转 RGB
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

    // 2. 设置输入
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = 640 * 640 * 3;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].buf = rgb.data;
    inputs[0].pass_through = 0; // SDK内部会做归一化

    int ret = rknn_inputs_set(ctx_, 1, inputs);
    if (ret < 0) {
        std::cerr << "[RKNN] rknn_inputs_set failed! ret=" << ret << std::endl;
        return -1.0f;
    }

    // 3. 执行推理并计时
    auto start = std::chrono::high_resolution_clock::now();
    ret = rknn_run(ctx_, nullptr);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (ret < 0) {
        std::cerr << "[RKNN] rknn_run failed! ret=" << ret << std::endl;
        return -1.0f;
    }

    // 4. 获取输出 (此处仅获取，暂不做后处理解析)
    rknn_output outputs[io_num_.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num_.n_output; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 0; // 保持 INT8 原始输出，加速后处理
    }
    ret = rknn_outputs_get(ctx_, io_num_.n_output, outputs, nullptr);
    if (ret < 0) {
        std::cerr << "[RKNN] rknn_outputs_get failed! ret=" << ret << std::endl;
        return -1.0f;
    }

    // 释放输出内存
    rknn_outputs_release(ctx_, io_num_.n_output, outputs);

    // 返回推理耗时 (毫秒)
    return std::chrono::duration<float, std::milli>(end - start).count();
}