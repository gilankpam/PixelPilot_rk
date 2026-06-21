#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/settings_fpvd_internal.h"
#include "conn_state.h"
}

static void ensure_lv_init() {
    static bool done = false;
    if (done) return;
    lv_init();
    done = true;
}

/* Mock fpvd-GS: /gs/config, /gs/apply, /gs/status, and the /air/* proxy. */
struct GsMockServer {
    httplib::Server svr;
    std::thread     th;
    int             port = 0;

    std::mutex mu;
    std::vector<std::string> log;   /* "METHOD path" in arrival order */
    std::string last_gs_patch_body, last_air_patch_body;

    std::atomic<bool> drone_up{true};      /* false => /air/* returns 502 */
    std::atomic<int>  air_get_override{0}; /* !=0 => GET /air/config returns this status */
    std::atomic<int>  gs_apply_fail_n{0};  /* fail this many /gs/apply calls (500) */
    std::atomic<int>  gs_apply_400_n{0};   /* fail this many /gs/apply calls (400) */
    std::atomic<int>  gs_apply_502_n{0};   /* fail this many /gs/apply calls (502) */
    std::string air_patch_error;           /* if set, PATCH /air/config -> 400 */

    std::string gs_config_response =
        R"({"link":{"channel":132,"width":40,"txPowerDbm":25,"region":"US",)"
        R"("linkId":7669206,"beamforming":{"enabled":false},"wlans":"auto"},)"
        R"("pixelpilot":{"videoScale":1.0,"screenMode":"1920x1080@60",)"
        R"("dvr":{"osd":true,"mode":"reencode","reencBitrate":50000}}})";
    std::string gs_status_response =
        R"({"fpvd":{"version":"0.1.0","uptimeMs":1000},)"
        R"("runner":{"running":true,"pid":1,"restarts":0},)"
        R"("radio":[{"wlan":"wlx0","type":"monitor","channel":132,)"
        R"("freqMhz":5660,"widthMhz":40,"txpowerDbm":19.0}],)"
        R"("link":{"linkId":7669206},)"
        R"("beamforming":{"enabled":false,"localMac":"84:fc:14:6c:36:e6"}})";
    std::string air_response =
        R"({"link":{"channel":132,"width":40,"txPowerDbm":20,"mcs":2,)"
        R"("fec":{"k":8,"n":12},"stbc":true,"ldpc":false},)"
        R"("video":{"codec":"h265","resolution":"1920x1080","fps":60,)"
        R"("bitrate":8192,"rcMode":"cbr","gopSize":1.0,"qpDelta":-4,)"
        R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
        R"("image":{"mirror":false,"flip":false,"rotate":0},)"
        R"("recording":{"enabled":false,"maxSeconds":300,"maxMB":500},)"
        R"("dynamicLink":{"enabled":false,"safe":{"mcs":1,"bitrateKbps":2000}}})";

