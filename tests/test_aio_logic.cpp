#include <catch2/catch_test_macros.hpp>
#include "../src/osd_aio_logic.hpp"

TEST_CASE("logic unit links", "[aio]") {
    REQUIRE(std::string(aio::aio_logic_version()) == "aio-logic-1");
}
