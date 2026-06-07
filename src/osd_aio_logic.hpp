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

// Link quality 0..100 from packet counters over a window.
int link_quality_pct(long pkt_all, long pkt_lost);

// Filled signal-bar count 0..5 from link quality %.
int signal_bar_count(int lq_pct);

// MHz -> WiFi channel number; nullopt if outside known 2.4/5 GHz grids.
std::optional<int> freq_to_channel(int freq_mhz);

// Elapsed seconds -> "HH:MM:SS" (zero-padded; negatives clamp to zero).
std::string format_timecode(long elapsed_s);

} // namespace aio