    void note(const char *m, const std::string &p) {
        std::lock_guard<std::mutex> g(mu);
        log.push_back(std::string(m) + " " + p);
    }
    void start() {
        svr.Get("/gs/config", [&](const httplib::Request &, httplib::Response &res) {
            note("GET", "/gs/config");
            res.set_content(gs_config_response, "application/json");
        });
        svr.Patch("/gs/config", [&](const httplib::Request &req, httplib::Response &res) {
            note("PATCH", "/gs/config");
            { std::lock_guard<std::mutex> g(mu); last_gs_patch_body = req.body; }
            res.set_content(gs_config_response, "application/json");
        });
        svr.Post("/gs/apply", [&](const httplib::Request &, httplib::Response &res) {
            note("POST", "/gs/apply");
            if (gs_apply_400_n > 0) {
                gs_apply_400_n--;
                res.status = 400;
                res.set_content(R"({"error":"bad config"})", "application/json");
                return;
            }
            if (gs_apply_502_n > 0) {
                gs_apply_502_n--;
                res.status = 502;
                res.set_content(R"({"error":"upstream failed"})", "application/json");
                return;
            }
            if (gs_apply_fail_n > 0) {
                gs_apply_fail_n--;
                res.status = 500;
                res.set_content(R"({"applied":false,"error":"runner failed; rolled back"})",
                                "application/json");
                return;
            }
            res.set_content(R"({"applied":true})", "application/json");
        });
        svr.Get("/gs/status", [&](const httplib::Request &, httplib::Response &res) {
            note("GET", "/gs/status");
            res.set_content(gs_status_response, "application/json");
        });
        svr.Get("/air/config", [&](const httplib::Request &, httplib::Response &res) {
            note("GET", "/air/config");
            if (air_get_override != 0) { res.status = air_get_override;
                res.set_content(R"({"error":"injected"})", "application/json");
                return; }
            if (!drone_up) { res.status = 502;
                res.set_content(R"({"error":"drone unreachable: timeout"})", "application/json");
                return; }
            res.set_content(air_response, "application/json");
        });
        svr.Patch("/air/config", [&](const httplib::Request &req, httplib::Response &res) {
            note("PATCH", "/air/config");
            if (!drone_up) { res.status = 502;
                res.set_content(R"({"error":"drone unreachable: timeout"})", "application/json");
                return; }
            if (!air_patch_error.empty()) {
                res.status = 400;
                res.set_content(air_patch_error, "application/json");
                return;
            }
            { std::lock_guard<std::mutex> g(mu); last_air_patch_body = req.body; }
            res.set_content(air_response, "application/json");
        });
        svr.Post("/air/apply", [&](const httplib::Request &, httplib::Response &res) {
            note("POST", "/air/apply");
            if (!drone_up) { res.status = 502;
                res.set_content(R"({"error":"drone unreachable: timeout"})", "application/json");
                return; }
            res.set_content(R"({"applied":true,"version":2,"restarted":["radio"]})",
                            "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([&] { svr.listen_after_bind(); });
        while (!svr.is_running())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void stop() { svr.stop(); if (th.joinable()) th.join(); }
    std::vector<std::string> snapshot_log() {
        std::lock_guard<std::mutex> g(mu); return log;
    }
    std::vector<std::string> writes_only() {
        std::vector<std::string> w;
        for (auto &l : snapshot_log())
            if (l.rfind("GET", 0) != 0) w.push_back(l);
        return w;
    }
};

static void install_provider_pointing_at(int port) {
    ensure_lv_init();
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("PP_FPVD_URL", url, 1);
    pp_settings_register_fpvd();
    conn_state_ingest(CONN_CONNECTED, "", -1);   /* default: drone link up */
}

/* Done-callback waiter: rc + error message, delivered via lv_async_call, so
 * we must pump lv_timer_handler() until the dispatch fires. */
struct DoneWaiter {
    std::atomic<int> called{0};
    std::atomic<int> rc{0};
    std::mutex mu;
    std::string err;

    static void cb(int rc, const char *err, void *ud) {
        auto *w = static_cast<DoneWaiter *>(ud);
        { std::lock_guard<std::mutex> g(w->mu); w->err = err ? err : ""; }
        w->rc = rc;
        w->called = 1;
    }
    bool wait(int ms = 20000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!called && std::chrono::steady_clock::now() < end) {
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return called != 0;
    }
    std::string err_str() { std::lock_guard<std::mutex> g(mu); return err; }
};

TEST_CASE("integration: reads route to the right endpoint", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);   /* primes snapshots synchronously */
    REQUIRE(pp_settings_is_connected() == true);

    char *chan = pp_settings_get("gs", "wfbng", "gs_channel");   /* /gs/config */
    REQUIRE(std::string(chan) == "132"); free(chan);
    char *fps = pp_settings_get("air", "camera", "fps");         /* /air/config */
    REQUIRE(std::string(fps) == "60"); free(fps);
    m.stop();
}

TEST_CASE("integration: air write hits /air/config + /air/apply only", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);

    DoneWaiter w;
    pp_settings_set_async("air", "camera", "fps", "90", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);

    auto writes = m.writes_only();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0] == "PATCH /air/config");
    REQUIRE(writes[1] == "POST /air/apply");
    REQUIRE(m.last_air_patch_body.find("\"fps\":90") != std::string::npos);
    m.stop();
}

