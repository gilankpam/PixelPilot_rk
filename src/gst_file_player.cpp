//
// GStreamer-backed DVR mp4 file player (H265 only).
// Live-path code (side-channels, IDR, restream, appsrc, socket reader) lives
// in rtp_video_receiver.cpp.
//

#include "gst_file_player.h"
#include "gst/gstparse.h"
#include "gst/gstpipeline.h"
#include "gst/app/gstappsink.h"
#include "spdlog/spdlog.h"
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <sstream>
#include <memory>
#include <thread>
#include <chrono>

static VideoCodec detect_mp4_codec(const char* file_path) {
    auto scan = [](const uint8_t* buf, size_t n) -> VideoCodec {
        for (size_t i = 0; i + 3 < n; i++) {
            if (buf[i]=='h' && buf[i+1]=='v' && buf[i+2]=='c' && buf[i+3]=='1')
                return VideoCodec::H265;
            if (buf[i]=='h' && buf[i+1]=='e' && buf[i+2]=='v' && buf[i+3]=='1')
                return VideoCodec::H265;
        }
        return VideoCodec::UNKNOWN;
    };

    FILE* f = fopen(file_path, "rb");
    if (!f) return VideoCodec::UNKNOWN;

    uint8_t buf[16384];
    size_t n = fread(buf, 1, sizeof(buf), f);
    VideoCodec result = scan(buf, n);

    if (result == VideoCodec::UNKNOWN) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        long tail_offset = (fsize > (long)sizeof(buf)) ? fsize - (long)sizeof(buf) : 0;
        fseek(f, tail_offset, SEEK_SET);
        n = fread(buf, 1, sizeof(buf), f);
        result = scan(buf, n);
    }

    fclose(f);
    return result;
}

static void initGstreamerOrThrow() {
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        g_error_free(error);
        throw std::runtime_error("GStreamer initialization failed");
    }
}

static std::shared_ptr<std::vector<uint8_t>> gst_copy_buffer(GstBuffer* buffer) {
    assert(buffer);
    const auto buff_size = gst_buffer_get_size(buffer);
    auto ret = std::make_shared<std::vector<uint8_t>>(buff_size);
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    assert(map.size == buff_size);
    std::memcpy(ret->data(), map.data, buff_size);
    gst_buffer_unmap(buffer, &map);
    return ret;
}

GstFilePlayer::GstFilePlayer() {
    initGstreamerOrThrow();
}

GstFilePlayer::~GstFilePlayer() {
    stop();
}

std::string GstFilePlayer::construct_file_playback_pipeline(const char* file_path) {
    m_playback_codec = VideoCodec::H265;
    std::stringstream ss;
    ss << "filesrc location=" << file_path << " ! qtdemux ! ";
    ss << "h265parse config-interval=-1 ! ";
    ss << "video/x-h265, stream-format=\"byte-stream\", alignment=au ! ";
    ss << "appsink drop=true name=out_appsink";
    return ss.str();
}

void GstFilePlayer::loop_pull_samples() {
    assert(m_app_sink_element);
    const uint64_t timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::milliseconds(100)).count();
    while (m_pull_samples_run) {
        GstSample* sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(m_app_sink_element), timeout_ns);
        if (sample) {
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            if (buffer && m_cb) {
                auto buff_copy = gst_copy_buffer(buffer);
                m_cb(buff_copy);
            }
            gst_sample_unref(sample);
        }
    }
}

VideoCodec GstFilePlayer::start(const char* file_path, NEW_FRAME_CALLBACK cb) {
    stop();

    m_cb = cb;
    const auto pipeline_str = construct_file_playback_pipeline(file_path);
    GError* error = nullptr;
    m_gst_pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    spdlog::info("GSTREAMER FILE PLAYBACK PIPE=[{}]", pipeline_str);

    if (error) {
        spdlog::error("gst_parse_launch error: {}", error->message);
        g_error_free(error);
        return m_playback_codec;
    }

    if (!m_gst_pipeline || !(GST_IS_PIPELINE(m_gst_pipeline))) {
        spdlog::error("Cannot construct file playback pipeline");
        m_gst_pipeline = nullptr;
        return m_playback_codec;
    }

    m_app_sink_element = gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
    assert(m_app_sink_element);

    gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);

    m_pull_samples_run = true;
    m_pull_samples_thread = std::make_unique<std::thread>(&GstFilePlayer::loop_pull_samples, this);
    return m_playback_codec;
}

