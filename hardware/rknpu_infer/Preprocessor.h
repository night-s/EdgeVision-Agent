#pragma once
#include "Yolov5PostProcess.h"
#ifdef EDGE_WITH_RGA
#include <im2d.hpp>
#include <rga.h>
#endif
class Preprocessor {
    cv::Mat full_, resized_, input_;
    Letterbox box_;
    bool rga_;

  public:
    Preprocessor(int width, int height, bool rga)
        : box_(Letterbox::make(width, height)), rga_(rga) {
        full_.create(height, width, CV_8UC3);
        resized_.create(box_.resized_h, box_.resized_w, CV_8UC3);
        input_ = cv::Mat(640, 640, CV_8UC3, cv::Scalar(114, 114, 114));
#ifndef EDGE_WITH_RGA
        if (rga)
            throw std::runtime_error("RGA not enabled in this build");
#endif
    }
    const cv::Mat &run(const uint8_t *yuyv) {
        cv::Mat raw(box_.height, box_.width, CV_8UC2, const_cast<uint8_t *>(yuyv));
        if (!rga_) {
            cv::cvtColor(raw, full_, cv::COLOR_YUV2RGB_YUYV);
            cv::resize(full_, resized_, resized_.size(), 0, 0, cv::INTER_LINEAR);
        } else {
#ifdef EDGE_WITH_RGA
            auto src = wrapbuffer_virtualaddr_t(raw.data, raw.cols, raw.rows, raw.cols, raw.rows,
                                                RK_FORMAT_YUYV_422);
            auto full = wrapbuffer_virtualaddr_t(full_.data, full_.cols, full_.rows, full_.cols,
                                                 full_.rows, RK_FORMAT_RGB_888);
            auto resized =
                wrapbuffer_virtualaddr_t(resized_.data, resized_.cols, resized_.rows, resized_.cols,
                                         resized_.rows, RK_FORMAT_RGB_888);
            auto status = imcvtcolor(src, full, RK_FORMAT_YUYV_422, RK_FORMAT_RGB_888);
            if (status != IM_STATUS_SUCCESS)
                throw std::runtime_error(std::string("RGA color conversion: ") +
                                         imStrError(status));
            status = imresize(full, resized);
            if (status != IM_STATUS_SUCCESS)
                throw std::runtime_error(std::string("RGA resize: ") + imStrError(status));
#endif
        }
        resized_.copyTo(input_(cv::Rect(box_.left, box_.top, box_.resized_w, box_.resized_h)));
        return input_;
    }
    const Letterbox &geometry() const {
        return box_;
    }
};
