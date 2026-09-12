#include "core/ipc/BoundedQueue.h"
#include "core/memory/FramePool.h"
#include "hardware/rknpu_infer/Yolov5PostProcess.h"
#include "core/reactor/EventLoop.h"
#include "app/Config.h"
#include "app/InferenceHealth.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include "media/EventRecorder.h"
#define CHECK(x) do {if(!(x))throw std::runtime_error("CHECK failed: " #x);}while(0)
int main(){
 try{
  BoundedQueue<int> newest(3);newest.pushLatest(1);newest.pushLatest(2);newest.pushLatest(3);
  int latest=0;CHECK(newest.popLatest(latest,0));CHECK(latest==3);CHECK(newest.dropped()==2);
  InferenceHealth health(150,2000);
  health.end(160);health.end(160);CHECK(!health.degraded());health.end(160);CHECK(health.degraded());
  for(int i=0;i<9;++i)health.end(80);CHECK(health.degraded());
  health.end(80);CHECK(!health.degraded());
  auto clock=std::chrono::steady_clock::now();health.begin(clock);
  CHECK(health.snapshot(clock+std::chrono::seconds(3))["state"]=="stalled");
  health.fail();CHECK(health.snapshot()["active"]==false);
  FramePool pool(2,32);
  auto f=pool.acquire();auto second=pool.acquire();CHECK(!pool.acquire());
  second.reset();CHECK(pool.available()==1);
  BoundedQueue<FramePtr> frames(1);
  frames.pushLatest(f);f.reset();frames.pushLatest(pool.acquire());
  CHECK(frames.dropped()==1);CHECK(pool.available()==1);
  frames.close();CHECK(pool.available()==2);
  FramePtr survivor;
  {FramePool temporary(1,8);survivor=temporary.acquire();}
  CHECK(survivor->bytes.size()==8);survivor.reset();
  BoundedQueue<int> commands(1);CHECK(commands.tryPush(1));CHECK(!commands.tryPush(2));
  int value=0;CHECK(commands.pop(value,0)&&value==1);
  auto waiter=std::async(std::launch::async,[&]{int x;return commands.pop(x,5000);});
  commands.close();CHECK(waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready);
  CHECK(!waiter.get());CHECK(!commands.tryPush(3));
  auto box=Letterbox::make(640,480);
  CHECK(box.top==80);CHECK(box.restore(0,80,640,560)==cv::Rect(0,0,640,480));
  CHECK(box.restore(-100,-100,800,800)==cv::Rect(0,0,640,480));
  CHECK(box.restore(10,10,20,20).area()==0);
  auto small=Letterbox::make(320,240);
  CHECK(small.restore(0,80,640,560)==cv::Rect(0,0,320,240));
  std::vector<Detection> overlapping{{0,.9,{10,10,40,40}},{0,.8,{10,10,40,40}},{1,.7,{10,10,40,40}}};
  CHECK(Yolov5PostProcess::nms(overlapping,.45).size()==2);
  std::array<std::vector<float>,3> tensors;
  std::array<rknn_output,3> outputs{};
  for(int i=0;i<3;++i){int g=80>>i;tensors[i].resize(255*g*g,0);outputs[i].buf=tensors[i].data();outputs[i].size=tensors[i].size()*4;}
  // One anchor at grid (40,40), xywh=.5 yields anchor dimensions 10x13.
  int hw=80*80,cell=40*80+40;
  for(int i=0;i<4;++i)tensors[0][cell+i*hw]=.5;
  tensors[0][cell+4*hw]=.9;tensors[0][cell+5*hw]=.9;
  auto detections=Yolov5PostProcess().process(outputs.data(),box,.35,.45,false);
  CHECK(detections.size()==1);CHECK(detections[0].box.width==10);CHECK(detections[0].box.height==13);
  CHECK(detections[0].box.y==238);CHECK(std::abs(detections[0].confidence-.81)<.001);
  std::array<std::vector<int8_t>,3> quant;
  std::array<rknn_tensor_attr,3> attrs{};
  std::array<rknn_output,3> native{};
  for(int i=0;i<3;++i){
   attrs[i].type=RKNN_TENSOR_INT8;attrs[i].qnt_type=RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
   attrs[i].scale=.01f;attrs[i].zp=-100;
   for(float value:tensors[i])quant[i].push_back(int8_t(std::lround(value/.01f)-100));
   native[i].buf=quant[i].data();native[i].size=quant[i].size();
  }
  auto native_dets=Yolov5PostProcess().process(native.data(),box,.35,.45,false,attrs.data());
  CHECK(native_dets.size()==detections.size());
  CHECK(native_dets[0].box==detections[0].box);
  CHECK(std::abs(native_dets[0].confidence-detections[0].confidence)<1e-5);
  outputs[0].size=0;bool rejected=false;
  try{Yolov5PostProcess().process(outputs.data(),box,.35,.45,false);}catch(...){rejected=true;}CHECK(rejected);
  // A callback can unregister itself without destroying the executing std::function.
  EventLoop loop;bool fired=false;
  loop.timerMgr().addTimer(1,0,[&]{fired=true;loop.stop();});
  auto task=std::async(std::launch::async,[&]{loop.run();});
  if(task.wait_for(std::chrono::seconds(2))!=std::future_status::ready){loop.stop();throw std::runtime_error("timer stalled");}
  task.get();CHECK(fired);
  EventLoop periodic;int calls=0;uint64_t timer=0;
  timer=periodic.timerMgr().addTimer(1,1,[&]{++calls;CHECK(periodic.timerMgr().cancelTimer(timer));});
  periodic.timerMgr().addTimer(30,0,[&]{periodic.stop();});periodic.run();CHECK(calls==1);
  EventLoop stopped;stopped.stop();auto stopped_task=std::async(std::launch::async,[&]{stopped.run();});
  CHECK(stopped_task.wait_for(std::chrono::seconds(1))==std::future_status::ready);stopped_task.get();
  BoundedQueue<int> drain(2);drain.tryPush(7);drain.close(false);
  CHECK(drain.pop(value,0)&&value==7);CHECK(!drain.pop(value,0));CHECK(drain.closed());
  // An event begins at a keyframe and extends, never shrinks, on out-of-order triggers.
  char directory[]="/tmp/edge-recorder-test-XXXXXX";CHECK(mkdtemp(directory)!=nullptr);
  Metrics metrics;
  {
   EventRecorder recorder(metrics,directory,1,1);recorder.start();
   recorder.trigger(2000000000ULL);recorder.trigger(0);
   for(unsigned i=0;i<5;++i){
    auto packet=std::make_shared<EncodedPacket>();packet->pts=uint64_t(i)*1000000000;
    packet->key=true;packet->bytes={0,0,0,1,0x65,uint8_t(i)};
    recorder.submit(packet);
   }
   recorder.stop();
  }
  bool manifest_found=false;
  for(const auto& entry:std::filesystem::directory_iterator(directory)){
   if(entry.path().extension()==".json"){
    std::ifstream file(entry.path());json manifest;file>>manifest;
    CHECK(manifest["complete"]==true);CHECK(manifest["end_pts_ns"]==3000000000ULL);
    manifest_found=true;
   }
  }
  CHECK(manifest_found);
  std::filesystem::remove_all(directory);
  char timeout_dir[]="/tmp/edge-timeout-test-XXXXXX";CHECK(mkdtemp(timeout_dir)!=nullptr);
  {
   EventRecorder recorder(metrics,timeout_dir,1,5,150);recorder.start();recorder.trigger(1000000000);
   auto p=std::make_shared<EncodedPacket>();p->pts=1000000000;p->key=true;p->bytes={0,0,0,1,0x65};
   recorder.submit(p);
   auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(recorder.status()["recent"].empty() && std::chrono::steady_clock::now()<deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
   auto state=recorder.status();CHECK(state["recent"].size()==1);
   CHECK(state["current"]["reason"]=="input_timeout");CHECK(state["current"]["complete"]==false);
   recorder.stop();
  }
  std::filesystem::remove_all(timeout_dir);
  std::cout<<"queue/pool/geometry/anchor/NMS/timer checks passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
