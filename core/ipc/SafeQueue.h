#ifndef SAFE_QUEUE_H
#define SAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

template <typename T>
class SafeQueue {
public:
    SafeQueue(size_t capacity = 5) : capacity_(capacity) {}

    // 设置丢弃旧数据时的回调函数（用于归还内存池）
    void setDropCallback(std::function<void(const T&)> cb) {
        drop_cb_ = cb;
    }

    void push(T item) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (queue_.size() >= capacity_) {
            // 队列满了，准备丢弃最老的一帧
            T old_item = std::move(queue_.front());
            queue_.pop();
            // 如果设置了回调，把内存还给内存池！
            if (drop_cb_) {
                drop_cb_(old_item);
            }
        }
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    bool pop(T& item, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); })) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // 新增：获取当前队列大小，方便生产者判断背压
    int size() {
        std::unique_lock<std::mutex> lock(mtx_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    size_t capacity_;
    std::function<void(const T&)> drop_cb_; // 丢弃回调
};

#endif // SAFE_QUEUE_H