TEST_CASE("integration: shared channel change is drone-first then GS", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    install_provider_pointing_at(srv.port);

    DoneWaiter w;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "100", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);

    auto writes = srv.writes_only();
    REQUIRE(writes.size() == 4);
    REQUIRE(writes[0] == "PATCH /air/config");
    REQUIRE(writes[1] == "POST /air/apply");
    REQUIRE(writes[2] == "PATCH /gs/config");
    REQUIRE(writes[3] == "POST /gs/apply");
    REQUIRE(srv.last_air_patch_body == R"({"link":{"channel":100}})");
    REQUIRE(srv.last_gs_patch_body  == R"({"link":{"channel":100}})");
    srv.stop();
}

TEST_CASE("integration: shared change degrades to GS-only when drone down", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();
    install_provider_pointing_at(srv.port);
    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);

    DoneWaiter w;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "108", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);

    auto writes = srv.writes_only();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0] == "PATCH /gs/config");
    REQUIRE(writes[1] == "POST /gs/apply");
    srv.stop();
}

TEST_CASE("integration: GS apply failure is retried then succeeds", "[fpvd][network]") {
    GsMockServer srv; srv.gs_apply_fail_n = 1; srv.start();
    install_provider_pointing_at(srv.port);

    DoneWaiter w;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "112", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);

    /* Two POST /gs/apply entries: the failed first attempt and the retry. */
    int applies = 0;
    for (auto &l : srv.snapshot_log()) if (l == "POST /gs/apply") applies++;
    REQUIRE(applies == 2);
    srv.stop();
}

TEST_CASE("integration: GS apply 400 fails immediately without retries", "[fpvd][network]") {
    GsMockServer srv; srv.gs_apply_400_n = 1; srv.start();
    install_provider_pointing_at(srv.port);

    DoneWaiter w;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "116", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);

    /* Deterministic 4xx must not be retried: exactly one POST /gs/apply. */
    int applies = 0;
    for (auto &l : srv.snapshot_log()) if (l == "POST /gs/apply") applies++;
    REQUIRE(applies == 1);
    srv.stop();
}

TEST_CASE("integration: GS-side 502 does not clear drone reachability", "[fpvd][network]") {
    /* 502 from /gs/apply persists through all retries (1 + 3) so the step
     * fails — a gs_side 502 is not a drone-proxy signal. */
    GsMockServer srv; srv.gs_apply_502_n = 4; srv.start();
    install_provider_pointing_at(srv.port);
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);

    DoneWaiter w;
    pp_settings_set_async("gs", "wfbng", "gs_channel", "120", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);

    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);
    srv.stop();
}

TEST_CASE("integration: beamforming enable sends MAC handshake", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    install_provider_pointing_at(srv.port);   /* localMac primed from /gs/status */

    DoneWaiter w;
    pp_settings_set_async("gs", "link", "beamforming", "on", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);

    REQUIRE(srv.last_air_patch_body ==
        R"({"link":{"beamforming":{"enabled":true,"remoteMac":"84:fc:14:6c:36:e6"},"stbc":false}})");
    REQUIRE(srv.last_gs_patch_body ==
        R"({"link":{"beamforming":{"enabled":true}}})");
    auto writes = srv.writes_only();
    REQUIRE(writes.size() == 4);
    REQUIRE(writes[0] == "PATCH /air/config");
    REQUIRE(writes[1] == "POST /air/apply");
    REQUIRE(writes[2] == "PATCH /gs/config");
    REQUIRE(writes[3] == "POST /gs/apply");
    srv.stop();
}

TEST_CASE("integration: beamforming rejected while drone down", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();
    install_provider_pointing_at(srv.port);
    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);

    DoneWaiter w;
    pp_settings_set_async("gs", "link", "beamforming", "on", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);
    REQUIRE(w.err_str() == "Drone unreachable");
    for (auto &l : srv.snapshot_log())
        REQUIRE(l.rfind("PATCH", 0) != 0);    /* nothing was written anywhere */
    srv.stop();
}

TEST_CASE("integration: dynamicLink enable is drone-first across both daemons", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    install_provider_pointing_at(srv.port);

    DoneWaiter w;
    pp_settings_set_async("air", "dlink", "enabled", "on", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);

    REQUIRE(srv.last_air_patch_body == R"({"dynamicLink":{"enabled":true}})");
    REQUIRE(srv.last_gs_patch_body  == R"({"dynamicLink":{"enabled":true}})");
    auto writes = srv.writes_only();
    REQUIRE(writes.size() == 4);
    REQUIRE(writes[0] == "PATCH /air/config");
    REQUIRE(writes[1] == "POST /air/apply");
    REQUIRE(writes[2] == "PATCH /gs/config");
    REQUIRE(writes[3] == "POST /gs/apply");
    srv.stop();
}

