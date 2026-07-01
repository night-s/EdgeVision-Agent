#pragma once
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include "hardware/rknpu_infer/Yolov5PostProcess.h"

class Skill {
public:
    virtual ~Skill() = default;
    virtual std::string getName() const = 0;
    virtual bool execute(const std::vector<Detection>& dets, cv::Mat& frame) = 0;
};