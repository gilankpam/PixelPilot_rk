#ifndef GST_FILE_PLAYER_H
#define GST_FILE_PLAYER_H

#include <stdint.h>
#ifndef USE_SIMULATOR
#include <gst/gst.h>
#endif
#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include "video_codec.h"

// GStreamer-backed DVR mp4 file player (H265). Decodes filesrc->qtdemux->h265parse
// ->appsink, emitting Annex-B access units via the callback, with transport controls.
class GstFilePlayer {
public:
    using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
    GstFilePlayer();
    ~GstFilePlayer();
    // Returns the detected codec (always H265 in production recordings).
    VideoCodec start(const char* file_path, NEW_FRAME_CALLBACK cb);
    void stop();
    void fast_forward(double rate = 2.0);
    void fast_rewind(double rate = 2.0);
    void normal_playback();
    void skip_duration(int64_t skip_ms);
    void pause();
    void resume();
private:
    std::string construct_file_playback_pipeline(const char* file_path);
    void loop_pull_samples();
    void set_playback_rate(double rate);
    GstElement* m_gst_pipeline = nullptr;
    GstElement* m_app_sink_element = nullptr;
    NEW_FRAME_CALLBACK m_cb;
    VideoCodec m_playback_codec = VideoCodec::H265;
    bool m_pull_samples_run = false;
    std::unique_ptr<std::thread> m_pull_samples_thread;
    double m_playback_rate = 1.0;
    bool m_is_paused = false;
    double m_pre_pause_rate = 1.0;
};

#endif // GST_FILE_PLAYER_H
