#ifndef RTP_VIDEO_RECEIVER_H
#define RTP_VIDEO_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "video_codec.h"
#include "hevc_depayloader.h"

// Custom, single-threaded HEVC RTP receiver that replaces the GStreamer live path.
// Receives RTP datagrams (UDP or abstract AF_UNIX), depayloads to Annex-B access
// units, and invokes the frame callback once per frame (feeding MPP + DVR record).
class RtpVideoReceiver {
public:
    using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>, uint32_t rtp_ts)>;

    explicit RtpVideoReceiver(int udp_port);       // UDP source (production: 5600)
    explicit RtpVideoReceiver(const char* unix_sock); // abstract AF_UNIX dgram (compat)
    ~RtpVideoReceiver();

    void start(NEW_FRAME_CALLBACK on_frame);
    void stop();

private:
    void recv_loop();
    void open_socket();

    int m_port = -1;
    std::string m_unix_socket;       // empty => UDP
    int m_sock = -1;
    int m_restream_sock = -1;
    std::atomic<bool> m_run{false};
    std::thread m_thread;
    NEW_FRAME_CALLBACK m_cb;
    HevcDepayloader m_depay;
    uint16_t m_last_seq = 0;
    bool m_have_seq = false;
};

#ifdef __cplusplus
extern "C" {
#endif
void idr_set_enabled(bool enabled);
bool idr_get_enabled();
void restream_set_enabled(bool enabled);
bool restream_get_enabled();
void restream_scan_clients(char* buf, size_t buf_len);
void restream_set_manual_ip(const char* ip);
const char* restream_get_manual_ip();
void restream_set_pinned_ip(const char* ip);
void idr_request_record_start();
void idr_request_decoder_issue(const char* reason);
void idr_notify_decoded_frame();
#ifdef __cplusplus
}
#endif

#endif // RTP_VIDEO_RECEIVER_H
