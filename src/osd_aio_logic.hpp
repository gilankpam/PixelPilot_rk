#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace aio {

enum class Band { Good, Warn, Crit, Neutral };
enum class Metric { Link, Bitrate, Latency, Rssi, Snr };
enum class Scheme { Accent, White };

struct Rgba {
    double r, g, b, a; // each 0..1
    bool operator==(const Rgba& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
};

// Returns the version string so we can prove the unit links. Replaced by real
// functions in later tasks.
const char* aio_logic_version();

// Threshold band for a metric value (units as displayed: Mb/s, ms, dBm, dB, %).
Band resolve_band(Metric m, double value);

// Final value/threshold color. is_neutral marks informational tiles (VIDEO, WIFI CH)
// which are always white. White scheme returns white for every band.
Rgba resolve_color(Band band, Scheme scheme, bool is_neutral);

} // namespace aio