TEST_CASE("integration: dynamicLink toggle rejected while drone down", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();
    install_provider_pointing_at(srv.port);
    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);

    DoneWaiter w;
    pp_settings_set_async("air", "dlink", "enabled", "on", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);
    REQUIRE(w.err_str() == "Drone unreachable");
    for (auto &l : srv.snapshot_log())
        REQUIRE(l.rfind("PATCH", 0) != 0);    /* nothing was written anywhere */
    srv.stop();
}

TEST_CASE("integration: reachability follows conn_state link, not /air", "[fpvd][network]") {
    GsMockServer srv; srv.drone_up = false; srv.start();  /* /air would report DOWN */
    install_provider_pointing_at(srv.port);               /* ingests CONNECTED */

    /* conn_state says connected even though the /air probe is failing. */
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == true);

    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);  /* link drops */
    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == false);
    /* GS-local rows stay reachable regardless of the drone. */
    REQUIRE(pp_settings_is_reachable("gs", "link", "rx_power") == true);
    srv.stop();
}

TEST_CASE("integration: staged screen_mode self-applies (patch + apply)", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    install_provider_pointing_at(srv.port);

    /* screen_mode is the only staged row left, and there is no manual Apply
     * button: a single set must produce both a /gs/config PATCH and a
     * POST /gs/apply, then clear pending. */
    DoneWaiter w;
    pp_settings_set_async("gs", "display", "screen_mode", "1280x720@60", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc == 0);
    REQUIRE(srv.last_gs_patch_body.find("\"screenMode\":\"1280x720@60\"") != std::string::npos);
    int applies = 0;
    for (auto &l : srv.snapshot_log()) if (l == "POST /gs/apply") applies++;
    REQUIRE(applies == 1);
    REQUIRE(pp_settings_has_pending() == false);
    srv.stop();
}

TEST_CASE("integration: rx_power reads dBm, null falls back to status radio", "[fpvd][network]") {
    GsMockServer srv;
    srv.gs_config_response =
        R"({"link":{"channel":132,"width":40,"txPowerDbm":null,"region":"US",)"
        R"("linkId":7669206,"beamforming":{"enabled":false},"wlans":"auto"},)"
        R"("pixelpilot":{}})";
    srv.start();
    install_provider_pointing_at(srv.port);   /* primes snapshots synchronously */

    char *v = pp_settings_get("gs", "link", "rx_power");
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == "19");   /* radio[0].txpowerDbm 19.0 rounded */
    free(v);
    srv.stop();
}

TEST_CASE("integration: dynamic_link_locked rejected client-side (air snapshot)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"txPowerDbm":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "mcs_index") == true);

    DoneWaiter w;
    pp_settings_set_async("air", "wfbng", "mcs_index", "5", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);
    for (auto &l : m.snapshot_log())
        REQUIRE(l != "PATCH /air/config");   /* never sent */
    m.stop();
}

TEST_CASE("integration: PATCH validation error short-circuits apply", "[fpvd][network]") {
    GsMockServer m;
    m.air_patch_error =
      R"({"error":"validation","message":"schema validation failed",)"
      R"("details":[{"path":"link.mcs","message":"must be 0..7"}]})";
    m.start();
    install_provider_pointing_at(m.port);

    DoneWaiter w;
    pp_settings_set_async("air", "wfbng", "mcs_index", "9", DoneWaiter::cb, &w);
    REQUIRE(w.wait());
    REQUIRE(w.rc != 0);
    REQUIRE(w.err_str() == "must be 0..7");
    for (auto &l : m.snapshot_log())
        REQUIRE(l != "POST /air/apply");
    m.stop();
}

