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


// MHz -> WiFi channel number; nullopt if outside known 2.4/5 GHz grids.
std::optional<int> freq_to_channel(int freq_mhz);

// Elapsed seconds -> "HH:MM:SS" (zero-padded; negatives clamp to zero).
std::string format_timecode(long elapsed_s);

// Tracks the latest value per antenna id and reports the best (max) across
// antennas seen within `stale_ms`. Pure: the caller supplies the clock (now_ms).
class AntennaAggregator {
public:
    explicit AntennaAggregator(long stale_ms = 2500) : stale_ms_(stale_ms) {}
    void update(const std::string& ant_id, long value, long now_ms);
    std::optional<long> best(long now_ms) const; // max over live antennas
    std::size_t live_count(long now_ms) const;
private:
    struct Entry { long value; long last_ms; };
    std::map<std::string, Entry> entries_;
    long stale_ms_;
};

// "1920x1080", 60 -> "1080p60". Parses the height after 'x'; if the string isn't
// WxH with a numeric height, returns it unchanged; if fps <= 0 the fps suffix is
// omitted; empty input -> "".
std::string format_video_mode(const std::string& resolution, int fps);

// RSSI dBm -> 0..5 bars, linear: -90 dBm -> 0, -55 dBm -> 5, clamped.
int rssi_to_bars(int rssi_dbm);

// Live vs configured fps -> band. configured <= 0 -> Neutral (no reference).
// ratio >= 0.90 -> Good; >= 0.70 -> Warn; else Crit.
Band fps_band(int live_fps, int configured_fps);

} // namespace aio
