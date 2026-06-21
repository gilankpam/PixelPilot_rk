#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "conn_state.h"

TEST_CASE("parse: connected with reason and sinceMs", "[conn_state]") {
    conn_state_t s;
    const char *j = R"({"connection":{"enabled":true,"state":"connected","reason":"","sinceMs":1234}})";
    REQUIRE(conn_state_parse(j, &s) == true);
    REQUIRE(s.state == CONN_CONNECTED);
    REQUIRE(s.since_ms == 1234);
    REQUIRE(s.updated_ms == 0);   /* parse must never read the clock */
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

TEST_CASE("parse: NULL input -> false, state == CONN_UNKNOWN", "[conn_state]") {
    conn_state_t s;
    REQUIRE(conn_state_parse(NULL, &s) == false);
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
    REQUIRE(conn_state_get().updated_ms > 0);   /* ingest stamps CLOCK_MONOTONIC */
    conn_state_stop();
}

namespace {
struct Counter { int calls = 0; conn_state_e last = CONN_UNKNOWN; };
void count_cb(const conn_state_t *st, void *ud) {
    auto *c = static_cast<Counter *>(ud);
    c->calls++; c->last = st->state;
}
}

TEST_CASE("subscribe delivers current state immediately", "[conn_state]") {
    conn_state_stop();
    conn_state_ingest(CONN_CONNECTED, "", -1);
    Counter c;
    int tok = conn_state_subscribe(count_cb, &c);
    REQUIRE(tok >= 0);
    REQUIRE(c.calls == 1);
    REQUIRE(c.last == CONN_CONNECTED);
    conn_state_unsubscribe(tok);
    conn_state_stop();
}

TEST_CASE("ingest notifies only on state change", "[conn_state]") {
    conn_state_stop();
    Counter c;
    int tok = conn_state_subscribe(count_cb, &c);   /* +1 immediate (UNKNOWN) */
    conn_state_ingest(CONN_CONNECTED, "", -1);       /* change -> +1 */
    conn_state_ingest(CONN_CONNECTED, "still", 5);   /* same state -> no fire */
    conn_state_ingest(CONN_DISCONNECTED, "drop", -1);/* change -> +1 */
    REQUIRE(c.calls == 3);
    REQUIRE(c.last == CONN_DISCONNECTED);
    conn_state_unsubscribe(tok);
    conn_state_stop();
}

TEST_CASE("unsubscribe stops delivery", "[conn_state]") {
    conn_state_stop();
    Counter c;
    int tok = conn_state_subscribe(count_cb, &c);    /* +1 immediate */
    conn_state_unsubscribe(tok);
    conn_state_ingest(CONN_CONNECTED, "", -1);        /* no delivery */
    REQUIRE(c.calls == 1);
    conn_state_stop();
}
