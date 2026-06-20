#include "hevc_depayloader.h"
#include <cstring>

HevcDepayloader::HevcDepayloader(FrameCallback on_access_unit)
    : cb_(std::move(on_access_unit)) {}

void HevcDepayloader::append_nal_with_startcode(const uint8_t* nal, size_t len) {
    if (len == 0) return;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    au_.insert(au_.end(), sc, sc + 4);
    au_.insert(au_.end(), nal, nal + len);
    au_has_data_ = true;
    const uint8_t t = (nal[0] >> 1) & 0x3F;
    if (t == 32) { vps_.assign(nal, nal + len); au_has_vps_ = true; }
    else if (t == 33) { sps_.assign(nal, nal + len); au_has_sps_ = true; }
    else if (t == 34) { pps_.assign(nal, nal + len); au_has_pps_ = true; }
    if (t >= 16 && t <= 21) au_has_irap_ = true;
}

void HevcDepayloader::flush_au() {
    if (au_has_data_ && !au_corrupt_) {
        const bool need_ps = au_has_irap_ &&
            !(au_has_vps_ && au_has_sps_ && au_has_pps_) &&
            !vps_.empty() && !sps_.empty() && !pps_.empty();
        if (need_ps) {
            static const uint8_t sc[4] = {0, 0, 0, 1};
            std::vector<uint8_t> out;
            out.reserve(au_.size() + vps_.size() + sps_.size() + pps_.size() + 12);
            auto add = [&](const std::vector<uint8_t>& n) {
                out.insert(out.end(), sc, sc + 4);
                out.insert(out.end(), n.begin(), n.end());
            };
            add(vps_); add(sps_); add(pps_);
            out.insert(out.end(), au_.begin(), au_.end());
            cb_(out.data(), out.size());
            stats_.param_sets_reinserted++;
        } else {
            cb_(au_.data(), au_.size());
        }
        stats_.aus_emitted++;
    }
    au_.clear();
    au_has_data_ = false;
    au_corrupt_ = false;
    au_has_irap_ = false;
    au_has_vps_ = au_has_sps_ = au_has_pps_ = false;
    have_ts_ = false;
    if (fu_active_) { stats_.fu_drops++; fu_active_ = false; fu_.clear(); }
}

void HevcDepayloader::handle_single_nal(const uint8_t* p, size_t len) {
    append_nal_with_startcode(p, len);
}

bool HevcDepayloader::handle_ap(const uint8_t* p, size_t len) {
    size_t off = 2;  // skip 2-byte AP payload header
    while (off + 2 <= len) {
        const size_t nal_size = (size_t(p[off]) << 8) | p[off + 1];
        off += 2;
        if (nal_size == 0 || off + nal_size > len) {
            stats_.malformed++;
            au_corrupt_ = true;
            return false;
        }
        append_nal_with_startcode(p + off, nal_size);
        off += nal_size;
    }
    return true;
}

bool HevcDepayloader::handle_fu(const uint8_t* p, size_t len) {
    if (len < 3) { stats_.malformed++; return false; }  // 2-byte hdr + 1-byte FU hdr
    const uint8_t fuh = p[2];
    const bool start = (fuh & 0x80) != 0;
    const bool end   = (fuh & 0x40) != 0;
    const uint8_t fu_type = fuh & 0x3F;
    const uint8_t* frag = p + 3;
    const size_t frag_len = len - 3;

    if (start) {
        if (fu_active_) { stats_.fu_drops++; au_corrupt_ = true; }  // lost End of prior FU
        fu_.clear();
        // Rebuild the 2-byte NAL header: keep forbidden/layer/tid, set type=fu_type.
        fu_.push_back(uint8_t((p[0] & 0x81) | (fu_type << 1)));
        fu_.push_back(p[1]);
        fu_.insert(fu_.end(), frag, frag + frag_len);
        fu_active_ = true;
    } else {
        if (!fu_active_) { stats_.fu_drops++; au_corrupt_ = true; return false; }  // lost Start
        fu_.insert(fu_.end(), frag, frag + frag_len);
    }

    if (end && fu_active_) {
        append_nal_with_startcode(fu_.data(), fu_.size());
        fu_active_ = false;
        fu_.clear();
    }
    return true;
}

bool HevcDepayloader::on_payload(const uint8_t* p, size_t len, bool marker, uint32_t rtp_ts) {
    if (len < 2) { stats_.malformed++; return false; }
    if (have_ts_ && rtp_ts != cur_ts_) flush_au();   // lost-marker fallback
    cur_ts_ = rtp_ts;
    have_ts_ = true;

    const uint8_t type = (p[0] >> 1) & 0x3F;
    bool ok = true;
    if (type <= 47)      handle_single_nal(p, len);
    else if (type == 48) ok = handle_ap(p, len);
    else if (type == 49) ok = handle_fu(p, len);
    else { stats_.malformed++; ok = false; }

    if (marker) flush_au();
    return ok;
}

void HevcDepayloader::on_discontinuity() {
    if (fu_active_) { stats_.fu_drops++; fu_active_ = false; fu_.clear(); }
    au_corrupt_ = true;
}

void HevcDepayloader::reset() {
    au_.clear(); fu_.clear();
    fu_active_ = au_has_data_ = au_corrupt_ = have_ts_ = au_has_irap_ = false;
    au_has_vps_ = au_has_sps_ = au_has_pps_ = false;
    vps_.clear(); sps_.clear(); pps_.clear();
    cur_ts_ = 0;
}
