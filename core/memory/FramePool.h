#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <stdexcept>
using FrameClock=std::chrono::steady_clock;
struct Frame {
 std::vector<uint8_t> bytes; // packed YUYV, immutable after publication
 uint64_t sequence=0, generation=0;
 FrameClock::time_point captured;
};
using FramePtr=std::shared_ptr<const Frame>;
class FramePool {
 struct State {
  std::mutex mutex;
  std::vector<std::unique_ptr<Frame>> frames;
  std::vector<Frame*> free;
 };
 std::shared_ptr<State> s_=std::make_shared<State>();
public:
 FramePool(size_t count,size_t bytes) {
  if(!count || !bytes) throw std::invalid_argument("invalid pool size");
  s_->free.reserve(count);
  for(size_t i=0;i<count;++i) {
   auto f=std::make_unique<Frame>(); f->bytes.resize(bytes);
   s_->free.push_back(f.get()); s_->frames.push_back(std::move(f));
  }
 }
 std::shared_ptr<Frame> acquire() {
  auto s=s_; std::lock_guard<std::mutex> l(s->mutex);
  if(s->free.empty()) return {};
  auto f=s->free.back(); s->free.pop_back();
  return std::shared_ptr<Frame>(f,[s](Frame* p){std::lock_guard<std::mutex> l(s->mutex);s->free.push_back(p);});
 }
 size_t available() const { std::lock_guard<std::mutex> l(s_->mutex);return s_->free.size(); }
};
