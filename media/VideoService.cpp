#include "VideoService.h"
#include <cstring>
#include <iostream>
VideoService::VideoService(const Config &c, Metrics &m, FrameClock::time_point o)
    : config_(c), metrics_(m), origin_(o),
      recorder_(m, c.output + "/events", c.event_pre, c.event_tail, 2000,
                StorageLimits{uint64_t(c.record_max_mb) * 1024 * 1024, size_t(c.record_max_files),
                              uint64_t(c.record_min_free_mb) * 1024 * 1024, c.record_max_seconds}) {
}
VideoService::~VideoService() {
    stop();
}
void VideoService::setError(const std::string &text) {
    std::lock_guard<std::mutex> l(error_mutex_);
    error_ = text;
    healthy_ = false;
    std::cerr << "[video] " << text << std::endl;
    metrics_.increment("video_errors");
}
void VideoService::start() {
    gst_init(nullptr, nullptr);
    GError *err = nullptr;
    std::string launch =
        "appsrc name=input is-live=true format=time block=false max-bytes=" +
        std::to_string(config_.width * config_.height * 4) +
        " ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream"
        " ! videoconvert ! video/x-raw,format=NV12"
        " ! mpph264enc bps=" +
        std::to_string(config_.bitrate) + " gop=" + std::to_string(config_.fps) +
        " header-mode=each-idr profile=baseline ! h264parse config-interval=-1"
        " ! video/x-h264,stream-format=byte-stream,alignment=au"
        " ! appsink name=encoded emit-signals=true sync=false max-buffers=4 drop=false";
    pipeline_ = gst_parse_launch(launch.c_str(), &err);
    if (err) {
        std::string e = err->message;
        g_error_free(err);
        throw std::runtime_error(e);
    }
    if (!pipeline_)
        throw std::runtime_error("cannot create MPP encoder pipeline");
    input_ = gst_bin_get_by_name(GST_BIN(pipeline_), "input");
    sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "encoded");
    GstCaps *caps =
        gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "YUY2", "width", G_TYPE_INT,
                            config_.width, "height", G_TYPE_INT, config_.height, "framerate",
                            GST_TYPE_FRACTION, config_.fps, 1, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(input_), caps);
    gst_caps_unref(caps);
    g_signal_connect(sink_, "new-sample", G_CALLBACK(sampleCallback), this);
    context_ = g_main_context_new();
    loop_ = g_main_loop_new(context_, FALSE);
    server_ = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server_, std::to_string(config_.rtsp_port).c_str());
    auto mounts = gst_rtsp_server_get_mount_points(server_);
    auto factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(
        factory, "( appsrc name=stream is-live=true format=time block=false max-bytes=1048576"
                 " caps=video/x-h264,stream-format=byte-stream,alignment=au"
                 " ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=-1 )");
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(configureMedia), this);
    gst_rtsp_mount_points_add_factory(mounts, "/live", factory);
    g_object_unref(mounts);
    source_ = gst_rtsp_server_attach(server_, context_);
    if (!source_)
        throw std::runtime_error("cannot bind RTSP port");
    recorder_.start();
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("MPP encoder refused PLAYING state");
    running_ = true;
    healthy_ = true;
    rtsp_thread_ = std::thread([this] {
        g_main_context_push_thread_default(context_);
        g_main_loop_run(loop_);
        g_main_context_pop_thread_default(context_);
    });
    video_thread_ = std::thread([this] { run(); });
    std::cout << "[video] RTSP /live on port " << config_.rtsp_port << std::endl;
}
void VideoService::configureMedia(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer user) {
    auto self = static_cast<VideoService *>(user);
    GstElement *element = gst_rtsp_media_get_element(media);
    GstElement *input = gst_bin_get_by_name_recurse_up(GST_BIN(element), "stream");
    gst_object_unref(element);
    std::lock_guard<std::mutex> l(self->rtsp_mutex_);
    if (self->rtsp_input_)
        gst_object_unref(self->rtsp_input_);
    if (self->media_) {
        g_signal_handlers_disconnect_by_data(self->media_, self);
        g_object_unref(self->media_);
    }
    self->rtsp_input_ = input;
    self->media_ = GST_RTSP_MEDIA(g_object_ref(media));
    self->waiting_key_ = true;
    self->rtsp_base_ = GST_CLOCK_TIME_NONE;
    g_signal_connect(media, "unprepared", G_CALLBACK(unprepareMedia), self);
}
void VideoService::unprepareMedia(GstRTSPMedia *, gpointer user) {
    auto self = static_cast<VideoService *>(user);
    std::lock_guard<std::mutex> l(self->rtsp_mutex_);
    if (self->rtsp_input_) {
        gst_object_unref(self->rtsp_input_);
        self->rtsp_input_ = nullptr;
    }
    self->waiting_key_ = true;
}
GstFlowReturn VideoService::sampleCallback(GstAppSink *sink, gpointer user) {
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_EOS;
    auto self = static_cast<VideoService *>(user);
    try {
        self->onPacket(sample);
    } catch (const std::exception &e) {
        self->setError(e.what());
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
void VideoService::onPacket(GstSample *sample) {
    auto buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return;
    auto packet = std::make_shared<EncodedPacket>();
    packet->pts = GST_BUFFER_PTS(buffer);
    packet->key = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    packet->bytes.assign(map.data, map.data + map.size);
    gst_buffer_unmap(buffer, &map);
    if (packet->pts == GST_CLOCK_TIME_NONE)
        return;
    metrics_.increment("encoded");
    double elapsed = std::chrono::duration<double, std::milli>(FrameClock::now() - origin_).count();
    metrics_.observe("capture_to_encoded", std::max(0., elapsed - packet->pts / 1e6));
    recorder_.submit(packet);
    std::lock_guard<std::mutex> l(rtsp_mutex_);
    if (!rtsp_input_)
        return;
    guint64 queued = 0;
    g_object_get(rtsp_input_, "current-level-bytes", &queued, nullptr);
    if (queued > 1048576) {
        waiting_key_ = true;
        metrics_.increment("rtsp_packets_dropped");
        return;
    }
    if (waiting_key_ && !packet->key)
        return;
    if (waiting_key_) {
        waiting_key_ = false;
        if (rtsp_base_ == GST_CLOCK_TIME_NONE)
            rtsp_base_ = packet->pts;
    }
    GstBuffer *out = gst_buffer_copy(buffer);
    GST_BUFFER_PTS(out) = packet->pts - rtsp_base_;
    GST_BUFFER_DTS(out) = GST_BUFFER_PTS(out);
    auto result = gst_app_src_push_buffer(GST_APP_SRC(rtsp_input_), out);
    if (result != GST_FLOW_OK)
        waiting_key_ = true;
}
void VideoService::trigger(FrameClock::time_point t) {
    auto pts = std::chrono::duration_cast<std::chrono::nanoseconds>(t - origin_).count();
    if (pts >= 0)
        recorder_.trigger(uint64_t(pts));
}
void VideoService::run() {
    auto bus = gst_element_get_bus(pipeline_);
    while (running_) {
        auto message =
            gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *err = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &err, &debug);
                setError(err ? err->message : "GStreamer error");
                if (err)
                    g_error_free(err);
                g_free(debug);
            } else
                setError("unexpected encoder EOS");
            gst_message_unref(message);
        }
        FramePtr frame;
        if (!frames_.pop(frame))
            continue;
        if (!healthy_)
            continue;
        guint64 queued = 0;
        g_object_get(input_, "current-level-bytes", &queued, nullptr);
        if (queued >= frame->bytes.size() * 2) {
            metrics_.increment("encoder_input_dropped");
            continue;
        }
        // Storage remains alive until GStreamer drops its final reference.
        auto holder = new FramePtr(frame);
        auto buffer = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY, const_cast<uint8_t *>(frame->bytes.data()),
            frame->bytes.size(), 0, frame->bytes.size(), holder,
            [](gpointer ptr) { delete static_cast<FramePtr *>(ptr); });
        GST_BUFFER_PTS(buffer) =
            std::chrono::duration_cast<std::chrono::nanoseconds>(frame->captured - origin_).count();
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = GST_SECOND / config_.fps;
        if (gst_app_src_push_buffer(GST_APP_SRC(input_), buffer) != GST_FLOW_OK)
            setError("encoder appsrc push failed");
    }
    gst_object_unref(bus);
}
void VideoService::stop() {
    healthy_ = false;
    running_ = false;
    frames_.close();
    if (video_thread_.joinable())
        video_thread_.join();
    if (pipeline_) {
        if (input_) {
            gst_app_src_end_of_stream(GST_APP_SRC(input_));
            auto bus = gst_element_get_bus(pipeline_);
            auto message = gst_bus_timed_pop_filtered(
                bus, 2 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (message)
                gst_message_unref(message);
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (sink_)
        g_signal_handlers_disconnect_by_data(sink_, this);
    recorder_.stop();
    if (context_ && loop_) {
        auto quit = g_idle_source_new();
        g_source_set_callback(
            quit,
            [](gpointer data) -> gboolean {
                g_main_loop_quit(static_cast<GMainLoop *>(data));
                return G_SOURCE_REMOVE;
            },
            loop_, nullptr);
        g_source_attach(quit, context_);
        g_source_unref(quit);
    }
    if (rtsp_thread_.joinable())
        rtsp_thread_.join();
    if (server_)
        gst_rtsp_server_client_filter(
            server_,
            [](GstRTSPServer *, GstRTSPClient *, gpointer) { return GST_RTSP_FILTER_REMOVE; },
            nullptr);
    {
        std::lock_guard<std::mutex> l(rtsp_mutex_);
        if (media_) {
            g_signal_handlers_disconnect_by_data(media_, this);
            g_object_unref(media_);
            media_ = nullptr;
        }
        if (rtsp_input_) {
            gst_object_unref(rtsp_input_);
            rtsp_input_ = nullptr;
        }
    }
    if (source_ && context_) {
        auto src = g_main_context_find_source_by_id(context_, source_);
        if (src)
            g_source_destroy(src);
        source_ = 0;
    }
    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
    if (context_) {
        g_main_context_unref(context_);
        context_ = nullptr;
    }
    if (input_) {
        gst_object_unref(input_);
        input_ = nullptr;
    }
    if (sink_) {
        gst_object_unref(sink_);
        sink_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}
