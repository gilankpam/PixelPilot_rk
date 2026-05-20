#include "latency_probe.hpp"

#include <ctime>

namespace latency_probe {

std::atomic<bool> active{false};

bool start(const std::string& /*host*/, uint16_t /*port*/) { return false; }
void stop() {}

void on_rtp_buffer(const uint8_t* /*data*/, size_t /*len*/, uint64_t /*gs_recv_us*/) {}
void record_decode_done(uint64_t /*gs_decode_us*/) {}
void record_display_submit(uint64_t /*gs_display_us*/) {}

uint64_t now_us() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1'000ull;
}

} // namespace latency_probe
