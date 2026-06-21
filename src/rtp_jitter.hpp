#ifndef RTP_JITTER_HPP
#define RTP_JITTER_HPP

#include <cstdint>

// RFC 3550 §6.4.1 interarrival jitter for an RTP video stream (90 kHz clock).
// Fed one frame-marker arrival at a time. Clock-sync independent: the estimate
// is built from a difference-of-differences, so the unknown sender/receiver
// clock offset cancels and no NTP-style sync is required.
class RtpJitterEstimator {
public:
    // Feed one frame-marker arrival.
    //   ssrc       : RTP SSRC (stream identity; a change resets state)
    //   rtp_ts     : sender RTP timestamp (90 kHz ticks, uint32, wraps)
    //   gs_recv_us : receiver arrival time (monotonic microseconds)
    // Returns the current smoothed jitter estimate in milliseconds. The first
    // arrival after construction or an SSRC change has no reference and
    // returns 0.0.
    double update(uint32_t ssrc, uint32_t rtp_ts, uint64_t gs_recv_us) {
        if (!have_prev_ || ssrc != ssrc_) {
            ssrc_         = ssrc;
            prev_rtp_ts_  = rtp_ts;
            prev_recv_us_ = gs_recv_us;
            j_us_         = 0.0;
            have_prev_    = true;
            return 0.0;
        }
        // Signed 32-bit tick delta handles uint32 wrap-around correctly.
        int32_t ts_delta_ticks = static_cast<int32_t>(rtp_ts - prev_rtp_ts_);
        double  s_delta_us = static_cast<double>(ts_delta_ticks) * (1e6 / RTP_HZ);
        double  r_delta_us = static_cast<double>(gs_recv_us - prev_recv_us_);
        double  d_us = r_delta_us - s_delta_us;
        if (d_us < 0) d_us = -d_us;
        j_us_ += (d_us - j_us_) / 16.0;

        prev_rtp_ts_  = rtp_ts;
        prev_recv_us_ = gs_recv_us;
        return jitter_ms();
    }

    double jitter_ms() const { return j_us_ / 1000.0; }

private:
    bool     have_prev_    = false;
    uint32_t ssrc_         = 0;
    uint32_t prev_rtp_ts_  = 0;
    uint64_t prev_recv_us_ = 0;
    double   j_us_         = 0.0;   // RFC 3550 J, in microseconds
    static constexpr double RTP_HZ = 90000.0;
};

#endif // RTP_JITTER_HPP
