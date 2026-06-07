#include <catch2/catch_test_macros.hpp>
#include "../src/osd_aio_logic.hpp"

TEST_CASE("logic unit links", "[aio]") {
    REQUIRE(std::string(aio::aio_logic_version()) == "aio-logic-1");
}

TEST_CASE("resolve_band boundaries", "[aio]") {
    using aio::Band; using aio::Metric; using aio::resolve_band;

    // LINK %: >=70 good, 40..69 warn, <40 crit
    REQUIRE(resolve_band(Metric::Link, 70) == Band::Good);
    REQUIRE(resolve_band(Metric::Link, 69) == Band::Warn);
    REQUIRE(resolve_band(Metric::Link, 40) == Band::Warn);
    REQUIRE(resolve_band(Metric::Link, 39) == Band::Crit);

    // BITRATE: >=15 good, 8..14.9 warn, <8 crit
    REQUIRE(resolve_band(Metric::Bitrate, 15) == Band::Good);
    REQUIRE(resolve_band(Metric::Bitrate, 14.9) == Band::Warn);
    REQUIRE(resolve_band(Metric::Bitrate, 8) == Band::Warn);
    REQUIRE(resolve_band(Metric::Bitrate, 7.9) == Band::Crit);

    // LATENCY: <=50 good, 51..100 warn, >100 crit (lower is better)
    REQUIRE(resolve_band(Metric::Latency, 50) == Band::Good);
    REQUIRE(resolve_band(Metric::Latency, 51) == Band::Warn);
    REQUIRE(resolve_band(Metric::Latency, 100) == Band::Warn);
    REQUIRE(resolve_band(Metric::Latency, 101) == Band::Crit);

    // RSSI: >=-70 good, -71..-80 warn, <-80 crit (higher is better)
    REQUIRE(resolve_band(Metric::Rssi, -70) == Band::Good);
    REQUIRE(resolve_band(Metric::Rssi, -71) == Band::Warn);
    REQUIRE(resolve_band(Metric::Rssi, -80) == Band::Warn);
    REQUIRE(resolve_band(Metric::Rssi, -81) == Band::Crit);

    // SNR: >=12 good, 6..11 warn, <6 crit
    REQUIRE(resolve_band(Metric::Snr, 12) == Band::Good);
    REQUIRE(resolve_band(Metric::Snr, 11) == Band::Warn);
    REQUIRE(resolve_band(Metric::Snr, 6) == Band::Warn);
    REQUIRE(resolve_band(Metric::Snr, 5) == Band::Crit);
}

TEST_CASE("resolve_color schemes", "[aio]") {
    using aio::Band; using aio::Scheme; using aio::Rgba; using aio::resolve_color;
    const Rgba white{1, 1, 1, 1};
    const Rgba green{0x1f / 255.0, 0xe0 / 255.0, 0x84 / 255.0, 1};
    const Rgba amber{0xff / 255.0, 0xb3 / 255.0, 0x00 / 255.0, 1};
    const Rgba red  {0xff / 255.0, 0x2e / 255.0, 0x3e / 255.0, 1};

    // White scheme: everything white regardless of band.
    REQUIRE(resolve_color(Band::Good, Scheme::White, false) == white);
    REQUIRE(resolve_color(Band::Crit, Scheme::White, false) == white);
    REQUIRE(resolve_color(Band::Neutral, Scheme::White, true) == white);

    // Accent scheme: threshold palette for metrics, white for neutral tiles.
    REQUIRE(resolve_color(Band::Good, Scheme::Accent, false) == green);
    REQUIRE(resolve_color(Band::Warn, Scheme::Accent, false) == amber);
    REQUIRE(resolve_color(Band::Crit, Scheme::Accent, false) == red);
    REQUIRE(resolve_color(Band::Neutral, Scheme::Accent, true) == white);
}

TEST_CASE("link_quality_pct", "[aio]") {
    using aio::link_quality_pct;
    REQUIRE(link_quality_pct(0, 0) == 0);       // no traffic -> 0%
    REQUIRE(link_quality_pct(100, 0) == 100);   // perfect
    REQUIRE(link_quality_pct(100, 100) == 0);   // all lost
    REQUIRE(link_quality_pct(100, 8) == 92);    // nominal sample
    REQUIRE(link_quality_pct(100, 200) == 0);   // clamp: lost > all
}

TEST_CASE("signal_bar_count", "[aio]") {
    using aio::signal_bar_count;
    REQUIRE(signal_bar_count(0) == 0);
    REQUIRE(signal_bar_count(100) == 5);
    REQUIRE(signal_bar_count(92) == 5);   // round(4.6)
    REQUIRE(signal_bar_count(41) == 2);   // round(2.05)
    REQUIRE(signal_bar_count(50) == 3);   // round(2.5)
    REQUIRE(signal_bar_count(-10) == 0);  // clamp low
    REQUIRE(signal_bar_count(150) == 5);  // clamp high
}

TEST_CASE("freq_to_channel", "[aio]") {
    using aio::freq_to_channel;
    REQUIRE(freq_to_channel(5745) == std::optional<int>(149));
    REQUIRE(freq_to_channel(5180) == std::optional<int>(36));
    REQUIRE(freq_to_channel(2412) == std::optional<int>(1));
    REQUIRE(freq_to_channel(2472) == std::optional<int>(13));
    REQUIRE(freq_to_channel(2484) == std::optional<int>(14));
    REQUIRE(freq_to_channel(3000) == std::nullopt); // out of band -> caller shows raw MHz
    REQUIRE(freq_to_channel(5183) == std::nullopt); // not on a 5 MHz grid
}

TEST_CASE("format_timecode", "[aio]") {
    using aio::format_timecode;
    REQUIRE(format_timecode(0) == "00:00:00");
    REQUIRE(format_timecode(8) == "00:00:08");
    REQUIRE(format_timecode(872) == "00:14:32");   // handoff REC sample
    REQUIRE(format_timecode(3661) == "01:01:01");
    REQUIRE(format_timecode(-5) == "00:00:00");     // clamp negatives
}

TEST_CASE("AntennaAggregator best + staleness", "[aio]") {
    aio::AntennaAggregator agg(2500); // 2500 ms stale window

    REQUIRE(agg.best(0) == std::nullopt);          // empty
    agg.update("0", -70, 1000);
    agg.update("1", -62, 1000);
    agg.update("256", -85, 1000);
    REQUIRE(agg.best(1000) == std::optional<long>(-62)); // max across antennas
    REQUIRE(agg.live_count(1000) == 3u);

    // Antenna "1" goes stale (no refresh); others refreshed at 4000ms.
    agg.update("0", -71, 4000);
    agg.update("256", -88, 4000);
    REQUIRE(agg.best(4000) == std::optional<long>(-71)); // "1" (-62) evicted as stale
    REQUIRE(agg.live_count(4000) == 2u);

    // Everything stale -> nullopt.
    REQUIRE(agg.best(10000) == std::nullopt);
    REQUIRE(agg.live_count(10000) == 0u);
}
