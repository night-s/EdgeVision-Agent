#pragma once
#include <map>
#include <memory>
#include "Skill.h"
#include "agent/SkillManager.h"
#include "agent/actions/CaptureSkill.h"

class SkillManager {
public:
    void registerSkill(std::unique_ptr<Skill> skill) {
        skills_[skill->getName()] = std::move(skill);
    }

    // 核心执行逻辑：先检查安全规则，再执行技能
    void execute(const std::vector<Detection>& dets, cv::Mat& frame) {
        // 模拟安全规则：如果框超过 10 个，拒绝执行
        if (dets.size() > 10) {
            std::cout << "[SAFETY] Too many detections! Blocked." << std::endl;
            return;
        }

        for (auto& [name, skill] : skills_) {
            if (skill->execute(dets, frame)) break;
        }
    }

    // 根据技能名称触发执行
    bool executeSkillByName(const std::string& name, const std::vector<Detection>& dets, cv::Mat& frame){
        if (skills_.find(name) != skills_.end()) {
            auto it = skills_.find(name);
            if (it != skills_.end()) {
                // 找到技能，执行它，并返回执行结果 (true/false)
                return it->second->execute(dets, frame);
            }
        }
        // 未找到该技能，返回 false
        return false;
    }
private:
    std::map<std::string, std::unique_ptr<Skill>> skills_;
};