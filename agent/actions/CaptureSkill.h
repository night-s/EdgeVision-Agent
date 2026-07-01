#ifndef CAPTURE_SKILL_H
#define CAPTURE_SKILL_H

#include "../Skill.h"
#include <iostream>

class CaptureSkill : public Skill {
public:
    std::string getName() const override { return "CaptureSkill"; }
    
    bool execute(const std::vector<Detection>& dets, cv::Mat& frame) override {
        for (auto& d : dets) {
            // 触发条件：如果是人(0)、自行车(1)、车(2)，且置信度大于 0.35
            if ((d.class_id == 0 || d.class_id == 1 || d.class_id == 2) && d.confidence > 0.35) {
                cv::imwrite("event_capture.jpg", frame);
                std::cout << "[SKILL EXEC] CaptureSkill triggered!" << std::endl;
                return true;
            }
        }
        return false;
    }
};

#endif