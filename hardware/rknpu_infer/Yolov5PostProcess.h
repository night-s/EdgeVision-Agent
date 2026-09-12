#pragma once
#include "rknn_api.h"
#include <algorithm>
#include <cmath>
#include <opencv2/opencv.hpp>
#include <stdexcept>
#include <vector>
struct Detection {
    int class_id;
    float confidence;
    cv::Rect box;
};
struct Letterbox {
    int width = 640, height = 640, resized_w = 640, resized_h = 640, left = 0, top = 0;
    static Letterbox make(int w, int h) {
        if (w <= 0 || h <= 0)
            throw std::invalid_argument("invalid input size");
        Letterbox b;
        b.width = w;
        b.height = h;
        float scale = std::min(640.f / w, 640.f / h);
        b.resized_w = std::max(1, int(std::round(w * scale)));
        b.resized_h = std::max(1, int(std::round(h * scale)));
        b.left = (640 - b.resized_w) / 2;
        b.top = (640 - b.resized_h) / 2;
        return b;
    }
    cv::Rect restore(float x1, float y1, float x2, float y2) const {
        float sx = float(width) / resized_w, sy = float(height) / resized_h;
        int l = std::clamp(int(std::round((x1 - left) * sx)), 0, width);
        int t = std::clamp(int(std::round((y1 - top) * sy)), 0, height);
        int r = std::clamp(int(std::round((x2 - left) * sx)), 0, width);
        int b = std::clamp(int(std::round((y2 - top) * sy)), 0, height);
        return {l, t, std::max(0, r - l), std::max(0, b - t)};
    }
};
class Yolov5PostProcess {
  public:
    static float iou(const Detection &a, const Detection &b) {
        double inter = (a.box & b.box).area(), area = double(a.box.area()) + b.box.area() - inter;
        return area > 0 ? float(inter / area) : 0;
    }
    static std::vector<Detection> nms(std::vector<Detection> d, float threshold) {
        std::sort(d.begin(), d.end(), [](auto &a, auto &b) { return a.confidence > b.confidence; });
        // A bounded top-k controls quadratic NMS time on pathological model output.
        if (d.size() > 1000)
            d.resize(1000);
        std::vector<Detection> out;
        out.reserve(100);
        for (const auto &candidate : d) {
            bool suppressed = false;
            for (const auto &kept : out)
                if (kept.class_id == candidate.class_id && iou(kept, candidate) > threshold) {
                    suppressed = true;
                    break;
                }
            if (!suppressed)
                out.push_back(candidate);
            if (out.size() == 100)
                break;
        }
        return out;
    }
    std::vector<Detection> process(const rknn_output *outputs, const Letterbox &b, float threshold,
                                   float nms_threshold, bool logits,
                                   const rknn_tensor_attr *quantization = nullptr) const {
        static constexpr float anchors[3][3][2] = {{{10, 13}, {16, 30}, {33, 23}},
                                                   {{30, 61}, {62, 45}, {59, 119}},
                                                   {{116, 90}, {156, 198}, {373, 326}}};
        std::vector<Detection> candidates;
        candidates.reserve(256);
        auto activation = [logits](float x) { return logits ? 1.f / (1.f + std::exp(-x)) : x; };
        for (int level = 0; level < 3; ++level) {
            int stride = 8 << level, grid = 640 / stride, hw = grid * grid;
            const auto *attr = quantization ? &quantization[level] : nullptr;
            if (attr && ((attr->type != RKNN_TENSOR_INT8 && attr->type != RKNN_TENSOR_UINT8) ||
                         attr->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
                         !std::isfinite(attr->scale) || attr->scale <= 0))
                throw std::runtime_error("unsupported YOLO quantization");
            if (!outputs[level].buf ||
                outputs[level].size < unsigned(255 * hw * (attr ? 1 : sizeof(float))))
                throw std::runtime_error("invalid YOLO output buffer");
            auto read = [&](int index) {
                if (!attr)
                    return static_cast<const float *>(outputs[level].buf)[index];
                int value = attr->type == RKNN_TENSOR_INT8
                                ? static_cast<const int8_t *>(outputs[level].buf)[index]
                                : static_cast<const uint8_t *>(outputs[level].buf)[index];
                return (value - attr->zp) * attr->scale;
            };
            for (int a = 0; a < 3; ++a)
                for (int cell = 0; cell < hw; ++cell) {
                    int offset = a * 85 * hw + cell;
                    float objectness = activation(read(offset + 4 * hw));
                    if (!std::isfinite(objectness))
                        continue;
                    if (!logits && (objectness < -.001f || objectness > 1.001f))
                        throw std::runtime_error(
                            "YOLO objectness is not a probability; select logits for this export");
                    if (objectness < threshold)
                        continue;
                    float score = 0;
                    int cls = 0;
                    for (int c = 0; c < 80; ++c) {
                        float v = activation(read(offset + (5 + c) * hw));
                        if (v > score) {
                            score = v;
                            cls = c;
                        }
                    }
                    float confidence = score * objectness;
                    if (!std::isfinite(confidence) || confidence < threshold)
                        continue;
                    if (confidence > 1.001f)
                        throw std::runtime_error(
                            "YOLO activation mismatch; check config/model export");
                    float tx = activation(read(offset)), ty = activation(read(offset + hw));
                    float tw = activation(read(offset + 2 * hw)),
                          th = activation(read(offset + 3 * hw));
                    if (!std::isfinite(tx + ty + tw + th) || tx < 0 || tx > 1 || ty < 0 || ty > 1 ||
                        tw < 0 || tw > 1 || th < 0 || th > 1)
                        throw std::runtime_error(
                            "YOLO coordinates outside normalized range; check output_activation");
                    float cx = (tx * 2 - .5f + cell % grid) * stride,
                          cy = (ty * 2 - .5f + cell / grid) * stride;
                    float w = 4 * tw * tw * anchors[level][a][0],
                          h = 4 * th * th * anchors[level][a][1];
                    auto box = b.restore(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
                    if (box.area() > 0)
                        candidates.push_back({cls, confidence, box});
                }
        }
        return nms(std::move(candidates), nms_threshold);
    }
    static void draw(cv::Mat &image, const std::vector<Detection> &detections) {
        for (auto &d : detections) {
            cv::rectangle(image, d.box, {0, 255, 0}, 2);
            cv::putText(
                image, std::to_string(d.class_id) + ":" + std::to_string(d.confidence).substr(0, 4),
                d.box.tl(), cv::FONT_HERSHEY_SIMPLEX, .5, {0, 255, 0}, 1);
        }
    }
};
