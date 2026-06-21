#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>   // rand, free, remove, atoi
#include <cstring>   // strlen
#include <string>
extern "C" {
#include "gsmenu/settings_runtime_cfg.h"
}

static std::string write_tmp(const char *body) {
    std::string path = std::string("/tmp/pp_rtcfg_test_") + std::to_string(::rand()) + ".json";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(body, 1, strlen(body), f);
    fclose(f);
    return path;
}

TEST_CASE("defaults are the documented built-ins") {
    pp_runtime_cfg_t c;
    pp_runtime_cfg_defaults(&c);
    REQUIRE(c.dvr_mode == 0);
    REQUIRE(c.dvr_max_size_mb == 4000);
    REQUIRE(c.dvr_reenc_kbps == 8000);
    REQUIRE(c.cc_enabled == 0);
    REQUIRE(c.cc_gain == 25);
    REQUIRE(c.cc_offset == -15);
    REQUIRE(c.video_scale_pct == 100);
}

TEST_CASE("load missing file yields defaults and returns false") {
    pp_runtime_cfg_set_path("/tmp/pp_rtcfg_does_not_exist_12345.json");
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == false);
    REQUIRE(c.dvr_max_size_mb == 4000);
    REQUIRE(c.cc_gain == 25);
}

TEST_CASE("load reads all fields") {
    std::string p = write_tmp(
        "{\"dvr\":{\"mode\":\"both\",\"maxSizeMb\":8000,\"reencBitrateKbps\":12000},"
        "\"colorCorrection\":{\"enabled\":true,\"gain\":30,\"offset\":-20},"
        "\"display\":{\"videoScalePct\":75}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == true);
    REQUIRE(c.dvr_mode == 2);
    REQUIRE(c.dvr_max_size_mb == 8000);
    REQUIRE(c.dvr_reenc_kbps == 12000);
    REQUIRE(c.cc_enabled == 1);
    REQUIRE(c.cc_gain == 30);
    REQUIRE(c.cc_offset == -20);
    REQUIRE(c.video_scale_pct == 75);
    remove(p.c_str());
}

TEST_CASE("load tolerates partial json — absent fields keep defaults") {
    std::string p = write_tmp("{\"dvr\":{\"mode\":\"reencode\"}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == true);
    REQUIRE(c.dvr_mode == 1);
    REQUIRE(c.dvr_max_size_mb == 4000);   /* default */
    REQUIRE(c.cc_gain == 25);             /* default */
    remove(p.c_str());
}

TEST_CASE("load on malformed json returns false and fills defaults") {
    std::string p = write_tmp("{ this is not json ");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c;
    REQUIRE(pp_runtime_cfg_load(&c) == false);
    REQUIRE(c.dvr_reenc_kbps == 8000);
    remove(p.c_str());
}

TEST_CASE("owns is NULL-safe (pp_row_text passes NULL domain/page)") {
    /* Regression: prov_get/is_available/is_locked call pp_runtime_cfg_owns first,
     * and the menu invokes the provider with NULL domain/page for non-settings
     * text rows (pp_row_text). owns must answer false, not dereference.
     * This crashed the OSD thread on device (build_system_tab -> pp_row_text). */
    REQUIRE_FALSE(pp_runtime_cfg_owns(nullptr, nullptr, nullptr));
    REQUIRE_FALSE(pp_runtime_cfg_owns(nullptr, nullptr, "Version"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("gs", nullptr, "dvr_mode"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("gs", "dvr", nullptr));
}

TEST_CASE("owns matches exactly the six keys") {
    REQUIRE(pp_runtime_cfg_owns("gs", "dvr", "dvr_mode"));
    REQUIRE(pp_runtime_cfg_owns("gs", "dvr", "dvr_max_size"));
    REQUIRE(pp_runtime_cfg_owns("gs", "dvr", "dvr_reenc_bitrate"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "color_correction"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "cc_gain"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "cc_offset"));
    REQUIRE(pp_runtime_cfg_owns("gs", "display", "video_scale"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("gs", "display", "screen_mode"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("gs", "dvr", "rec_enabled"));
    REQUIRE_FALSE(pp_runtime_cfg_owns("air", "camera", "fps"));
}

TEST_CASE("get returns widget-format strings from the loaded file") {
    std::string p = write_tmp(
        "{\"dvr\":{\"mode\":\"reencode\",\"maxSizeMb\":8000,\"reencBitrateKbps\":12000},"
        "\"colorCorrection\":{\"enabled\":true,\"gain\":30,\"offset\":-20}}");
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c; pp_runtime_cfg_load(&c);

    auto chk = [](const char *d, const char *pg, const char *k, const char *want) {
        char *v = pp_runtime_cfg_get(d, pg, k);
        REQUIRE(v != nullptr);
        REQUIRE(std::string(v) == want);
        free(v);
    };
    chk("gs", "dvr", "dvr_mode", "reencode");
    chk("gs", "dvr", "dvr_max_size", "8000");
    chk("gs", "dvr", "dvr_reenc_bitrate", "12000");
    chk("gs", "display", "color_correction", "on");
    chk("gs", "display", "cc_gain", "30");
    chk("gs", "display", "cc_offset", "-20");
    chk("gs", "display", "video_scale", "100");   /* absent in file -> default */
    REQUIRE(pp_runtime_cfg_get("gs", "display", "screen_mode") == nullptr);
    remove(p.c_str());
}

/* ---- fake apply ops ---- */
struct FakeOps {
    int mode = -1, max_mb = -1, kbps = -1;
    int cc_enabled = -1; float cc_gain = -1, cc_offset = -1;
    int recording = 0;
    float vscale = -1;
};
static FakeOps g_fake;
static void f_mode(int m)      { g_fake.mode = m; }
static void f_max(int mb)      { g_fake.max_mb = mb; }
static void f_kbps(int k)      { g_fake.kbps = k; }
static void f_cc(int e, float g, float o) { g_fake.cc_enabled = e; g_fake.cc_gain = g; g_fake.cc_offset = o; }
static int  f_rec(void)        { return g_fake.recording; }
static void f_vscale(float fct){ g_fake.vscale = fct; }

static void install_fake_ops() {
    static pp_runtime_cfg_ops_t ops = { f_mode, f_max, f_kbps, f_cc, f_rec, f_vscale };
    pp_runtime_cfg_set_ops(&ops);
}

TEST_CASE("set applies via ops and persists to disk") {
    g_fake = FakeOps{};
    install_fake_ops();
    std::string p = std::string("/tmp/pp_rtcfg_set_") + std::to_string(::rand()) + ".json";
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t c; pp_runtime_cfg_load(&c);   /* primes from (missing) file -> defaults */

    pp_runtime_cfg_set("gs", "dvr", "dvr_mode", "both");
    REQUIRE(g_fake.mode == 2);

    pp_runtime_cfg_set("gs", "dvr", "dvr_max_size", "9000");
    REQUIRE(g_fake.max_mb == 9000);

    pp_runtime_cfg_set("gs", "dvr", "dvr_reenc_bitrate", "16000");
    REQUIRE(g_fake.kbps == 16000);

    pp_runtime_cfg_set("gs", "display", "cc_gain", "30");
    /* colortrans_apply gets all three cc fields; gain mapped /10 */
    REQUIRE(g_fake.cc_gain == 3.0f);
    REQUIRE(g_fake.cc_offset == -0.15f);   /* default -15 / 100 */
    REQUIRE(g_fake.cc_enabled == 0);

    pp_runtime_cfg_set("gs", "display", "color_correction", "on");
    REQUIRE(g_fake.cc_enabled == 1);

    pp_runtime_cfg_set("gs", "display", "video_scale", "75");
    REQUIRE(g_fake.vscale == 0.75f);   /* pct mapped /100 */

    /* Persisted: reload from a fresh path-reset and confirm round-trip. */
    pp_runtime_cfg_set_path(p.c_str());
    pp_runtime_cfg_t r; REQUIRE(pp_runtime_cfg_load(&r) == true);
    REQUIRE(r.dvr_mode == 2);
    REQUIRE(r.dvr_max_size_mb == 9000);
    REQUIRE(r.dvr_reenc_kbps == 16000);
    REQUIRE(r.cc_gain == 30);
    REQUIRE(r.cc_enabled == 1);
    REQUIRE(r.video_scale_pct == 75);
    remove(p.c_str());
    pp_runtime_cfg_set_ops(NULL);
}

TEST_CASE("set on a non-owned key is a no-op") {
    g_fake = FakeOps{};
    install_fake_ops();
    pp_runtime_cfg_set("gs", "display", "screen_mode", "1280x720@60");
    REQUIRE(g_fake.mode == -1);
    pp_runtime_cfg_set_ops(NULL);
}

TEST_CASE("is_recording reflects ops; false when unregistered") {
    pp_runtime_cfg_set_ops(NULL);
    REQUIRE(pp_runtime_cfg_is_recording() == false);
    g_fake = FakeOps{}; g_fake.recording = 1;
    install_fake_ops();
    REQUIRE(pp_runtime_cfg_is_recording() == true);
    pp_runtime_cfg_set_ops(NULL);
}
