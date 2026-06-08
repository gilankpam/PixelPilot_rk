#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

extern "C" {
#include "lvgl/lvgl.h"
#include "gsmenu/settings.h"
#include "gsmenu/settings_fpvd_internal.h"
}

namespace fs = std::filesystem;

static void ensure_lv_init() {
    static bool done = false;
    if (done) return;
    lv_init();
    done = true;
}

/* Mock GS fpvd: /link (+ /link/apply) and the /air/* proxy. */
struct GsMockServer {
    httplib::Server svr;
    std::thread     th;
    int             port = 0;

    std::atomic<int> link_get{0}, link_patch{0}, link_apply{0};
    std::atomic<int> air_get{0},  air_patch{0},  air_apply{0};
    std::atomic<int> config_get{0}, config_patch{0}, apply_post{0};
    std::string last_link_patch_body, last_air_patch_body, last_apply_to;
    std::string last_config_patch_body;
    std::string config_response =
        R"({"link":{"channel":161,"width":20},)"
        R"("pixelpilot":{"videoScale":1.0,"screenMode":"1920x1080@60",)"
        R"("dvr":{"osd":true,"mode":"reencode","reencBitrate":50000}}})";

    /* GET /link returns the link block flat (channel/width/txpower/...). */
    std::string link_response =
        R"({"channel":161,"width":20,"txpower":1950,"region":"US",)"
        R"("linkId":7669206,"droneReachable":true})";
    /* GET /air/config returns the drone config. */
    std::string air_response =
        R"({"link":{"channel":161,"width":20,"txpower":1,"mcs":2,)"
        R"("fec":{"k":8,"n":12},"stbc":false,"ldpc":false},)"
        R"("video":{"codec":"h265","resolution":"1920x1080","fps":60,)"
        R"("bitrate":8192,"rcMode":"cbr","gopSize":1.0,"qpDelta":-4,)"
        R"("roi":{"enabled":true,"qp":0,"center":0.4,"steps":2}},)"
        R"("image":{"mirror":false,"flip":false,"rotate":0},)"
        R"("recording":{"enabled":false,"maxSeconds":300,"maxMB":500},)"
        R"("dynamicLink":{"enabled":false,"safe":{"mcs":1,"bitrateKbps":2000}}})";
    std::string air_patch_error;   /* if set, PATCH /air/config -> 400 */

