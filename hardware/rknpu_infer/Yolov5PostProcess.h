#ifndef YOLOV5_POST_PROCESS_H
#define YOLOV5_POST_PROCESS_H

#include "rknn_api.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <cmath>
#include <algorithm>

struct Detection {
    int class_id;
    float confidence;
    cv::Rect box;
};

class Yolov5PostProcess {
public:
    Yolov5PostProcess() {
        // COCO 80 个类别名称（完整版）
        labels_ = {"person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
                   "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
                   "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
                   "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
                   "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
                   "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
                   "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
                   "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
                   "hair drier", "toothbrush"};
    }

    float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

    // 计算两个框的交并比 (IoU)
    float iou(const Detection& a, const Detection& b) {
        float inter_x1 = std::max(a.box.x, b.box.x);
        float inter_y1 = std::max(a.box.y, b.box.y);
        float inter_x2 = std::min(a.box.x + a.box.width, b.box.x + b.box.width);
        float inter_y2 = std::min(a.box.y + a.box.height, b.box.y + b.box.height);
        float inter_area = std::max(0.0f, inter_x2 - inter_x1) * std::max(0.0f, inter_y2 - inter_y1);
        float union_area = a.box.area() + b.box.area() - inter_area;
        if (union_area == 0) return 0;
        return inter_area / union_area;
    }

    // 非极大值抑制 (NMS) 去重叠
    std::vector<Detection> nms(const std::vector<Detection>& detections, float iou_threshold = 0.1) {
        std::vector<Detection> result;
        if (detections.empty()) return result;

        // 按置信度从高到低排序
        std::vector<Detection> sorted_dets = detections;
        std::sort(sorted_dets.begin(), sorted_dets.end(), 
                  [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });

        std::vector<bool> removed(sorted_dets.size(), false);
        for (size_t i = 0; i < sorted_dets.size(); ++i) {
            if (removed[i]) continue;
            result.push_back(sorted_dets[i]);
            for (size_t j = i + 1; j < sorted_dets.size(); ++j) {
                if (!removed[j] && sorted_dets[i].class_id == sorted_dets[j].class_id) {
                    if (iou(sorted_dets[i], sorted_dets[j]) > iou_threshold) {
                        removed[j] = true;
                    }
                }
            }
        }
        return result;
    }

    std::vector<Detection> process(rknn_output* outputs, int orig_w, int orig_h) {
        std::vector<Detection> candidates;
        int strides[3] = {8, 16, 32};
        int grid_sizes[3] = {80, 40, 20};
        float conf_threshold = 0.25f; // 阈值降到 0.25

        float max_confidence_seen = 0.0f; // 用来记录本帧出现的最高置信度

        for (int idx = 0; idx < 3; ++idx) {
            float* data = (float*)outputs[idx].buf;
            int grid_h = grid_sizes[idx];
            int grid_w = grid_sizes[idx];
            int stride = strides[idx];
            int hw = grid_h * grid_w;

            for (int gy = 0; gy < grid_h; ++gy) {
                for (int gx = 0; gx < grid_w; ++gx) {
                    for (int a = 0; a < 3; ++a) {
                        int offset = (a * 85 + 0) * hw + gy * grid_w + gx; 

                        float tx = sigmoid(data[offset]);
                        float ty = sigmoid(data[offset + 1 * hw]);
                        float tw = sigmoid(data[offset + 2 * hw]);
                        float th = sigmoid(data[offset + 3 * hw]);
                        float box_conf = sigmoid(data[offset + 4 * hw]);
                        
                        // 记录当前帧遇到的最大置信度
                        if (box_conf > max_confidence_seen) {
                            max_confidence_seen = box_conf;
                        }

                        if (box_conf < conf_threshold) continue;

                        float max_cls_score = 0;
                        int max_cls_id = 0;
                        for (int c = 5; c < 85; ++c) {
                            float cls_score = sigmoid(data[offset + c * hw]);
                            if (cls_score > max_cls_score) {
                                max_cls_score = cls_score;
                                max_cls_id = c - 5;
                            }
                        }

                        float final_conf = box_conf * max_cls_score;
                        // 记录综合置信度
                        if (final_conf > max_confidence_seen) {
                            max_confidence_seen = final_conf;
                        }

                        if (final_conf < conf_threshold) continue;

                        // ... 后续画框代码保持不变 ...
                        float cx = (tx * 2.0f - 0.5f + gx) * stride;
                        float cy = (ty * 2.0f - 0.5f + gy) * stride;
                        float w  = std::pow(tw * 2.0f, 2) * stride;
                        float h  = std::pow(th * 2.0f, 2) * stride;

                        Detection det;
                        det.class_id = max_cls_id;
                        det.confidence = final_conf;
                        det.box.x = std::max(0, (int)((cx - w/2) * orig_w / 640.0f));
                        det.box.y = std::max(0, (int)((cy - h/2) * orig_h / 640.0f));
                        det.box.width = std::min(orig_w - det.box.x, (int)(w * orig_w / 640.0f));
                        det.box.height = std::min(orig_h - det.box.y, (int)(h * orig_h / 640.0f));
                        
                        candidates.push_back(det);
                    }
                }
            }
        }

        // 诊断输出：
        std::cout << "[DEBUG] Max confidence seen this frame: " << max_confidence_seen << std::endl;
        
        return nms(candidates);
    }

    void draw(cv::Mat& img, const std::vector<Detection>& dets) {
        for (auto& d : dets) {
            cv::rectangle(img, d.box, cv::Scalar(0, 255, 0), 2);
            std::string label = labels_[d.class_id] + ": " + std::to_string(d.confidence).substr(0, 4);
            cv::putText(img, label, cv::Point(d.box.x, d.box.y - 5), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
        }
    }

private:
    std::vector<std::string> labels_;
};

#endif // YOLOV5_POST_PROCESS_H