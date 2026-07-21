#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "TimerManager.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <iostream>
#include <cstring>
#include <cerrno>

/**
 * EventLoop — 基于 epoll 的单线程 Reactor 事件循环
 *
 * 职责：
 *  - epoll_wait 分派 fd 事件
 *  - 集成 TimerManager（timerfd 自动注册到 epoll）
 *  - eventfd 跨线程唤醒（queueInLoop / runInLoop）
 *  - Level-triggered 模式，简单可靠
 *
 * 线程模型：
 *  - 所有 fd 操作、定时器操作必须在该线程内执行
 *  - 跨线程任务通过 queueInLoop() 投递
 */
class EventLoop {
public:
    using EventCallback = std::function<void(uint32_t revents)>;

    static constexpr int kMaxEvents = 64;

    EventLoop()
        : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC))
        , wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
        , running_(false)
    {
        if (epoll_fd_ < 0) {
            std::cerr << "[EventLoop] epoll_create1 failed: "
                      << std::strerror(errno) << std::endl;
            return;
        }
        if (wakeup_fd_ < 0) {
            std::cerr << "[EventLoop] eventfd failed: "
                      << std::strerror(errno) << std::endl;
            return;
        }

        // 将 wakeup_fd 注册到 epoll
        addFd(wakeup_fd_, EPOLLIN, [this](uint32_t) { handleWakeup(); });

        // 将 TimerManager 的 timerfd 注册到 epoll（如果创建成功）
        int tfd = timer_mgr_.getTimerFd();
        if (tfd >= 0) {
            addFd(tfd, EPOLLIN, [this](uint32_t) {
                timer_mgr_.handleTimerEvent();
            });
        }
    }

    ~EventLoop() {
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
        if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // ========== 循环控制 ==========

    /// 进入主循环（阻塞，直到 stop() 被调用）
    void run() {
        loop_tid_ = std::this_thread::get_id();
        running_ = true;

        while (running_) {
            int nfds = ::epoll_wait(epoll_fd_, events_, kMaxEvents, -1);
            if (nfds < 0) {
                if (errno == EINTR) continue;  // 信号中断，重试
                std::cerr << "[EventLoop] epoll_wait error: "
                          << std::strerror(errno) << std::endl;
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events_[i].data.fd;
                auto it = fd_ctxs_.find(fd);
                if (it != fd_ctxs_.end()) {
                    it->second.callback(events_[i].events);
                }
            }

            doPendingFunctors();
        }

        running_ = false;
    }

    /// 停止事件循环（线程安全）
    void stop() {
        running_ = false;
        uint64_t one = 1;
        if (::write(wakeup_fd_, &one, sizeof(one)) < 0) {
            // 忽略 EAGAIN
        }
    }

    // ========== fd 管理 ==========

    /// 注册 fd 到 epoll（LT 模式）
    bool addFd(int fd, uint32_t events, EventCallback cb) {
        if (epoll_fd_ < 0) return false;

        fd_ctxs_[fd] = FdCtx{events, std::move(cb)};

        struct epoll_event ev;
        ev.events   = events;
        ev.data.fd  = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::cerr << "[EventLoop] epoll_ctl ADD failed for fd "
                      << fd << ": " << std::strerror(errno) << std::endl;
            fd_ctxs_.erase(fd);
            return false;
        }
        return true;
    }

    /// 修改 fd 监听的事件
    bool updateFd(int fd, uint32_t events) {
        auto it = fd_ctxs_.find(fd);
        if (it == fd_ctxs_.end()) return false;
        it->second.events = events;

        struct epoll_event ev;
        ev.events   = events;
        ev.data.fd  = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            std::cerr << "[EventLoop] epoll_ctl MOD failed for fd "
                      << fd << ": " << std::strerror(errno) << std::endl;
            return false;
        }
        return true;
    }

    /// 从 epoll 移除 fd
    void removeFd(int fd) {
        fd_ctxs_.erase(fd);
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    // ========== 跨线程任务投递 ==========

    /// 在 EventLoop 线程中执行回调
    /// - 如果在 EventLoop 线程中调用，直接同步执行
    /// - 如果在其他线程中调用，异步排队
    void runInLoop(std::function<void()> cb) {
        if (isInLoopThread()) {
            cb();
        } else {
            queueInLoop(std::move(cb));
        }
    }

    /// 将回调投递到 EventLoop 的任务队列（始终异步）
    void queueInLoop(std::function<void()> cb) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_functors_.push_back(std::move(cb));
        }

        // 如果不是在 EventLoop 线程，或 EventLoop 正在执行 pending functors，
        // 需要唤醒 epoll_wait
        if (!isInLoopThread() || calling_pending_functors_) {
            uint64_t one = 1;
            if (::write(wakeup_fd_, &one, sizeof(one)) < 0) {
                // 忽略 EAGAIN
            }
        }
    }

    // ========== 查询 ==========

    bool isInLoopThread() const {
        return std::this_thread::get_id() == loop_tid_;
    }

    TimerManager& timerMgr() { return timer_mgr_; }

private:
    // 每个 fd 的上下文
    struct FdCtx {
        uint32_t events = 0;
        EventCallback callback;
    };

    /// 处理 eventfd 唤醒
    void handleWakeup() {
        uint64_t one;
        if (::read(wakeup_fd_, &one, sizeof(one)) < 0) {
            // EAGAIN 说明没有待处理的通知，忽略
        }
    }

    /// 执行所有待处理的跨线程任务
    void doPendingFunctors() {
        std::vector<std::function<void()>> functors;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            functors.swap(pending_functors_);
        }

        calling_pending_functors_ = true;
        for (auto& f : functors) {
            f();
        }
        calling_pending_functors_ = false;
    }

    int epoll_fd_;
    int wakeup_fd_;
    std::atomic<bool> running_;
    std::thread::id loop_tid_;

    TimerManager timer_mgr_;

    std::unordered_map<int, FdCtx> fd_ctxs_;
    struct epoll_event events_[kMaxEvents];

    std::mutex pending_mutex_;
    std::vector<std::function<void()>> pending_functors_;
    bool calling_pending_functors_ = false;
};

#endif // EVENT_LOOP_H