TEST_CASE("integration: offline -> reconnect tracks GS reachability", "[fpvd][network]") {
    setenv("PP_FPVD_URL", "http://127.0.0.1:1", 1);
    ensure_lv_init();
    pp_settings_register_fpvd();
    REQUIRE(pp_settings_is_connected() == false);

    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    for (int i = 0; i < 300 && pp_settings_is_connected() == false; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(pp_settings_is_connected() == true);
    m.stop();
}

TEST_CASE("integration: drone reachability gates air rows, not GS rows", "[fpvd][network]") {
    GsMockServer m; m.drone_up = false; m.start();
    install_provider_pointing_at(m.port);
    conn_state_ingest(CONN_DISCONNECTED, "timeout", -1);

    REQUIRE(pp_settings_is_reachable("air", "camera", "fps") == false);
    REQUIRE(pp_settings_is_reachable("gs", "wfbng", "txpower") == false);  /* drone TX power */
    REQUIRE(pp_settings_is_reachable("gs", "link", "beamforming") == false);
    /* GS rows stay reachable — including shared channel/width (recovery path). */
    REQUIRE(pp_settings_is_reachable("gs", "wfbng", "gs_channel") == true);
    REQUIRE(pp_settings_is_reachable("gs", "link", "rx_power") == true);
    REQUIRE(pp_settings_is_reachable("gs", "dvr", "dvr_reenc_bitrate") == true);
    m.stop();
}

TEST_CASE("integration: Bandwidth row DL-locked while rx_power stays editable",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":40},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    // Bandwidth (gs/wfbng/bandwidth -> link.width) is pushed to the drone and
    // rejected by its DL lock, so it must report locked while DL is on.
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "bandwidth") == true);
    // rx_power (gs/link/rx_power -> link.txPowerDbm, EP_GS) is the GS card's
    // OWN power, not drone-controlled — it must stay editable.
    REQUIRE(pp_settings_is_locked("gs", "link", "rx_power") == false);
    m.stop();
}

TEST_CASE("integration: Bandwidth row editable when Dynamic Link is off",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":40},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "bandwidth") == false);
    m.stop();
}

TEST_CASE("integration: TX Power DL-locked while GS card power stays editable",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":40,"txPowerDbm":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    // Drone TX Power (gs/wfbng/txpower -> link.txPowerDbm, EP_AIR) is pushed to
    // the drone and rejected by its DL lock, so it reports locked while DL is on.
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "txpower") == true);
    // GS card power (gs/link/rx_power -> link.txPowerDbm, EP_GS) is the GS
    // card's OWN power, not drone-controlled — it must stay editable.
    REQUIRE(pp_settings_is_locked("gs", "link", "rx_power") == false);
    m.stop();
}

TEST_CASE("integration: TX Power editable when Dynamic Link is off",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":40,"txPowerDbm":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "txpower") == false);
    m.stop();
}

TEST_CASE("integration: hidden->visible triggers an immediate refresh", "[fpvd][network]") {
    GsMockServer srv; srv.start();
    install_provider_pointing_at(srv.port);
    char *c0 = pp_settings_get("gs", "wfbng", "gs_channel");
    REQUIRE(std::string(c0) == "132"); free(c0);

    /* GS config changes server-side; only a refresh will surface it. */
    srv.gs_config_response =
        R"({"link":{"channel":120,"width":40,"txPowerDbm":25,"region":"US",)"
        R"("linkId":7669206,"beamforming":{"enabled":false},"wlans":"auto"},)"
        R"("pixelpilot":{"videoScale":1.0,"screenMode":"1920x1080@60",)"
        R"("dvr":{"osd":true,"mode":"reencode","reencBitrate":50000}}})";
    pp_settings_set_visibility(false);
    pp_settings_set_visibility(true);     /* menu opens -> immediate refresh */

    bool flipped = false;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!flipped && std::chrono::steady_clock::now() < end) {
        char *c = pp_settings_get("gs", "wfbng", "gs_channel");
        flipped = (std::string(c) == "120"); free(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(flipped == true);
    pp_settings_set_visibility(false);
    srv.stop();
}

TEST_CASE("integration: DL on + swfec — mode/deadline/overhead editable, "
          "k/n + compute knobs locked", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"swfec"}},"dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_overhead_pct") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_n") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == true);
    /* Min/Max bitrate + Max MCS stay editable in both modes. */
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_min_bitrate_kbps") == false);
    m.stop();
}

TEST_CASE("integration: DL on + rs — compute knobs editable, k/n locked, "
          "deadline/overhead locked (hidden)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"rs"}},"dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_n") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_overhead_pct") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == false);
    m.stop();
}

TEST_CASE("integration: DL off — FEC + compute rows all editable", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"fec":{"mode":"swfec"}},"dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
    m.stop();
}
