#ifndef HEVC_DEPAYLOADER_H
#define HEVC_DEPAYLOADER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Pure RFC 7798 HEVC RTP depayloader. No sockets, threads, gstreamer, or MPP.
// Feed RTP *payloads* (bytes after the 12-byte RTP header) plus the marker bit and
// RTP timestamp; it emits one complete Annex-B access unit per frame via the
// callback (4-byte start codes, parameter sets ensured before each IRAP).
class HevcDepayloader {
public:
    using FrameCallback = std::function<void(const uint8_t* au, size_t len, uint32_t rtp_ts)>;
    explicit HevcDepayloader(FrameCallback on_access_unit);

    // Returns false if this payload was malformed (dropped); true otherwise.
    bool on_payload(const uint8_t* payload, size_t len, bool marker, uint32_t rtp_ts);
    // Receiver detected an RTP sequence gap: drop partial FU + mark AU corrupt.
    void on_discontinuity();
    // Clear parameter-set cache and pending AU (stream restart).
    void reset();

    struct Stats {
        uint64_t malformed = 0;
        uint64_t fu_drops = 0;
        uint64_t aus_emitted = 0;
        uint64_t param_sets_reinserted = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    void append_nal_with_startcode(const uint8_t* nal, size_t len);
    void flush_au();
    void handle_single_nal(const uint8_t* p, size_t len);
    bool handle_ap(const uint8_t* p, size_t len);
    bool handle_fu(const uint8_t* p, size_t len);

    FrameCallback cb_;
    std::vector<uint8_t> au_;   // current access unit (Annex-B)
    std::vector<uint8_t> fu_;   // in-progress FU NAL (Annex-B, incl. rebuilt header)
    bool fu_active_ = false;
    bool au_has_data_ = false;
    bool au_corrupt_ = false;
    bool have_ts_ = false;
    uint32_t cur_ts_ = 0;
    bool au_has_irap_ = false;
    bool au_has_vps_ = false, au_has_sps_ = false, au_has_pps_ = false;
    std::vector<uint8_t> vps_, sps_, pps_;  // cached most-recent param sets (no start code)
    Stats stats_;
};

#endif // HEVC_DEPAYLOADER_H
