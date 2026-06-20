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
        cb_(au_.data(), au_.size());
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

bool HevcDepayloader::handle_ap(const uint8_t* p, size_t len) { (void)p; (void)len; return true; }
bool HevcDepayloader::handle_fu(const uint8_t* p, size_t len) { (void)p; (void)len; return true; }

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
