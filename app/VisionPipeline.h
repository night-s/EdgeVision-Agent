#pragma once
#include "Config.h"
#include "InferenceHealth.h"
#include "core/memory/FramePool.h"
#include "core/ipc/BoundedQueue.h"
#include "core/metrics/Metrics.h"
#include "core/reactor/TcpServer.h"
#include "hardware/v4l2_camera/V4L2Camera.h"
#include "hardware/rknpu_infer/RKNNEngine.h"
#include "hardware/rknpu_infer/Preprocessor.h"
#include "media/VideoService.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <deque>
class VisionPipeline {
 Config config_;
 FrameClock::time_point origin_=FrameClock::now();
 FramePool pool_;
 BoundedQueue<FramePtr> inference_;
 struct Snapshot {FramePtr frame;uint64_t connection;json id;std::string cache_key;};
 BoundedQueue<Snapshot> snapshots_{8};
 Metrics metrics_;
 InferenceHealth inference_health_;
 std::atomic<int> infer_delay_ms_{0};
 RKNNEngine engine_;
 Preprocessor preprocess_;
 EventLoop loop_;
 TcpServer server_;
 std::unique_ptr<VideoService> video_;
 std::atomic<bool> running_{false},enabled_{true};
 std::atomic<float> threshold_;
 // Protected with state_mutex_: a stop/restart invalidates queued and in-flight results.
 uint64_t inference_epoch_=0;
 std::atomic<bool> restart_capture_{false};
 std::thread capture_thread_,infer_thread_,io_thread_,network_thread_;
 mutable std::mutex state_mutex_;
 std::string capture_state_="starting",capture_error_,infer_error_;
 FramePtr latest_;
 json detections_=json::array();
 uint64_t detection_sequence_=0;
 FrameClock::time_point detection_time_{};
 // Network-thread-only bounded replay cache, scoped to connection generation.
 std::map<std::string,json> replies_;
 std::deque<std::string> reply_order_;
 std::ofstream metrics_file_;
 void captureLoop();
 void inferenceLoop();
 void snapshotLoop();
 void command(uint64_t,const std::string&);
 void cacheReply(const std::string&,const json&);
 json status();
 void publishMetrics();
public:
 explicit VisionPipeline(Config config);
 ~VisionPipeline();
 void start();
 void stop();
 bool running()const{return running_;}
};
