#ifndef LATENCY_PROBE_HPP
#define LATENCY_PROBE_HPP

#include <atomic>
#include <cstdint>
#include <cstddef>   // size_t
#include <string>

namespace latency_probe {

struct RtpHeaderInfo {
    uint32_t ssrc;
    uint32_t timestamp;
    bool marker;
};

// Parses the fixed 12-byte RTP header (no CSRC, no extension).
// Returns true and fills info on success, false on short buffer or
// version != 2. Does NOT validate CSRC count or extension — we only
// need ssrc/timestamp/marker which all sit in the fixed prefix.
bool parse_rtp_header(const uint8_t* data, size_t len, RtpHeaderInfo& info);

// Cheap-to-check gate for hot paths. Loaded with relaxed memory order;
// changes only at start()/stop() which the application drives at startup
// and shutdown.
extern std::atomic<bool> active;

// Lifecycle. host is the drone's IP, port is the sidecar UDP port
// (typical default 5602). Returns true on success.
// Safe to call when already started (no-op). Safe to call with active==false.
bool start(const std::string& host, uint16_t port);
void stop();

// Hot-path hook: called from the GStreamer pad probe for every RTP
// packet received on the video udpsrc. data points at the raw RTP packet
// (header + payload); len is the full packet length. Implementation
// parses the header and acts only on packets with marker=1.
// No-op when active==false.
void on_rtp_buffer(const uint8_t* data, size_t len, uint64_t gs_recv_us);

// Hot-path hook: called from the decoder thread immediately after a
// successful decode_get_frame(). Binds to the oldest pending frame in
// the matcher's deque.
// No-op when active==false.
void record_decode_done(uint64_t gs_decode_us);

// Hot-path hook: called from the display thread immediately after
// drmModeAtomicCommit. Binds to the oldest pending frame in the matcher's
// deque.
// No-op when active==false.
void record_display_submit(uint64_t gs_display_us);

// Returns a CLOCK_MONOTONIC sample in microseconds. Convenience for callers.
uint64_t now_us();

} // namespace latency_probe

#endif // LATENCY_PROBE_HPP
