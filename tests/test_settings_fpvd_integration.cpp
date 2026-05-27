#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <string>
#include <cstdio>

extern "C" {
#include "gsmenu/settings.h"
#include "gsmenu/settings_fpvd_internal.h"
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
