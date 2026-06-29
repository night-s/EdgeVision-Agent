#ifndef SAFE_QUEUE_H
#define SAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class SafeQueue {
public:
    SafeQueue(size_t capacity = 5) : capacity_(capacity) {}

    void push(T item) {
        std::unique_lock<std::mutex> lock(mtx_);
        // 如果队列满了，丢弃最老的一帧（这是工业界处理慢消费的常见策略）
        if (queue_.size() >= capacity_) {
            queue_.pop();
        }
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    bool pop(T& item, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); })) {
            return false; // 超时未获取到数据
        }
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

private:
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    size_t capacity_;
};

#endif // SAFE_QUEUE_H