    void start() {
        svr.Get("/link", [this](const httplib::Request&, httplib::Response& res) {
            link_get++; res.set_content(link_response, "application/json");
        });
        svr.Patch("/link", [this](const httplib::Request& req, httplib::Response& res) {
            link_patch++; last_link_patch_body = req.body;
            res.set_content("{}", "application/json");
        });
        svr.Post("/link/apply", [this](const httplib::Request& req, httplib::Response& res) {
            link_apply++; last_apply_to = req.body;
            res.set_content(R"({"gsApplied":true})", "application/json");
        });
        svr.Get("/air/config", [this](const httplib::Request&, httplib::Response& res) {
            air_get++; res.set_content(air_response, "application/json");
        });
        svr.Patch("/air/config", [this](const httplib::Request& req, httplib::Response& res) {
            air_patch++; last_air_patch_body = req.body;
            if (!air_patch_error.empty()) { res.status = 400; res.set_content(air_patch_error, "application/json"); }
            else res.set_content(air_response, "application/json");
        });
        svr.Post("/air/apply", [this](const httplib::Request&, httplib::Response& res) {
            air_apply++; res.set_content(R"({"applied":true})", "application/json");
        });
        svr.Get("/config", [this](const httplib::Request&, httplib::Response& res) {
            config_get++; res.set_content(config_response, "application/json");
        });
        svr.Patch("/config", [this](const httplib::Request& req, httplib::Response& res) {
            config_patch++; last_config_patch_body = req.body;
            res.set_content("{}", "application/json");
        });
        svr.Post("/apply", [this](const httplib::Request&, httplib::Response& res) {
            apply_post++; res.set_content(R"({"applied":true})", "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    void stop() { svr.stop(); if (th.joinable()) th.join(); }
};

static void install_provider_pointing_at(int port) {
    ensure_lv_init();
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("PP_FPVD_URL", url, 1);
    pp_settings_register_fpvd();
}

static void wait_first_poll(GsMockServer& m) {
    for (int i = 0; i < 100 && m.link_get == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

TEST_CASE("integration: reads route to the right endpoint", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(m.link_get >= 1);
    REQUIRE(m.air_get  >= 1);
    REQUIRE(pp_settings_is_connected() == true);

    char *chan = pp_settings_get("gs", "wfbng", "gs_channel");   /* from /link */
    REQUIRE(std::string(chan) == "161"); free(chan);
    char *fps = pp_settings_get("air", "camera", "fps");         /* from /air/config */
    REQUIRE(std::string(fps) == "60"); free(fps);
    m.stop();
}

TEST_CASE("integration: air write hits /air/config + /air/apply only", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("air", "camera", "fps", "90", nullptr, nullptr);
    for (int i = 0; i < 200 && m.air_apply == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.air_patch >= 1);
    REQUIRE(m.air_apply >= 1);
    REQUIRE(m.last_air_patch_body.find("\"fps\":90") != std::string::npos);
    REQUIRE(m.link_patch == 0);
    m.stop();
}

TEST_CASE("integration: shared link write hits /link + applyTo both", "[fpvd][network]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("gs", "wfbng", "gs_channel", "149", nullptr, nullptr);
    for (int i = 0; i < 200 && m.link_apply == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.link_patch >= 1);
    REQUIRE(m.last_link_patch_body.find("\"channel\":149") != std::string::npos);
    REQUIRE(m.last_apply_to.find("\"applyTo\":\"both\"") != std::string::npos);
    REQUIRE(m.air_patch == 0);
    m.stop();
}

TEST_CASE("integration: rx_power maps percent -> link.txpower, applyTo gs", "[fpvd][network]") {
    /* NIC fixture: one wlx interface with the rtl88x2eu driver. */
    fs::path root = fs::temp_directory_path() / ("ppnic_" + std::to_string(::getpid()));
    fs::create_directories(root / "wlx_test" / "device");
    std::ofstream(root / "wlx_test" / "device" / "uevent") << "DRIVER=rtl88x2eu\n";
    setenv("PP_GS_SYS_CLASS_NET", root.c_str(), 1);

    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("gs", "link", "rx_power", "50", nullptr, nullptr);
    for (int i = 0; i < 200 && m.link_apply == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.link_patch >= 1);
    REQUIRE(m.last_link_patch_body.find("\"txpower\":1950") != std::string::npos);  /* 50% rtl88x2eu */
    REQUIRE(m.last_apply_to.find("\"applyTo\":\"gs\"") != std::string::npos);

    unsetenv("PP_GS_SYS_CLASS_NET");
    fs::remove_all(root);
    m.stop();
}

TEST_CASE("integration: dynamic_link_locked rejected client-side (air snapshot)", "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"txpower":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "mcs_index") == true);

    int before = m.air_patch;
    pp_settings_set_async("air", "wfbng", "mcs_index", "5", nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(m.air_patch == before);   /* never sent */
    m.stop();
}

TEST_CASE("integration: PATCH validation error short-circuits apply", "[fpvd][network]") {
    GsMockServer m;
    m.air_patch_error =
      R"({"error":"validation","message":"schema validation failed",)"
      R"("details":[{"path":"link.mcs","message":"must be 0..7"}]})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    pp_settings_set_async("air", "wfbng", "mcs_index", "9", nullptr, nullptr);
    for (int i = 0; i < 200 && m.air_patch == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.air_patch >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(m.air_apply == 0);
    m.stop();
}

TEST_CASE("integration: pixelpilot row stages via /config, no apply; Apply posts /apply",
          "[fpvd][network][config]") {
    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);

    /* The mock registers Get("/config"); httplib matches on path and ignores
     * the "?pending=true" query, so the staged-snapshot read still resolves. */

    /* a DVR row stages: PATCH /config, NO /apply */
    pp_settings_set_async("gs", "dvr", "dvr_reenc_bitrate", "12000", nullptr, nullptr);
    for (int i = 0; i < 200 && m.config_patch == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.config_patch >= 1);
    REQUIRE(m.apply_post == 0);                          // staged, not applied
    REQUIRE(m.last_config_patch_body.find("\"reencBitrate\":12000") != std::string::npos);
    /* Staging sets config_dirty AFTER the PATCH returns, so poll the flag itself
     * (not just config_patch) to avoid racing the worker's dirty mark. */
    for (int i = 0; i < 200 && !pp_settings_has_pending(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(pp_settings_has_pending());

    /* explicit Apply: POST /apply, no further PATCH */
    int patch_before = m.config_patch.load();
    pp_settings_apply(nullptr, nullptr);
    for (int i = 0; i < 200 && m.apply_post == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(m.apply_post >= 1);
    REQUIRE(m.config_patch == patch_before);            // apply does not PATCH
    /* Apply clears config_dirty AFTER the /apply POST returns, so poll the flag
     * itself (not just apply_post) to avoid racing the worker's clear. */
    for (int i = 0; i < 200 && pp_settings_has_pending(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(!pp_settings_has_pending());
    m.stop();
}

TEST_CASE("integration: offline -> reconnect tracks /link reachability", "[fpvd][network]") {
    setenv("PP_FPVD_URL", "http://127.0.0.1:1", 1);
    pp_settings_register_fpvd();
    REQUIRE(pp_settings_is_connected() == false);

    GsMockServer m; m.start();
    install_provider_pointing_at(m.port);
    for (int i = 0; i < 100 && pp_settings_is_connected() == false; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(pp_settings_is_connected() == true);
    m.stop();
}

TEST_CASE("integration: Bandwidth row DL-locked while rx_power stays editable",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":20},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    // Bandwidth (gs/wfbng/bandwidth -> link.width) is pushed to the drone and
    // rejected by its DL lock, so it must report locked while DL is on.
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "bandwidth") == true);
    // rx_power (gs/link/rx_power -> link.txpower) is the GS card's OWN power
    // (apply_to "gs"), not drone-controlled — it must stay editable.
    REQUIRE(pp_settings_is_locked("gs", "link", "rx_power") == false);
    m.stop();
}

TEST_CASE("integration: Bandwidth row editable when Dynamic Link is off",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":20},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "bandwidth") == false);
    m.stop();
}

TEST_CASE("integration: TX Power DL-locked while GS card power stays editable",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":20,"txpower":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":true}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    // Drone TX Power (gs/wfbng/txpower -> link.txpower, EP_AIR) is pushed to the
    // drone and rejected by its DL lock, so it must report locked while DL is on.
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "txpower") == true);
    // GS card power (gs/link/rx_power -> link.txpower, EP_LINK) is the GS card's
    // OWN power (apply_to "gs"), not drone-controlled — it must stay editable.
    REQUIRE(pp_settings_is_locked("gs", "link", "rx_power") == false);
    m.stop();
}

TEST_CASE("integration: TX Power editable when Dynamic Link is off",
          "[fpvd][network]") {
    GsMockServer m;
    m.air_response =
      R"({"link":{"mcs":2,"width":20,"txpower":1},"video":{"bitrate":8192},)"
      R"("dynamicLink":{"enabled":false}})";
    m.start();
    install_provider_pointing_at(m.port);
    wait_first_poll(m);
    REQUIRE(pp_settings_is_locked("gs", "wfbng", "txpower") == false);
    m.stop();
}
