#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <string>
#include <cstdio>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/settings_fpvd_internal.h"
}

/* Minimal LVGL initialisation required by the fpvd provider (lv_async_call,
 * lv_malloc).  Called once per process via a static init guard. */
static void ensure_lv_init() {
    static bool done = false;
    if (done) return;
    lv_init();
    done = true;
}

/* The provider talks HTTP to PP_FPVD_URL. We spin up a cpp-httplib server
 * on an ephemeral port, set the env var, and register the provider. */
struct FpvdMockServer {
    httplib::Server svr;
    std::thread     th;
    int             port = 0;
    std::atomic<int> patch_calls{0};
    std::atomic<int> apply_calls{0};
    std::atomic<int> get_calls{0};
    std::string     last_patch_body;
    std::string     get_response =
        R"({"link":{"channel":161,"width":20,"txpower":1,"mcs":2,)"
        R"("fec":{"k":8,"n":12},"stbc":false,"ldpc":false},)"
        R"("video":{"codec":"h265","resolution":"1920x1080","fps":60,)"
        R"("bitrate":8192,"rcMode":"cbr","gopSize":1.0,"qpDelta":-4,)"
        R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
        R"("image":{"mirror":false,"flip":false,"rotate":0},)"
        R"("recording":{"enabled":false,"maxSeconds":300,"maxMB":500},)"
        R"("dynamicLink":{"enabled":false,"safe":{"mcs":1,"bitrateKbps":2000}}})";
    /* Optional override: if non-empty, PATCH returns 400 with this body. */
    std::string     patch_error_body;
    /* Optional override: if non-empty, POST /apply returns 400 with this body. */
    std::string     apply_error_body;

    void start() {
        svr.Get("/config", [this](const httplib::Request &, httplib::Response &res) {
            get_calls++;
            res.set_content(get_response, "application/json");
        });
        svr.Patch("/config", [this](const httplib::Request &req, httplib::Response &res) {
            patch_calls++;
            last_patch_body = req.body;
            if (!patch_error_body.empty()) {
                res.status = 400;
                res.set_content(patch_error_body, "application/json");
            } else {
                res.set_content(get_response, "application/json");
            }
        });
        svr.Post("/apply", [this](const httplib::Request &, httplib::Response &res) {
            apply_calls++;
            if (!apply_error_body.empty()) {
                res.status = 400;
                res.set_content(apply_error_body, "application/json");
            } else {
                res.set_content(R"({"applied":true,"version":1,"restarted":[]})",
                                "application/json");
            }
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void stop() {
        svr.stop();
        if (th.joinable()) th.join();
    }
};

/* Helper: build PP_FPVD_URL, set env, then register provider. */
static void install_provider_pointing_at(int port) {
    ensure_lv_init();
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("PP_FPVD_URL", url, 1);
    pp_settings_register_fpvd();
}

TEST_CASE("fixture: mock server starts and accepts requests",
          "[fpvd][network][fixture]") {
    FpvdMockServer m;
    m.start();
    REQUIRE(m.port > 0);
    /* No client call yet; just verify the server thread is alive and routes are bound. */
    httplib::Client c("127.0.0.1", m.port);
    auto r = c.Get("/config");
    REQUIRE(r != nullptr);
    REQUIRE(r->status == 200);
    REQUIRE(m.get_calls.load() == 1);
    m.stop();
}

TEST_CASE("integration: PATCH + apply happy path", "[fpvd][network]") {
    FpvdMockServer m; m.start();
    install_provider_pointing_at(m.port);

    /* Wait for initial GET /config to land. */
    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(m.get_calls >= 1);
    REQUIRE(pp_settings_is_connected() == true);

    /* Trigger an async set; the callback fires on the LVGL thread which
     * we don't have a loop for in tests. Instead we observe the server. */
    pp_settings_set_async("air", "camera", "fps", "90", nullptr, nullptr);
    for (int i = 0; i < 200 && m.apply_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    REQUIRE(m.patch_calls >= 1);
    REQUIRE(m.apply_calls >= 1);
    REQUIRE(m.last_patch_body.find("\"fps\":90") != std::string::npos);

    m.stop();
}

TEST_CASE("integration: PATCH validation error short-circuits apply",
          "[fpvd][network]") {
    FpvdMockServer m;
    m.patch_error_body =
      R"({"error":"validation","message":"schema validation failed",)"
      R"("details":[{"path":"link.mcs","message":"must be 0..7"}]})";
    m.start();
    install_provider_pointing_at(m.port);

    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    /* Fire a write; server returns 400 on PATCH. Worker should NOT proceed
     * to POST /apply. */
    pp_settings_set_async("air", "wfbng", "mcs_index", "9", nullptr, nullptr);
    for (int i = 0; i < 200 && m.patch_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.patch_calls >= 1);
    /* Apply should NOT have been called (PATCH 400 short-circuits). */
    /* Wait a touch longer to be confident the worker has finished the job. */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(m.apply_calls == 0);

    m.stop();
}

TEST_CASE("integration: dynamic_link_locked rejected client-side",
          "[fpvd][network]") {
    FpvdMockServer m;
    /* Snapshot has dynamicLink.enabled = true so client-side lock check fires
     * before any network call. */
    m.get_response =
      R"({"link":{"channel":161,"width":20,"txpower":1,"mcs":2,)"
      R"("fec":{"k":8,"n":12}},"video":{"bitrate":8192,"qpDelta":-4,)"
      R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);

    for (int i = 0; i < 50 && m.get_calls == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(pp_settings_is_connected() == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "mcs_index") == true);

    /* Attempting to set the locked field should be rejected before any
     * PATCH; the server sees no patch call. */
    int before = m.patch_calls;
    pp_settings_set_async("air", "wfbng", "mcs_index", "5", nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(m.patch_calls == before);

    m.stop();
}

TEST_CASE("integration: offline -> reconnect transitions connected flag",
          "[fpvd][network]") {
    /* No server running yet — pointing at a port that should refuse. */
    setenv("PP_FPVD_URL", "http://127.0.0.1:1", 1);
    pp_settings_register_fpvd();
    /* The synchronous prime fails → connected==false. */
    REQUIRE(pp_settings_is_connected() == false);

    /* Now start a server, repoint the env, re-register (provider re-reads
     * env in pp_settings_register_fpvd). */
    FpvdMockServer m; m.start();
    install_provider_pointing_at(m.port);
    for (int i = 0; i < 50 && pp_settings_is_connected() == false; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(pp_settings_is_connected() == true);
    m.stop();
}
