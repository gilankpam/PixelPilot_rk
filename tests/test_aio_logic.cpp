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
