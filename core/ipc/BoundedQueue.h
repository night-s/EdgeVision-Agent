#pragma once
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
template<class T> class BoundedQueue {
public:
 explicit BoundedQueue(size_t n): capacity_(n) { if(!n) throw std::invalid_argument("zero queue capacity"); }
 bool pushLatest(T item) {
  std::lock_guard<std::mutex> l(m_);
  if(closed_) return false;
  if(q_.size()==capacity_) { q_.pop_front(); ++dropped_; }
  q_.push_back(std::move(item)); high_=std::max(high_,q_.size()); cv_.notify_one(); return true;
 }
 bool tryPush(T item) {
  std::lock_guard<std::mutex> l(m_);
  if(closed_ || q_.size()==capacity_) return false;
  q_.push_back(std::move(item)); high_=std::max(high_,q_.size()); cv_.notify_one(); return true;
 }
 bool pop(T& item,int ms=200) {
  std::unique_lock<std::mutex> l(m_);
  cv_.wait_for(l,std::chrono::milliseconds(ms),[&]{return closed_ || !q_.empty();});
  if(q_.empty()) return false;
  item=std::move(q_.front()); q_.pop_front(); return true;
 }
 bool popLatest(T& item,int ms=200) {
  std::unique_lock<std::mutex> l(m_);
  cv_.wait_for(l,std::chrono::milliseconds(ms),[&]{return closed_ || !q_.empty();});
  if(q_.empty())return false;
  dropped_+=q_.size()-1;item=std::move(q_.back());q_.clear();return true;
 }
 void clear() { std::lock_guard<std::mutex> l(m_); q_.clear(); }
 // Drain is used by disk workers; real-time queues discard obsolete work.
 void close(bool discard=true) { std::lock_guard<std::mutex> l(m_); closed_=true; if(discard) q_.clear(); cv_.notify_all(); }
 bool closed() const { std::lock_guard<std::mutex> l(m_); return closed_; }
 size_t size() const { std::lock_guard<std::mutex> l(m_); return q_.size(); }
 size_t dropped() const { std::lock_guard<std::mutex> l(m_); return dropped_; }
 size_t highWatermark() const { std::lock_guard<std::mutex> l(m_); return high_; }
private:
 size_t capacity_,dropped_=0,high_=0; bool closed_=false;
 mutable std::mutex m_; std::condition_variable cv_; std::deque<T> q_;
};
