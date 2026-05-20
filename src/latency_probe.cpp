#include "latency_probe.hpp"

#include <ctime>
#include <limits>

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

void ClockOffset::add_sample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4) {
    // rtt_us = (t4 - t1) - (t3 - t2). Both subtractions can be negative
    // in pathological samples (out-of-order delivery, clock jumps). In
    // that case we treat rtt as huge so this sample never wins.
    uint64_t rtt;
    if (t4 >= t1 && t3 >= t2 && (t4 - t1) >= (t3 - t2))
        rtt = (t4 - t1) - (t3 - t2);
    else
        rtt = std::numeric_limits<uint64_t>::max();

    // offset_us = ((t2 - t1) + (t3 - t4)) / 2, signed arithmetic.
    int64_t off = ((static_cast<int64_t>(t2) - static_cast<int64_t>(t1)) +
                   (static_cast<int64_t>(t3) - static_cast<int64_t>(t4))) / 2;

    std::lock_guard<std::mutex> lk(m_);
    Sample& slot = samples_[next_];
    bool overwriting_best =
        have_best_ && slot.valid &&
        slot.rtt_us == best_rtt_us_ && slot.offset_us == best_offset_us_;

    slot.valid     = true;
    slot.offset_us = off;
    slot.rtt_us    = rtt;
    slot.taken_us  = 0; // not used; kept for future drift handling
    next_ = (next_ + 1) % kRing;

    if (!have_best_ || rtt < best_rtt_us_) {
        best_rtt_us_    = rtt;
        best_offset_us_ = off;
        have_best_      = true;
        return;
    }
    if (overwriting_best) {
        // Re-scan the ring to pick the new minimum.
        uint64_t new_best_rtt = std::numeric_limits<uint64_t>::max();
        int64_t  new_best_off = 0;
        bool     any = false;
        for (const auto& s : samples_) {
            if (s.valid && s.rtt_us < new_best_rtt) {
                new_best_rtt = s.rtt_us;
                new_best_off = s.offset_us;
                any = true;
            }
        }
        have_best_      = any;
        best_rtt_us_    = any ? new_best_rtt : 0;
        best_offset_us_ = any ? new_best_off : 0;
    }
}

void ClockOffset::get(int64_t& offset_us, uint64_t& rtt_us) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!have_best_) { offset_us = 0; rtt_us = 0; return; }
    offset_us = best_offset_us_;
    rtt_us    = best_rtt_us_;
}

static bool is_complete(const FrameTimings& f) {
    return f.sidecar_seen &&
           f.gs_recv_last_us != 0 &&
           f.gs_decode_done_us != 0 &&
           f.gs_display_submit_us != 0;
}

void FrameMatcher::set_publish_callback(PublishFn cb) {
    std::lock_guard<std::mutex> lk(m_);
    publish_ = std::move(cb);
}

size_t FrameMatcher::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return ring_.size();
}

FrameTimings* FrameMatcher::find_by_key_locked(uint32_t ssrc, uint32_t rtp_ts) {
    for (auto& f : ring_) {
        if (f.ssrc == ssrc && f.rtp_ts == rtp_ts) return &f;
    }
    return nullptr;
}

FrameTimings& FrameMatcher::push_new_locked(uint64_t now) {
    if (ring_.size() >= kRingCap) ring_.pop_front();
    ring_.emplace_back();
    ring_.back().inserted_us = now;
    return ring_.back();
}

void FrameMatcher::on_marker_arrival(uint32_t ssrc, uint32_t rtp_ts,
                                     uint64_t gs_recv_us, uint64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    if (auto* slot = find_by_key_locked(ssrc, rtp_ts)) {
        if (slot->gs_recv_last_us == 0) slot->gs_recv_last_us = gs_recv_us;
        try_publish_locked();
        return;
    }
    auto& s = push_new_locked(now);
    s.ssrc = ssrc;
    s.rtp_ts = rtp_ts;
    s.gs_recv_last_us = gs_recv_us;
    try_publish_locked();
}

void FrameMatcher::on_decode_done(uint64_t gs_decode_us, uint64_t /*now*/) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& f : ring_) {
        if (f.gs_decode_done_us == 0) {
            f.gs_decode_done_us = gs_decode_us;
            break;
        }
    }
    try_publish_locked();
}

void FrameMatcher::on_display_submit(uint64_t gs_display_us, uint64_t /*now*/) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& f : ring_) {
        if (f.gs_display_submit_us == 0) {
            f.gs_display_submit_us = gs_display_us;
            break;
        }
    }
    try_publish_locked();
}

void FrameMatcher::on_msg_frame(uint32_t ssrc, uint32_t rtp_ts,
                                uint64_t capture_us, uint64_t frame_ready_us,
                                uint64_t last_pkt_send_us, uint64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    FrameTimings* slot = find_by_key_locked(ssrc, rtp_ts);
    if (!slot) {
        slot = &push_new_locked(now);
        slot->ssrc = ssrc;
        slot->rtp_ts = rtp_ts;
    }
    slot->capture_us        = capture_us;
    slot->frame_ready_us    = frame_ready_us;
    slot->last_pkt_send_us  = last_pkt_send_us;
    slot->sidecar_seen      = true;
    try_publish_locked();
}

void FrameMatcher::ttl_sweep(uint64_t now, uint64_t ttl_us) {
    std::lock_guard<std::mutex> lk(m_);
    while (!ring_.empty() && now - ring_.front().inserted_us > ttl_us) {
        ring_.pop_front();
    }
}

void FrameMatcher::try_publish_locked() {
    // Walk from front and publish any complete slot. We allow non-head
    // completion because sidecar_seen can land out-of-order vs. the
    // decode/display FIFO binding for adjacent frames.
    for (auto it = ring_.begin(); it != ring_.end(); ) {
        if (is_complete(*it)) {
            if (publish_) publish_(*it);
            it = ring_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace latency_probe
