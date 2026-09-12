#pragma once
#include "EventRecorder.h"
#include "app/Config.h"
#include "core/ipc/BoundedQueue.h"
#include "core/memory/FramePool.h"
#include "core/metrics/Metrics.h"
#include <atomic>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <mutex>
#include <thread>
class VideoService {
    Config config_;
    Metrics &metrics_;
    FrameClock::time_point origin_;
    BoundedQueue<FramePtr> frames_{2};
    EventRecorder recorder_;
    GstElement *pipeline_ = nullptr, *input_ = nullptr, *sink_ = nullptr, *rtsp_input_ = nullptr;
    GstRTSPServer *server_ = nullptr;
    GstRTSPMedia *media_ = nullptr;
    GMainContext *context_ = nullptr;
    GMainLoop *loop_ = nullptr;
    guint source_ = 0;
    std::thread video_thread_, rtsp_thread_;
    std::mutex rtsp_mutex_, error_mutex_;
    bool waiting_key_ = true;
    uint64_t rtsp_base_ = 0;
    std::atomic<bool> running_{false}, healthy_{false};
    std::string error_;
    static GstFlowReturn sampleCallback(GstAppSink *, gpointer);
    static void configureMedia(GstRTSPMediaFactory *, GstRTSPMedia *, gpointer);
    static void unprepareMedia(GstRTSPMedia *, gpointer);
    void onPacket(GstSample *);
    void run();
    void setError(const std::string &);

  public:
    VideoService(const Config &, Metrics &, FrameClock::time_point);
    ~VideoService();
    void start();
    void stop();
    void submit(FramePtr frame) {
        if (healthy_)
            frames_.pushLatest(std::move(frame));
    }
    void trigger(FrameClock::time_point t);
    nlohmann::json recordingStatus() {
        return recorder_.status();
    }
    void resetRecording(FrameClock::time_point t) {
        recorder_.reset(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t - origin_).count()));
    }
    bool healthy() const {
        return healthy_;
    }
    std::string error() {
        std::lock_guard<std::mutex> l(error_mutex_);
        return error_;
    }
    size_t dropped() const {
        return frames_.dropped();
    }
};
