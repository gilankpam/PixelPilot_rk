/* tests/test_settings_router.cpp */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "gsmenu/settings.h"
#include "gsmenu/settings_router_internal.h"
}

namespace {

struct FakeChild {
    std::vector<std::tuple<std::string,std::string,std::string,std::string>> sets;
    int next_rc = 0;
    const char *next_err = nullptr;
    bool connected = true;
    bool locked    = false;

    void on_set_async(const char *d, const char *p, const char *k, const char *v,
                      pp_settings_done_cb cb, void *ud) {
        sets.push_back({d, p, k, v});
        if (cb) cb(next_rc, next_err, ud);
    }
};

FakeChild g_drone, g_gs;

extern "C" {
    static void drone_set_async(const char *d, const char *p, const char *k, const char *v,
                                pp_settings_done_cb cb, void *ud) { g_drone.on_set_async(d,p,k,v,cb,ud); }
    static void drone_set(const char *d, const char *p, const char *k, const char *v) { drone_set_async(d,p,k,v,nullptr,nullptr); }
    static char *drone_get(const char *d, const char *p, const char *k) { (void)d;(void)p;(void)k; return strdup("DRONE"); }
    static bool drone_is_connected(void) { return g_drone.connected; }
    static bool drone_is_locked(const char *d, const char *p, const char *k) { (void)d;(void)p;(void)k; return g_drone.locked; }

    static void gs_set_async(const char *d, const char *p, const char *k, const char *v,
                              pp_settings_done_cb cb, void *ud) { g_gs.on_set_async(d,p,k,v,cb,ud); }
    static void gs_set(const char *d, const char *p, const char *k, const char *v) { gs_set_async(d,p,k,v,nullptr,nullptr); }
    static char *gs_get(const char *d, const char *p, const char *k) { (void)d;(void)p;(void)k; return strdup("GS"); }
    static bool gs_is_connected(void) { return g_gs.connected; }
}

static pp_settings_provider_t DRONE = {
    .set = drone_set, .get = drone_get, .set_async = drone_set_async,
    .is_locked = drone_is_locked, .is_connected = drone_is_connected,
    .set_snapshot_listener = nullptr, .set_visibility = nullptr,
};
static pp_settings_provider_t GS = {
    .set = gs_set, .get = gs_get, .set_async = gs_set_async,
    .is_locked = nullptr, .is_connected = gs_is_connected,
    .set_snapshot_listener = nullptr, .set_visibility = nullptr,
};

struct DoneCapture { int rc = 99; std::string err; };
static void capture_done(int rc, const char *err, void *ud) {
    auto *c = (DoneCapture *)ud;
    c->rc = rc; c->err = err ? err : "";
}

} // namespace

TEST_CASE("router: non-fanout drone key hits only drone", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("air", "camera", "fps", "60", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.empty());
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: gs-only key hits only gs", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "link", "rx_power", "70", capture_done, &cap);
    REQUIRE(g_drone.sets.empty());
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(std::get<2>(g_gs.sets[0]) == "rx_power");
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: fan-out channel writes drone then gs", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "36", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(std::get<3>(g_drone.sets[0]) == "36");
    REQUIRE(std::get<3>(g_gs.sets[0]) == "36");
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: fan-out aborts gs when drone fails", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    g_drone.next_rc = -1; g_drone.next_err = "Drone unreachable";
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "wfbng", "bandwidth", "40", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.empty());
    REQUIRE(cap.rc == -1);
    REQUIRE(cap.err == "Drone unreachable");
    pp_router_reset();
}

TEST_CASE("router: fan-out reports gs error when drone ok but gs fails", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    g_gs.next_rc = -1; g_gs.next_err = "GS write failed";
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("air", "camera", "codec", "h264", capture_done, &cap);
    REQUIRE(g_drone.sets.size() == 1);
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(cap.rc == -1);
    REQUIRE(cap.err.find("GS") != std::string::npos);
    pp_router_reset();
}

TEST_CASE("router: get delegates by domain", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    char *a = pp_settings_get("air", "camera", "fps"); REQUIRE(std::string(a) == "DRONE"); free(a);
    char *g = pp_settings_get("gs", "link", "rx_power"); REQUIRE(std::string(g) == "GS");   free(g);
    pp_router_reset();
}

TEST_CASE("router: gs-actions key routes to gs child only", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    DoneCapture cap;
    pp_settings_set_async("gs", "actions", "restart_pixelpilot", "trigger", capture_done, &cap);
    REQUIRE(g_drone.sets.empty());
    REQUIRE(g_gs.sets.size() == 1);
    REQUIRE(std::get<2>(g_gs.sets[0]) == "restart_pixelpilot");
    REQUIRE(cap.rc == 0);
    pp_router_reset();
}

TEST_CASE("router: is_connected = drone AND gs", "[router]") {
    g_drone = FakeChild{}; g_gs = FakeChild{};
    pp_router_install_children(&DRONE, &GS);
    REQUIRE(pp_settings_is_connected() == true);
    g_drone.connected = false;
    REQUIRE(pp_settings_is_connected() == false);
    g_drone.connected = true; g_gs.connected = false;
    REQUIRE(pp_settings_is_connected() == false);
    pp_router_reset();
}
