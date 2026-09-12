#pragma once
#include "third_party/nlohmann_json.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

// One recorder owns this directory. Only finalized, recognized event pairs are
// eligible for deletion; unrecognized files, symlinks and the active file survive.
struct StorageLimits {
    uint64_t bytes = 256ULL * 1024 * 1024;
    size_t files = 128;
    uint64_t free_bytes = 64ULL * 1024 * 1024;
    int event_seconds = 60;
};
class EventStorage {
    std::filesystem::path directory_;
    StorageLimits limits_;

  public:
    EventStorage(std::string directory, StorageLimits limits)
        : directory_(std::move(directory)), limits_(limits) {}
    bool reserve(uint64_t incoming, const std::string &active, bool opening) {
        namespace fs = std::filesystem;
        struct Item {
            fs::path path;
            uint64_t size;
            fs::file_time_type time;
            bool removable;
        };
        if (fs::is_symlink(directory_))
            return false;
        std::vector<Item> items;
        uint64_t used = 0;
        static const std::regex name("^event-[0-9]+-[0-9]+\\.h264$");
        for (const auto &entry : fs::directory_iterator(directory_)) {
            if (!std::regex_match(entry.path().filename().string(), name))
                continue;
            if (entry.is_symlink() || !entry.is_regular_file())
                continue;
            auto manifest = fs::path(entry.path().string() + ".json");
            uint64_t size = entry.file_size();
            bool removable = false;
            if (!fs::is_symlink(manifest) && fs::is_regular_file(manifest)) {
                size += fs::file_size(manifest);
                std::ifstream in(manifest);
                auto data = nlohmann::json::parse(in, nullptr, false);
                removable = data.is_object() &&
                            data.value("format", std::string{}) == "Annex-B H264" &&
                            data.value("write_ok", false) && data.value("complete", false) &&
                            data.contains("reason");
            }
            used += size;
            items.push_back({entry.path(), size, entry.last_write_time(),
                             removable && entry.path().string() != active});
        }
        size_t count = items.size() + (opening ? 1 : 0);
        std::sort(items.begin(), items.end(),
                  [](const auto &a, const auto &b) { return a.time < b.time; });
        auto enough = [&] {
            return incoming <= limits_.bytes && used <= limits_.bytes - incoming &&
                   count <= limits_.files &&
                   fs::space(directory_).available >= limits_.free_bytes + incoming;
        };
        for (const auto &item : items) {
            if (enough())
                break;
            if (!item.removable)
                continue;
            // Scope is one directly enumerated file and its same-directory manifest.
            if (fs::remove(item.path)) {
                fs::remove(item.path.string() + ".json");
                used -= item.size;
                --count;
            }
        }
        return enough();
    }
    int maxSeconds() const {
        return limits_.event_seconds;
    }
};
