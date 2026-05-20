#ifndef VIDEO_STUTTER_HPP
#define VIDEO_STUTTER_HPP

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

// Returns true if `interval_ms` represents a stutter relative to the recent
// frame intervals. A stutter is `interval > factor * median(recent)`.
//
// During warm-up (fewer than 30 samples in `recent`), always returns false to
// avoid spurious classifications when the median is unstable.
//
// `recent` is treated as read-only. Median is computed via std::nth_element on
// a local copy — O(n) and run at most ~60 times/sec, negligible cost.
inline bool is_stutter(long interval_ms,
                       const std::deque<long>& recent,
                       double factor = 1.5) {
    if (recent.size() < 30) return false;

    std::vector<long> scratch(recent.begin(), recent.end());
    auto mid = scratch.begin() + scratch.size() / 2;
    std::nth_element(scratch.begin(), mid, scratch.end());
    long median = *mid;

    // Strict greater-than: equal-to-threshold is not a stutter.
    return static_cast<double>(interval_ms) > factor * static_cast<double>(median);
}

// Updates the decaying peak in-place. Two steps:
//   1) Expire: if peak is set and older than 10s, reset to (0, 0).
//   2) Promote: only if the new frame is classified as a stutter AND
//      its interval exceeds the current peak.
//
// The "only promote on stutter" rule keeps the ▲ readout meaningful — it
// shows the largest stutter-classified interval in the last 10s, not the
// natural max of normal jitter.
inline void update_peak(long& peak_ms,
                        uint64_t& peak_ts_ms,
                        long interval_ms,
                        bool is_stutter,
                        uint64_t now_ms) {
    constexpr uint64_t PEAK_TTL_MS = 10'000;

    if (peak_ms != 0 && now_ms - peak_ts_ms > PEAK_TTL_MS) {
        peak_ms = 0;
        peak_ts_ms = 0;
    }
    if (is_stutter && interval_ms > peak_ms) {
        peak_ms = interval_ms;
        peak_ts_ms = now_ms;
    }
}

#endif // VIDEO_STUTTER_HPP
