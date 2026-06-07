#include "osd_aio_logic.hpp"

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

} // namespace aio
