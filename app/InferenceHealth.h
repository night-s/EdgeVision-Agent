#pragma once
#include "third_party/nlohmann_json.hpp"
#include <chrono>
#include <mutex>
// Service time excludes input starvation and queue wait. Only the inference
// worker owns RKNN; the observer must never destroy an in-flight context.
class InferenceHealth {
 using Clock=std::chrono::steady_clock;
 mutable std::mutex mutex_;
 bool active_=false,degraded_=false;
 unsigned slow_=0,fast_=0;
 double last_ms_=0;
 Clock::time_point started_{};
 int slow_ms_,stall_ms_;
public:
 InferenceHealth(int slow,int stall):slow_ms_(slow),stall_ms_(stall){}
 void begin(Clock::time_point now=Clock::now()){
  std::lock_guard<std::mutex> l(mutex_);active_=true;started_=now;
 }
 void end(double ms){
  std::lock_guard<std::mutex> l(mutex_);active_=false;last_ms_=ms;
  if(ms>=slow_ms_){fast_=0;if(++slow_>=3)degraded_=true;}
  else if(ms<slow_ms_*0.75){slow_=0;if(++fast_>=10)degraded_=false;}
  else {slow_=0;fast_=0;}
 }
 void fail(){std::lock_guard<std::mutex> l(mutex_);active_=false;slow_=fast_=0;}
 bool degraded()const {std::lock_guard<std::mutex> l(mutex_);return degraded_;}
 nlohmann::json snapshot(Clock::time_point now=Clock::now())const{
  std::lock_guard<std::mutex> l(mutex_);
  double age=active_?std::chrono::duration<double,std::milli>(now-started_).count():0;
  return {{"state",age>=stall_ms_?"stalled":degraded_?"degraded":"normal"},
   {"active",active_},{"active_ms",age},{"last_service_ms",last_ms_},
   {"slow_streak",slow_},{"recovery_streak",fast_}};
 }
};
