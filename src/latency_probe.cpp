#include "latency_probe.hpp"

#include <ctime>

namespace latency_probe {

std::atomic<bool> active{false};

bool start(const std::string& /*host*/, uint16_t /*port*/) { return false; }
void stop() {}

void on_rtp_buffer(const uint8_t* /*data*/, size_t /*len*/, uint64_t /*gs_recv_us*/) {}
void record_decode_done(uint64_t /*gs_decode_us*/) {}
void record_display_submit(uint64_t /*gs_display_us*/) {}

bool parse_rtp_header(const uint8_t* data, size_t len, RtpHeaderInfo& info) {
    if (!data || len < 12) return false;
    if ((data[0] >> 6) != 2) return false;  // version must be 2

    info.marker    = (data[1] & 0x80) != 0;
    info.timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                     (static_cast<uint32_t>(data[5]) << 16) |
                     (static_cast<uint32_t>(data[6]) << 8)  |
                     (static_cast<uint32_t>(data[7]));
    info.ssrc      = (static_cast<uint32_t>(data[8])  << 24) |
                     (static_cast<uint32_t>(data[9])  << 16) |
                     (static_cast<uint32_t>(data[10]) << 8)  |
                     (static_cast<uint32_t>(data[11]));
    return true;
}

uint64_t now_us() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1'000ull;
}

} // namespace latency_probe
