#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "conn_state.h"

TEST_CASE("parse: connected with reason and sinceMs", "[conn_state]") {
    conn_state_t s;
    const char *j = R"({"connection":{"enabled":true,"state":"connected","reason":"","sinceMs":1234}})";
    REQUIRE(conn_state_parse(j, &s) == true);
    REQUIRE(s.state == CONN_CONNECTED);
    REQUIRE(s.since_ms == 1234);
}

TEST_CASE("parse: armed and disconnected map through", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(R"({"connection":{"state":"armed"}})", &s));
    REQUIRE(s.state == CONN_ARMED);
    REQUIRE(conn_state_parse(R"({"connection":{"state":"disconnected","reason":"timeout"}})", &s));
    REQUIRE(s.state == CONN_DISCONNECTED);
    REQUIRE(std::strcmp(s.reason, "timeout") == 0);
}

TEST_CASE("parse: missing connection block -> UNKNOWN, false", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(R"({"runner":{"running":true}})", &s) == false);
    REQUIRE(s.state == CONN_UNKNOWN);
    REQUIRE(s.since_ms == -1);
    REQUIRE(s.reason[0] == '\0');
}

TEST_CASE("parse: garbage / unknown state -> UNKNOWN, false", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse("not json", &s) == false);
    REQUIRE(s.state == CONN_UNKNOWN);
    REQUIRE(conn_state_parse(R"({"connection":{"state":"bogus"}})", &s) == false);
    REQUIRE(s.state == CONN_UNKNOWN);
}

TEST_CASE("parse: sinceMs null -> -1", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(R"({"connection":{"state":"connected","sinceMs":null}})", &s));
    REQUIRE(s.since_ms == -1);
}

TEST_CASE("get returns the last ingested snapshot", "[conn_state]") {
    conn_state_stop();   /* reset */
    conn_state_ingest(CONN_CONNECTED, "", 7);
    REQUIRE(conn_state_get().state == CONN_CONNECTED);
    conn_state_stop();
}
