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

#endif // VIDEO_STUTTER_HPP
