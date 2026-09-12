#pragma once
#include "core/ipc/BoundedQueue.h"
#include "core/metrics/Metrics.h"
#include <atomic>
#include <thread>
#include <fstream>
#include <filesystem>
#include <deque>
#include <memory>
struct EncodedPacket {
 std::vector<uint8_t> bytes;
 uint64_t pts=0;
 bool key=false,gap=false;
};
using PacketPtr=std::shared_ptr<EncodedPacket>;
class EventRecorder {
 BoundedQueue<PacketPtr> queue_{120};
 Metrics& metrics_;std::string directory_;
 uint64_t pre_ns_,tail_ns_;
 std::atomic<bool> running_{false},gap_{false};
 std::atomic<uint64_t> trigger_until_{0};
 std::thread thread_;
 std::atomic<uint64_t> reset_pts_{0};
 int inactivity_ms_;
 mutable std::mutex status_mutex_;
 nlohmann::json current_={{"state","idle"}};
 std::deque<nlohmann::json> history_;
 void publish(nlohmann::json state,bool final=false){
  std::lock_guard<std::mutex> lock(status_mutex_);current_=std::move(state);
  if(final){history_.push_back(current_);if(history_.size()>32)history_.pop_front();}
 }
public:
 EventRecorder(Metrics& m,std::string dir,int pre,int tail,int inactivity_ms=2000):
  metrics_(m),directory_(std::move(dir)),pre_ns_(uint64_t(pre)*1000000000),tail_ns_(uint64_t(tail)*1000000000),inactivity_ms_(inactivity_ms){}
 ~EventRecorder(){stop();}
 void start(){std::filesystem::create_directories(directory_);running_=true;thread_=std::thread([this]{run();});}
 void reset(uint64_t pts){reset_pts_=pts;}
 nlohmann::json status()const{
  std::lock_guard<std::mutex> lock(status_mutex_);
  return {{"current",current_},{"recent",history_}};
 }
 void trigger(uint64_t pts){
  if(pts<reset_pts_.load())return;
  auto until=trigger_until_.load();
  while(until<pts+tail_ns_ && !trigger_until_.compare_exchange_weak(until,pts+tail_ns_)){}
 }
 void submit(PacketPtr packet){
  packet->gap=gap_.exchange(false);
  if(!queue_.tryPush(std::move(packet))){gap_=true;metrics_.increment("record_packets_dropped");}
 }
 void stop(){running_=false;queue_.close(false);if(thread_.joinable())thread_.join();}
private:
 void run(){
  std::deque<PacketPtr> ring;size_t bytes=0;
  std::ofstream file;std::string path;uint64_t start=0,last=0,event_id=0,handled_until=0;
  uint64_t cutoff=0;
  auto received=std::chrono::steady_clock::now();
  auto finish=[&](const std::string& reason){
   if(!file.is_open())return;
   file.close();
   const bool success=!file.fail();
   std::ofstream manifest(path+".json");
   nlohmann::json result={{"start_pts_ns",start},{"end_pts_ns",last},{"reason",reason},
     {"format","Annex-B H264"},{"complete",reason=="tail_complete" && success},
     {"write_ok",success},{"event_id",std::filesystem::path(path).stem().string()},{"path",path}};
   manifest<<result.dump(2);
   manifest.close();
   if(success && !manifest.fail())metrics_.increment("events_written");
   else metrics_.increment("record_errors");
   result["manifest_ok"]=!manifest.fail();
   result["state"]=!success || manifest.fail()?"error":reason=="tail_complete"?"completed":"incomplete";
   publish(std::move(result),true);
   handled_until=std::max(handled_until,trigger_until_.load());
   file.clear();
  };
  while(true){
   PacketPtr p;bool available=queue_.pop(p,100);
   uint64_t requested_cutoff=reset_pts_.load();
   if(requested_cutoff>cutoff){
    finish("capture_restart");ring.clear();bytes=0;cutoff=requested_cutoff;
    handled_until=std::max(handled_until,cutoff+tail_ns_);
   }
   if(!available){
    if(std::chrono::steady_clock::now()-received>=std::chrono::milliseconds(inactivity_ms_)){
     finish("input_timeout");ring.clear();bytes=0;
    }
    if(queue_.closed())break;
    continue;
   }
   if(p->pts<cutoff)continue;
   received=std::chrono::steady_clock::now();
   if(p->gap){finish("encoded_packet_gap");ring.clear();bytes=0;}
   if(p->key || !ring.empty()){ring.push_back(p);bytes+=p->bytes.size();}
   // Retain the GOP containing the requested pre-event start.
   while(ring.size()>1){
    auto next=std::find_if(std::next(ring.begin()),ring.end(),[](auto& q){return q->key;});
    if(next==ring.end() || (*next)->pts+pre_ns_>=p->pts)break;
    size_t count=size_t(std::distance(ring.begin(),next));
    while(count--){bytes-=ring.front()->bytes.size();ring.pop_front();}
   }
   if(bytes>16*1024*1024){ring.clear();bytes=0;metrics_.increment("record_ring_resets");}
   auto until=trigger_until_.load();
   if(!file.is_open() && until>p->pts && until>handled_until && !ring.empty()){
    path=directory_+"/event-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+
      "-"+std::to_string(++event_id)+".h264";
    file.open(path,std::ios::binary);handled_until=until;
    if(!file){metrics_.increment("record_errors");publish({{"state","error"},{"reason","open_failed"},{"path",path}},true);file.clear();continue;}
    publish({{"state","recording"},{"event_id",std::filesystem::path(path).stem().string()},{"path",path}});
    start=ring.front()->pts;
    for(auto& q:ring)file.write(reinterpret_cast<const char*>(q->bytes.data()),q->bytes.size());
    last=p->pts;
   }else if(file.is_open()){
    file.write(reinterpret_cast<const char*>(p->bytes.data()),p->bytes.size());last=p->pts;
   }
   if(file.is_open() && !file){metrics_.increment("record_errors");finish("write_error");}
   if(file.is_open() && p->pts>=until)finish("tail_complete");
  }
  finish("shutdown");
 }
};
