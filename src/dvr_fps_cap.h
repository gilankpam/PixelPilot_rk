#ifndef DVR_FPS_CAP_H
#define DVR_FPS_CAP_H

#include <cstdint>

// Frame-rate cap / decimator for the re-encode pacer. Emits at up to cap_fps:
// input above the cap is decimated (e.g. 90 -> ~60), input at or below passes
// through unchanged (variable-fps, made correct by PTS-delta muxing). The
// accumulator advances the target slot by one interval per emit so the rate
// tracks the cap without the undershoot of a naive "reset to now" throttle; a
// long gap resyncs the slot so silence can't cause a catch-up burst. No
// tolerance is used, so the rate never sustains above the cap (encoder-safe).
class FpsCap {
public:
    explicit FpsCap(int cap_fps)
        : interval_us_(cap_fps > 0 ? 1000000 / cap_fps : 0) {}

    bool should_emit(int64_t now_us) {
        if (interval_us_ <= 0) return true;          // no cap
        if (now_us < next_us_) return false;         // too soon -> drop
        next_us_ += interval_us_;
        if (next_us_ < now_us)                        // first frame or long gap
            next_us_ = now_us + interval_us_;         // resync, no backlog burst
        return true;
    }

private:
    int64_t interval_us_;
    int64_t next_us_ = INT64_MIN;
};

#endif // DVR_FPS_CAP_H
