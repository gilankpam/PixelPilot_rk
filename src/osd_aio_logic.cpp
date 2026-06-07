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

} // namespace aio
