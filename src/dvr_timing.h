#ifndef DVR_TIMING_H
#define DVR_TIMING_H

#include <cstddef>
#include <cstdint>

// True if an Annex-B buffer contains at least one VCL (coded-slice) NAL, i.e. it
// is a real frame rather than a parameter-set-only buffer (VPS/SPS/PPS). The
// encoder emits headers as their own buffers carrying the same timestamp as the
// next frame, so only VCL buffers may advance the DVR's frame-duration timeline.
//   H264: VCL nal_type in [1, 5]      (byte & 0x1f)
//   H265: VCL nal_type in [0, 31]     ((byte >> 1) & 0x3f)
// Handles both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
inline bool dvr_buffer_has_vcl(const uint8_t *data, size_t len, bool is_h265) {
    if (!data || len < 4) return false;
    for (size_t i = 0; i + 3 < len; ) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            uint8_t hdr = data[i + 3];
            int t = is_h265 ? ((hdr >> 1) & 0x3f) : (hdr & 0x1f);
            bool vcl = is_h265 ? (t <= 31) : (t >= 1 && t <= 5);
            if (vcl) return true;
            i += 3;
        } else {
            i++;
        }
    }
    return false;
}

// Compute an MP4 sample duration in 90 kHz units from a frame's capture
// timestamp (ms) and the previous frame's timestamp (ms).
//
// The re-encode pacer pushes frames on a best-effort timer whose long-term rate
// drifts slightly below nominal, so stamping every frame with a fixed 90000/fps
// makes the file shorter than wall-clock and it plays back faster. Deriving the
// duration from the real capture-time delta keeps the recorded duration honest
// regardless of pacing jitter.
//
//   last_pts_ms < 0       -> first frame of a segment: use nominal 1/fps.
//   delta <= 0            -> duplicate / non-monotonic timestamp: use nominal.
//   otherwise             -> delta_ms * 90, clamped to [1 ms, 1 s] so a single
//                            glitched timestamp can't wreck the timeline.
inline int64_t dvr_frame_duration_90k(int64_t pts_ms, int64_t last_pts_ms, int fps) {
    const int64_t nominal = (fps > 0) ? (int64_t)90000 / fps : 3000;
    if (last_pts_ms < 0) return nominal;            // first frame of a segment
    const int64_t delta_ms = pts_ms - last_pts_ms;
    if (delta_ms <= 0) return nominal;              // duplicate / regressing ts
    int64_t dur = delta_ms * 90;                    // ms -> 90 kHz
    const int64_t lo = 90;                          // 1 ms floor
    const int64_t hi = 90000;                       // 1 s ceiling
    if (dur < lo) dur = lo;
    if (dur > hi) dur = hi;
    return dur;
}

// Raw-DVR per-frame MP4 duration (90 kHz) from consecutive RTP timestamps
// (uint32, 90 kHz, wrap-safe forward delta). First frame, duplicate timestamp,
// or a gap over 1 s falls back to the nominal 1/fallback_fps.
inline int dvr_rtp_duration_90k(uint32_t ts, uint32_t last_ts, bool have_last, int fallback_fps) {
    const int nominal = (fallback_fps > 0) ? 90000 / fallback_fps : 1500;
    if (!have_last) return nominal;
    uint32_t d = ts - last_ts;                  // wrap-safe forward delta
    if (d == 0 || d > 90000) return nominal;    // duplicate ts / >1 s gap
    return (int)d;
}

#endif // DVR_TIMING_H
