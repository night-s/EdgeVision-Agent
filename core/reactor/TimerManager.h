#ifndef TIMER_MANAGER_H
#define TIMER_MANAGER_H

#include <sys/timerfd.h>
#include <unistd.h>
#include <chrono>
#include <functional>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <cerrno>

/**
 * TimerManager — 基于 timerfd + 最小堆的软实时定时器管理
 *
 * 设计要点：
 *  - timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK) 内核定时器 fd
 *  - std::vector 最小堆，按到期时间排序 (std::push_heap / pop_heap)
 *  - O(log n) addTimer, O(n) cancelTimer (惰性删除)
 *  - 非线程安全 —— 所有操作必须在 EventLoop 线程内完成
 */
class TimerManager {
public:
    using TimerCallback = std::function<void()>;

    TimerManager()
        : timerfd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK))
        , next_id_(1)
    {
        if (timerfd_ < 0) {
            std::cerr << "[TimerManager] timerfd_create failed: "
                      << std::strerror(errno) << std::endl;
        }
    }

    ~TimerManager() {
        if (timerfd_ >= 0) ::close(timerfd_);
    }

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    /// 添加定时器
    /// @param delay_ms  首次触发延迟 (毫秒)
    /// @param period_ms 重复周期 (0 = 一次性)
    /// @param cb        回调函数
    /// @return 定时器 ID (可用来取消)
    uint64_t addTimer(uint64_t delay_ms, uint64_t period_ms, TimerCallback cb) {
        if (timerfd_ < 0) return 0;

        auto now = std::chrono::steady_clock::now();
        auto expiry = now + std::chrono::milliseconds(delay_ms);

        TimerEntry entry{next_id_++, expiry, period_ms, std::move(cb), true};
        heap_.push_back(std::move(entry));
        std::push_heap(heap_.begin(), heap_.end(), TimerComparer{});

        // 如果新定时器是最早到期的，重设 timerfd
        if (heap_.front().id == next_id_ - 1) {
            resetTimerFd();
        }

        return next_id_ - 1;
    }

    /// 取消定时器（惰性删除，标记 active = false）
    bool cancelTimer(uint64_t id) {
        for (auto& e : heap_) {
            if (e.id == id && e.active) {
                e.active = false;
                return true;
            }
        }
        return false;
    }

    /// 获取 timerfd（用于 EventLoop 注册到 epoll）
    int getTimerFd() const { return timerfd_; }

    /// 处理到期的定时器 —— 由 EventLoop 在 timerfd 可读时调用
    void handleTimerEvent() {
        if (timerfd_ < 0) return;

        // 读取 timerfd 清除通知
        uint64_t expirations;
        if (::read(timerfd_, &expirations, sizeof(expirations)) != sizeof(expirations)) {
            // EAGAIN 说明被别处处理了，忽略
            return;
        }

        auto now = std::chrono::steady_clock::now();

        while (!heap_.empty()) {
            auto& top = heap_.front();
            if (top.expiry > now) break;  // 第一个都还没到，退出

            // pop 堆顶
            std::pop_heap(heap_.begin(), heap_.end(), TimerComparer{});
            TimerEntry entry = std::move(heap_.back());
            heap_.pop_back();

            if (entry.active) {
                // 执行回调
                if (entry.callback) {
                    entry.callback();
                }

                // 重复定时器：计算下次到期时间，重新入堆
                if (entry.period_ms > 0) {
                    entry.expiry = now + std::chrono::milliseconds(entry.period_ms);
                    heap_.push_back(std::move(entry));
                    std::push_heap(heap_.begin(), heap_.end(), TimerComparer{});
                }
            }
            // 惰性删除：active == false 直接丢弃
        }

        // 重新设置 timerfd 到最近到期时间
        resetTimerFd();
    }

private:
    struct TimerEntry {
        uint64_t id;
        std::chrono::steady_clock::time_point expiry;
        uint64_t period_ms;               // 0 = 一次性
        TimerCallback callback;
        bool active = true;
    };

    // 最小堆比较器：最早到期的优先
    struct TimerComparer {
        bool operator()(const TimerEntry& a, const TimerEntry& b) const {
            // std::push_heap 构建最大堆，取反实现最小堆
            return a.expiry > b.expiry;
        }
    };

    /// 重设 timerfd 到最近到期时间（或停用）
    void resetTimerFd() {
        if (timerfd_ < 0) return;

        struct itimerspec its;
        std::memset(&its, 0, sizeof(its));

        if (!heap_.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto& top = heap_.front();

            if (top.expiry > now) {
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    top.expiry - now);
                its.it_value.tv_sec  = duration.count() / 1000;
                its.it_value.tv_nsec = (duration.count() % 1000) * 1000000;
            } else {
                // 已经到期了，立即触发
                its.it_value.tv_sec  = 0;
                its.it_value.tv_nsec = 1;
            }
        }
        // else: 堆空 → its.it_value 全零 → timerfd 停用

        if (::timerfd_settime(timerfd_, 0, &its, nullptr) < 0) {
            std::cerr << "[TimerManager] timerfd_settime failed: "
                      << std::strerror(errno) << std::endl;
        }
    }

    int timerfd_;
    std::vector<TimerEntry> heap_;
    uint64_t next_id_;
};

#endif // TIMER_MANAGER_H
