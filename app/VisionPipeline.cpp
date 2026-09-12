#include "VisionPipeline.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <cmath>
VisionPipeline::VisionPipeline(Config c):config_(std::move(c)),
 pool_(config_.pool,size_t(config_.width)*config_.height*2),inference_(config_.queue),
 inference_health_(config_.infer_slow_ms,config_.infer_stall_ms),infer_delay_ms_(config_.infer_delay_ms),
 preprocess_(config_.width,config_.height,config_.preprocess=="rga"),
 server_(loop_),threshold_(config_.threshold){}
VisionPipeline::~VisionPipeline(){stop();}
void VisionPipeline::start(){
 std::filesystem::create_directories(config_.output);
 metrics_file_.open(config_.output+"/metrics.jsonl",std::ios::app);
 if(!metrics_file_)throw std::runtime_error("cannot create metrics file");
 engine_.loadModel(config_.model);
 if(config_.video){
  video_=std::make_unique<VideoService>(config_,metrics_,origin_);video_->start();
 }
 server_.setMessageCallback([this](uint64_t id,const std::string& line){command(id,line);});
 if(!server_.start(config_.port))throw std::runtime_error("cannot bind control port");
 loop_.timerMgr().addTimer(1000,5000,[this]{publishMetrics();});
 running_=true;
 network_thread_=std::thread([this]{
  try{loop_.run();}catch(const std::exception& e){std::cerr<<"[network] "<<e.what()<<std::endl;}
  running_=false;
 });
 io_thread_=std::thread([this]{snapshotLoop();});
 infer_thread_=std::thread([this]{inferenceLoop();});
 capture_thread_=std::thread([this]{captureLoop();});
}
void VisionPipeline::captureLoop(){
 V4L2Camera camera(config_.device,config_.width,config_.height,config_.fps);
 bool opened=false;int failures=0;uint64_t sequence=0;
 auto next_retry=FrameClock::now(),next_synthetic=FrameClock::now();
 try{
  std::vector<uint8_t> replay;
  if(!config_.replay_yuyv.empty()){
   replay.resize(size_t(config_.width)*config_.height*2);
   std::ifstream file(config_.replay_yuyv,std::ios::binary);
   if(!file.read(reinterpret_cast<char*>(replay.data()),replay.size()) || file.peek()!=EOF)
    throw std::runtime_error("replay_yuyv must contain exactly one packed YUYV frame");
  }
  while(running_){
   if(restart_capture_.exchange(false)){
    camera.close();opened=false;failures=0;next_retry=FrameClock::now();
    {std::lock_guard<std::mutex> l(state_mutex_);
     ++inference_epoch_;latest_.reset();detections_=json::array();detection_sequence_=0;
     capture_state_="recovering";}
    inference_.clear();if(video_)video_->resetRecording(FrameClock::now());metrics_.increment("camera_manual_restarts");
   }
   if(!config_.synthetic && !opened){
    if(FrameClock::now()<next_retry){std::this_thread::sleep_for(std::chrono::milliseconds(50));continue;}
    opened=camera.open() && camera.start();
    {
     std::lock_guard<std::mutex> l(state_mutex_);
     capture_state_=opened?"running":"recovering";capture_error_=opened?"":camera.error();
    }
    if(!opened){
     camera.close();metrics_.increment("camera_retries");
     next_retry=FrameClock::now()+std::chrono::seconds(2);continue;
    }
    std::cout<<"[camera] "<<config_.width<<"x"<<config_.height<<" YUYV fps="<<camera.fps()<<std::endl;
    failures=0;
   }
   auto frame=pool_.acquire();
   if(!frame){metrics_.increment("pool_exhausted");std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
   {std::lock_guard<std::mutex> l(state_mutex_);frame->generation=inference_epoch_;}
   auto copy_start=FrameClock::now();
   if(config_.synthetic){
    std::this_thread::sleep_until(next_synthetic);
    next_synthetic=std::max(next_synthetic+std::chrono::microseconds(1000000/config_.fps),FrameClock::now());
    if(!replay.empty())std::copy(replay.begin(),replay.end(),frame->bytes.begin());
    else for(size_t p=0;p<frame->bytes.size();p+=4){
     frame->bytes[p]=uint8_t(32+(sequence%160));frame->bytes[p+1]=128;
     frame->bytes[p+2]=frame->bytes[p];frame->bytes[p+3]=128;
    }
    {std::lock_guard<std::mutex> l(state_mutex_);capture_state_=replay.empty()?"synthetic":"replay";}
   }else{
    auto result=camera.copyFrame(frame->bytes.data(),frame->bytes.size());
    if(result!=V4L2Camera::Result::Frame){
     metrics_.increment(result==V4L2Camera::Result::Timeout?"camera_timeouts":"camera_errors");
     if(++failures>=5){
      {std::lock_guard<std::mutex> l(state_mutex_);capture_state_="recovering";capture_error_=camera.error().empty()?"capture timeout":camera.error();
       ++inference_epoch_;latest_.reset();detections_=json::array();detection_sequence_=0;}
      camera.close();opened=false;next_retry=FrameClock::now()+std::chrono::seconds(1);
      inference_.clear();if(video_)video_->resetRecording(FrameClock::now());
     }
     continue;
    }
    failures=0;
   }
   frame->captured=FrameClock::now();frame->sequence=++sequence;
   metrics_.observe("capture_wait_and_copy",std::chrono::duration<double,std::milli>(frame->captured-copy_start).count());
   metrics_.increment("captured");
   {std::lock_guard<std::mutex> l(state_mutex_);latest_=frame;}
   if(enabled_)inference_.pushLatest(frame);
   if(video_)video_->submit(frame);
  }
 }catch(const std::exception& e){
  std::lock_guard<std::mutex> l(state_mutex_);capture_state_="failed";capture_error_=e.what();
 }
 camera.close();
}
void VisionPipeline::inferenceLoop(){
 int confirmed=0,consecutive_errors=0;
 uint64_t generation=UINT64_MAX;
 auto next_admission=FrameClock::now();
 while(running_){
  while(running_ && FrameClock::now()<next_admission)
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if(!running_)break;
  FramePtr frame;if(!inference_.popLatest(frame))continue;
  if(frame->generation!=generation){confirmed=0;generation=frame->generation;}
  {std::lock_guard<std::mutex> l(state_mutex_);
   if(!enabled_ || frame->generation!=inference_epoch_){confirmed=0;continue;}}
  try{
   auto start=FrameClock::now();
   if(std::chrono::duration<double,std::milli>(start-frame->captured).count()>config_.infer_max_age_ms){
    confirmed=0;metrics_.increment("expired_input_frames");continue;
   }
   inference_health_.begin(start);
   metrics_.observe("queue_wait",std::chrono::duration<double,std::milli>(start-frame->captured).count());
   const auto& rgb=preprocess_.run(frame->bytes.data());
   auto prepared=FrameClock::now();
   auto result=engine_.infer(rgb,preprocess_.geometry(),threshold_,config_.nms,config_.output_activation=="logits",config_.native_outputs);
   const int delay=infer_delay_ms_.load();
   if(delay){
    for(int elapsed=0;running_ && elapsed<delay;elapsed+=10)
     std::this_thread::sleep_for(std::chrono::milliseconds(10));
   }
   auto completed=FrameClock::now();
   double service_ms=std::chrono::duration<double,std::milli>(completed-start).count();
   bool was_degraded=inference_health_.degraded();
   inference_health_.end(service_ms);
   if(was_degraded!=inference_health_.degraded())
    metrics_.increment(inference_health_.degraded()?"inference_degradations":"inference_recoveries");
   metrics_.observe("inference_service",service_ms);
   next_admission=completed+std::chrono::milliseconds(inference_health_.degraded()?config_.infer_degraded_interval_ms:0);
   consecutive_errors=0;
   metrics_.observe("preprocess",std::chrono::duration<double,std::milli>(prepared-start).count());
   metrics_.observe("input",result.input_ms);metrics_.observe("inference",result.run_ms);
   metrics_.observe("output",result.output_ms);metrics_.observe("postprocess",result.post_ms);
   metrics_.observe("capture_to_detection",std::chrono::duration<double,std::milli>(completed-frame->captured).count());
   metrics_.increment("processed");
   json data=json::array();bool person_in_roi=false;
   for(auto& d:result.detections){
    data.push_back({{"class_id",d.class_id},{"confidence",d.confidence},
      {"box",{d.box.x,d.box.y,d.box.width,d.box.height}}});
    float x=float(d.box.x+d.box.width/2)/config_.width,y=float(d.box.y+d.box.height/2)/config_.height;
    if(d.class_id==0 && x>=config_.roi[0] && y>=config_.roi[1] && x<=config_.roi[2] && y<=config_.roi[3])
     person_in_roi=true;
   }
   {
    std::lock_guard<std::mutex> l(state_mutex_);
    if(!running_ || !enabled_ || frame->generation!=inference_epoch_ ||
       std::chrono::duration<double,std::milli>(completed-frame->captured).count()>config_.infer_max_age_ms){
     confirmed=0;metrics_.increment("stale_results_discarded");continue;
    }
    detections_=std::move(data);detection_sequence_=frame->sequence;detection_time_=frame->captured;infer_error_.clear();
    confirmed=person_in_roi?std::min(confirmed+1,config_.event_confirm):0;
    if(confirmed>=config_.event_confirm && video_)video_->trigger(frame->captured);
   }
   consecutive_errors=0;

  }catch(const std::exception& e){
   inference_health_.fail();
   metrics_.increment("inference_errors");
   {std::lock_guard<std::mutex> l(state_mutex_);infer_error_=e.what();}
   std::cerr<<"[inference] "<<e.what()<<std::endl;
   confirmed=0;
   if(++consecutive_errors>=3){
    {std::lock_guard<std::mutex> l(state_mutex_);
     if(frame->generation==inference_epoch_){enabled_=false;++inference_epoch_;}}
    inference_.clear();consecutive_errors=0;
   }
  }
 }
}
json VisionPipeline::status(){
 std::lock_guard<std::mutex> l(state_mutex_);
 json j={{"capture_state",capture_state_},{"capture_error",capture_error_},
  {"inference_health",inference_health_.snapshot()},
  {"inference_enabled",enabled_.load()},{"inference_error",infer_error_},
  {"threshold",threshold_.load()},{"video_enabled",bool(video_)},
  {"video_healthy",video_?video_->healthy():false},{"video_error",video_?video_->error():""},
  {"pool_available",pool_.available()},{"inference_queue",inference_.size()},
  {"queue_high_watermark",inference_.highWatermark()},{"inference_dropped",inference_.dropped()},
  {"video_dropped",video_?video_->dropped():0}};
 if(video_)j["recorder"]=video_->recordingStatus();
 return j;
}
void VisionPipeline::cacheReply(const std::string& key,const json& reply){
 if(!replies_.count(key)){
  if(reply_order_.size()>=128){replies_.erase(reply_order_.front());reply_order_.pop_front();}
  reply_order_.push_back(key);
 }
 replies_[key]=reply;
}
void VisionPipeline::command(uint64_t connection,const std::string& line){
 json id=nullptr;
 try{
  auto request=json::parse(line);
  if(!request.is_object())throw std::runtime_error("request must be an object");
  if(request.contains("id"))id=request["id"];
  if(!id.is_null() && !id.is_number_integer() && !id.is_string())throw std::runtime_error("id must be integer/string");
  if(id.dump().size()>128)throw std::runtime_error("id too long");
  std::string key=std::to_string(connection)+":"+id.dump();
  if(!id.is_null()){
   auto found=replies_.find(key);
   if(found!=replies_.end()){server_.sendResponse(connection,found->second.dump()+"\n");return;}
  }else key.clear();
  auto cmd=request.value("cmd",std::string{});
  if(cmd.empty()){
   auto skill=request.value("skill_name",std::string{});
   if(skill=="CaptureSkill")cmd="capture";else if(skill=="QuerySkill")cmd="get_status";
  }
  json response={{"id",id},{"status","completed"},{"code",0}};
  if(cmd=="get_status")response["data"]=status();
  else if(cmd=="get_events"){
   if(!video_)throw std::runtime_error("video unavailable");
   response["data"]=video_->recordingStatus();
  }
  else if(cmd=="set_infer_delay"){
   if(!config_.allow_fault_injection)throw std::runtime_error("fault injection disabled");
   if(!request.at("value").is_number_integer())throw std::runtime_error("delay must be integer milliseconds");
   int delay=request.at("value").get<int>();
   if(delay<0 || delay>5000)throw std::runtime_error("delay outside [0,5000]");
   infer_delay_ms_=delay;response["data"]={{"infer_delay_ms",delay}};
  }
  else if(cmd=="get_metrics"){response["data"]=metrics_.snapshot();response["data"]["pipeline"]=status();}
  else if(cmd=="get_detections"){
   std::lock_guard<std::mutex> l(state_mutex_);
   bool fresh=detection_sequence_ && std::chrono::duration<double,std::milli>(FrameClock::now()-detection_time_).count()<=config_.infer_max_age_ms;
   response["data"]={{"fresh",fresh},{"sequence",detection_sequence_},{"detections",fresh?detections_:json::array()},
    {"age_ms",detection_sequence_?std::chrono::duration<double,std::milli>(FrameClock::now()-detection_time_).count():-1.}};
  }else if(cmd=="start" || cmd=="stop"){
   std::lock_guard<std::mutex> l(state_mutex_);
   ++inference_epoch_;enabled_=(cmd=="start");inference_.clear();
   detections_=json::array();detection_sequence_=0;
   response["data"]={{"inference_enabled",enabled_.load()}};
  }else if(cmd=="restart_camera"){
   restart_capture_=true;response["status"]="accepted";
   response["data"]={{"note","camera restart requested; poll get_status"}};
  }
  else if(cmd=="set_threshold"){
   float value=request.at("value").get<float>();
   if(!std::isfinite(value) || value<=0 || value>=1)throw std::runtime_error("threshold must be in (0,1)");
   threshold_=value;response["data"]={{"threshold",value}};
  }else if(cmd=="capture"){
   FramePtr frame;
   {std::lock_guard<std::mutex> l(state_mutex_);frame=latest_;}
   if(!frame || FrameClock::now()-frame->captured>std::chrono::seconds(2))
    throw std::runtime_error("no fresh frame available");
   if(!snapshots_.tryPush(Snapshot{frame,connection,id,key})){
    response["status"]="error";response["code"]=429;response["reason"]="snapshot queue full";
   }else {server_.retain(connection);response["status"]="accepted";}
  }else if(cmd=="record_event"){
   if(!video_ || !video_->healthy())throw std::runtime_error("video unavailable");
   video_->trigger(FrameClock::now());response["status"]="accepted";
   response["data"]={{"note","event recording requested; poll get_events (overlapping triggers merge)"}};
  }else throw std::runtime_error("unknown command");
  if(!key.empty())cacheReply(key,response);
  server_.sendResponse(connection,response.dump()+"\n");
 }catch(const std::exception& e){
  server_.sendResponse(connection,json({{"id",id},{"status","error"},{"code",400},{"reason",e.what()}}).dump()+"\n");
 }
}
void VisionPipeline::snapshotLoop(){
 while(true){
  Snapshot job;if(!snapshots_.pop(job)){if(snapshots_.closed())break;continue;}
  json response={{"id",job.id},{"status","completed"},{"code",0}};
  try{
   cv::Mat raw(config_.height,config_.width,CV_8UC2,const_cast<uint8_t*>(job.frame->bytes.data())),bgr;
   cv::cvtColor(raw,bgr,cv::COLOR_YUV2BGR_YUYV);
   auto path=config_.output+"/capture-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+".jpg";
   if(!cv::imwrite(path,bgr))throw std::runtime_error("JPEG write failed");
   response["data"]={{"path",path},{"sequence",job.frame->sequence}};metrics_.increment("snapshots");
  }catch(const std::exception& e){response["status"]="error";response["code"]=500;response["reason"]=e.what();}
  loop_.queueInLoop([this,connection=job.connection,key=job.cache_key,response]{
   if(!key.empty())cacheReply(key,response);
   server_.sendResponse(connection,response.dump()+"\n");server_.release(connection);
  });
 }
}
void VisionPipeline::publishMetrics(){
 auto j=metrics_.snapshot();j["pipeline"]=status();j["connections"]=server_.connectionCount();
 std::cout<<"[metrics] "<<j.dump()<<std::endl;
 metrics_file_<<j.dump()<<"\n";metrics_file_.flush();
}
void VisionPipeline::stop(){
 running_=false;inference_.close();snapshots_.close(false);
 if(capture_thread_.joinable())capture_thread_.join();
 if(infer_thread_.joinable())infer_thread_.join();
 if(io_thread_.joinable())io_thread_.join();
 if(video_)video_->stop();
 loop_.stop();if(network_thread_.joinable())network_thread_.join();
 server_.stop();
 {std::lock_guard<std::mutex> l(state_mutex_);latest_.reset();capture_state_="stopped";}
 if(metrics_file_.is_open()){publishMetrics();metrics_file_.close();}
}
