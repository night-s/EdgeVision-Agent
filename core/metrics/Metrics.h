#pragma once
#include "third_party/nlohmann_json.hpp"
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <unistd.h>
#include <cmath>
#include <sys/resource.h>
class Metrics {
 using Clock=std::chrono::steady_clock;
 mutable std::mutex m_;
 std::map<std::string,uint64_t> counts_;
 std::map<std::string,std::deque<double>> samples_;
 Clock::time_point start_=Clock::now(),last_=start_;
 double last_cpu_=0;
 nlohmann::json cached_rates_;
 uint64_t last_capture_=0,last_process_=0,last_encode_=0;
public:
 void increment(const std::string& key,uint64_t n=1){std::lock_guard<std::mutex> l(m_);counts_[key]+=n;}
 void observe(const std::string& key,double value){
  std::lock_guard<std::mutex> l(m_);auto& s=samples_[key];
  if(s.size()==1024) s.pop_front();
  s.push_back(value);
 }
 nlohmann::json snapshot(){
  std::lock_guard<std::mutex> l(m_);auto now=Clock::now();
  double elapsed=std::chrono::duration<double>(now-last_).count();
  nlohmann::json j;
  j["uptime_s"]=std::chrono::duration<double>(now-start_).count();
  j["counts"]=counts_;j["sample_window"]="latest 1024 observations per stage";
  if(elapsed>=1.0 || cached_rates_.is_null()){
   j["capture_fps"]=double(counts_["captured"]-last_capture_)/elapsed;
   j["process_fps"]=double(counts_["processed"]-last_process_)/elapsed;
   j["encode_fps"]=double(counts_["encoded"]-last_encode_)/elapsed;

  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  double cpu=usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
  j["cpu_percent_one_core"]=elapsed>0?100*(cpu-last_cpu_)/elapsed:0;last_cpu_=cpu;
  last_=now;last_capture_=counts_["captured"];last_process_=counts_["processed"];last_encode_=counts_["encoded"];
  cached_rates_={{"capture_fps",j["capture_fps"]},{"process_fps",j["process_fps"]},
    {"encode_fps",j["encode_fps"]},{"cpu_percent_one_core",j["cpu_percent_one_core"]},
    {"rate_window_s",elapsed}};
  }
  j.update(cached_rates_);
  for(auto& [name,s]:samples_){
   if(s.empty())continue;
   std::vector<double> sorted(s.begin(),s.end());std::sort(sorted.begin(),sorted.end());
   auto percentile=[&](double p){return sorted[std::min(sorted.size()-1,size_t(std::ceil(p*sorted.size()))-1)];};
   j["stages_ms"][name]={{"count",s.size()},{"average",std::accumulate(s.begin(),s.end(),0.)/s.size()},
    {"p50",percentile(.5)},{"p95",percentile(.95)},{"maximum",sorted.back()}};
  }
  long pages=0,resident=0;std::ifstream stat("/proc/self/statm");stat>>pages>>resident;
  j["rss_mb"]=double(resident)*sysconf(_SC_PAGESIZE)/1048576.;
  return j;
 }
};
