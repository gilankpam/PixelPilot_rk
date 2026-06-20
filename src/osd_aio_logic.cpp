#include "osd_aio_logic.hpp"
#include <cmath>
#include <cstdio>

namespace aio {

const char* aio_logic_version() { return "aio-logic-1"; }

Band resolve_band(Metric m, double v) {
    switch (m) {
    case Metric::Link:
        if (v >= 70) return Band::Good;
        if (v >= 40) return Band::Warn;
        return Band::Crit;
    case Metric::Bitrate:
        if (v >= 15) return Band::Good;
        if (v >= 8)  return Band::Warn;
        return Band::Crit;
    case Metric::Latency: // lower is better
        if (v <= 50)  return Band::Good;
        if (v <= 100) return Band::Warn;
        return Band::Crit;
    case Metric::Rssi: // higher (less negative) is better
        if (v >= -70) return Band::Good;
        if (v >= -80) return Band::Warn;
        return Band::Crit;
    case Metric::Snr:
        if (v >= 12) return Band::Good;
        if (v >= 6)  return Band::Warn;
        return Band::Crit;
    }
    return Band::Neutral;
}

Rgba resolve_color(Band band, Scheme scheme, bool is_neutral) {
    const Rgba white{1, 1, 1, 1};
    if (scheme == Scheme::White || is_neutral || band == Band::Neutral) return white;
    switch (band) {
    case Band::Good: return Rgba{0x1f / 255.0, 0xe0 / 255.0, 0x84 / 255.0, 1};
    case Band::Warn: return Rgba{0xff / 255.0, 0xb3 / 255.0, 0x00 / 255.0, 1};
    case Band::Crit: return Rgba{0xff / 255.0, 0x2e / 255.0, 0x3e / 255.0, 1};
    default:         return white;
    }
}

int link_quality_pct(long all, long lost, long fec_rec) {
    if (all <= 0) return 0;
    long clean = all - lost - fec_rec;
    if (clean < 0) clean = 0;
    long pct = std::lround(100.0 * static_cast<double>(clean) / static_cast<double>(all));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return static_cast<int>(pct);
}


std::optional<int> freq_to_channel(int f) {
    if (f == 2484) return 14;                                  // 2.4 GHz ch 14
    if (f >= 2412 && f <= 2472 && (f - 2407) % 5 == 0)
        return (f - 2407) / 5;                                 // 2.4 GHz ch 1..13
    if (f >= 5150 && f <= 5895 && (f - 5000) % 5 == 0)
        return (f - 5000) / 5;                                 // 5 GHz
    return std::nullopt;
}

std::string format_timecode(long s) {
    if (s < 0) s = 0;
    long h = s / 3600;
    long m = (s % 3600) / 60;
    long sec = s % 60;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", h, m, sec);
    return std::string(buf);
}

void AntennaAggregator::update(const std::string& ant_id, long value, long now_ms) {
    entries_[ant_id] = Entry{value, now_ms};
}

std::optional<long> AntennaAggregator::best(long now_ms) const {
    std::optional<long> result;
    for (const auto& [id, e] : entries_) {
        if (now_ms - e.last_ms > stale_ms_) continue; // stale
        if (!result || e.value > *result) result = e.value;
    }
    return result;
}

std::size_t AntennaAggregator::live_count(long now_ms) const {
    std::size_t n = 0;
    for (const auto& [id, e] : entries_)
        if (now_ms - e.last_ms <= stale_ms_) ++n;
    return n;
}

std::string format_video_mode(const std::string& resolution, int fps) {
    if (resolution.empty()) return "";
    auto xpos = resolution.find('x');
    if (xpos == std::string::npos) return resolution;       // not WxH -> raw
    std::string h = resolution.substr(xpos + 1);
    if (h.empty()) return resolution;
    for (char c : h) if (c < '0' || c > '9') return resolution; // non-numeric -> raw
    std::string out = h + "p";
    if (fps > 0) out += std::to_string(fps);
    return out;
}

int rssi_to_bars(int rssi_dbm) {
    const double lo = -90.0, hi = -55.0;
    double f = (static_cast<double>(rssi_dbm) - lo) / (hi - lo);
    int n = static_cast<int>(std::lround(f * 5.0));
    if (n < 0) n = 0;
    if (n > 5) n = 5;
    return n;
}

Band fps_band(int live_fps, int configured_fps) {
    if (configured_fps <= 0) return Band::Neutral;
    double ratio = static_cast<double>(live_fps) / static_cast<double>(configured_fps);
    if (ratio >= 0.90) return Band::Good;
    if (ratio >= 0.70) return Band::Warn;
    return Band::Crit;
}

} // namespace aio