void GstFilePlayer::stop() {
    spdlog::info("GstFilePlayer::stop start");
    m_pull_samples_run = false;

    if (m_pull_samples_thread) {
        m_pull_samples_thread->join();
        m_pull_samples_thread = nullptr;
    }

    if (m_gst_pipeline != nullptr) {
        gst_element_send_event((GstElement*)m_gst_pipeline, gst_event_new_eos());
        gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
        gst_element_set_state(m_gst_pipeline, GST_STATE_NULL);
        gst_object_unref(m_gst_pipeline);
        m_gst_pipeline = nullptr;
    }
    m_app_sink_element = nullptr;
    spdlog::info("GstFilePlayer::stop end");
}

void GstFilePlayer::set_playback_rate(double rate) {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot set playback rate: pipeline is not running.");
        return;
    }

    spdlog::info("Setting playback rate to: {}", rate);

    GstEvent *seek_event = gst_event_new_seek(
        rate,
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_NONE, 0,
        GST_SEEK_TYPE_NONE, 0
    );

    if (!gst_element_send_event(m_gst_pipeline, seek_event)) {
        spdlog::warn("Failed to send seek event to change playback rate.");
    } else {
        m_playback_rate = rate;
    }
}

void GstFilePlayer::fast_forward(double rate) {
    if (rate <= 1.0) {
        spdlog::warn("Fast forward rate must be greater than 1.0. Using 2.0 instead.");
        rate = 2.0;
    }
    set_playback_rate(rate);
}

void GstFilePlayer::fast_rewind(double rate) {
    if (rate <= 1.0) {
        spdlog::warn("Fast rewind rate must be greater than 1.0. Using 2.0 instead.");
        rate = 2.0;
    }
    set_playback_rate(-rate);
}

void GstFilePlayer::normal_playback() {
    set_playback_rate(1.0);
}

void GstFilePlayer::pause() {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot pause: pipeline is not running.");
        return;
    }

    if (m_is_paused) {
        spdlog::debug("Pipeline is already paused.");
        return;
    }

    m_pre_pause_rate = m_playback_rate;

    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to pause pipeline");
        return;
    }

    ret = gst_element_get_state(m_gst_pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to complete pause operation");
        return;
    }

    m_is_paused = true;
    spdlog::info("Pipeline paused");
}

void GstFilePlayer::resume() {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot resume: pipeline is not running.");
        return;
    }

    if (!m_is_paused) {
        spdlog::debug("Pipeline is not paused.");
        return;
    }

    GstStateChangeReturn ret = gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to resume pipeline");
        return;
    }

    if (m_pre_pause_rate != 1.0) {
        set_playback_rate(m_pre_pause_rate);
    }

    m_is_paused = false;
    spdlog::info("Pipeline resumed");
}

void GstFilePlayer::skip_duration(int64_t skip_ms) {
    if (!m_gst_pipeline) {
        spdlog::warn("Cannot skip: pipeline is not running.");
        return;
    }

    if (skip_ms == 0) {
        spdlog::debug("Skip duration is zero - no action taken.");
        return;
    }

    gint64 current_pos;
    if (!gst_element_query_position(m_gst_pipeline, GST_FORMAT_TIME, &current_pos)) {
        spdlog::warn("Could not query current position");
        return;
    }

    gint64 new_pos = current_pos + (skip_ms * GST_MSECOND);

    if (new_pos < 0) {
        new_pos = 0;
        spdlog::debug("Clamped skip to start of stream");
    }

    spdlog::info("Skipping {} ms (from {} to {} ms)",
                skip_ms,
                current_pos / GST_MSECOND,
                new_pos / GST_MSECOND);

    GstEvent* seek_event = gst_event_new_seek(
        1.0,
        GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, new_pos,
        GST_SEEK_TYPE_NONE, 0
    );

    if (!gst_element_send_event(m_gst_pipeline, seek_event)) {
        spdlog::warn("Failed to send seek event for skipping.");
    }
}
