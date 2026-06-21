#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "gsmenu/settings_fpvd_internal.h"
#include "gsmenu/settings.h"
#include "gsmenu/settings_runtime_cfg.h"
}

TEST_CASE("keymap: lookup returns the json path for known triples", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "video.fps") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("air", "camera", "bitrate");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "video.bitrate") == 0);
    /* Raw kbps int: the Mbps slider (camera.c) does the kbps<->Mbps conversion,
     * so the keymap serializes the plain integer rather than an M-suffix string. */
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("gs", "wfbng", "bandwidth");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.width") == 0);

    e = fpvd_keymap_lookup("air", "wfbng", "fec_k");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.k") == 0);

    e = fpvd_keymap_lookup("air", "dlink", "safe_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.safe.bitrateKbps") == 0);
}

TEST_CASE("keymap: lookup returns null for unknown triples", "[fpvd][keymap]") {
    REQUIRE(fpvd_keymap_lookup("air", "camera", "nope") == nullptr);
    REQUIRE(fpvd_keymap_lookup("nope", "nope", "nope") == nullptr);
}

TEST_CASE("keymap: camera resilience/osd + dlink compute/maxMcs", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "resilience");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "video.resilience") == 0);
    REQUIRE(e->type == FPVD_T_ENUM);

    e = fpvd_keymap_lookup("air", "camera", "osd_enabled");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "osd.enabled") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);

    e = fpvd_keymap_lookup("air", "dlink", "compute_base_redundancy");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.baseRedundancyRatio") == 0);
    REQUIRE(e->type == FPVD_T_FLOAT);

    e = fpvd_keymap_lookup("air", "dlink", "compute_blocks_per_frame");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.blocksPerFrame") == 0);
    REQUIRE(e->type == FPVD_T_FLOAT);

    e = fpvd_keymap_lookup("air", "dlink", "compute_min_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.minBitrateKbps") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("air", "dlink", "compute_max_bitrate_kbps");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.compute.maxBitrateKbps") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("gs", "dlink", "max_mcs");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.maxMcs") == 0);
    REQUIRE(e->type == FPVD_T_INT);
    REQUIRE(e->endpoint == FPVD_EP_GS);
}

TEST_CASE("keymap: removed dynamic-link rows no longer resolve", "[fpvd][keymap]") {
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "interleaving") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "mavlink_enable") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "osd_enabled") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "osd_debug_latency") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "health_timeout_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "min_idr_interval_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "apply_stagger_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "apply_subpace_ms") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_threshold_kbps") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_low_anchor_kbps") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_floor") == nullptr);
    REQUIRE(fpvd_keymap_lookup("air", "dlink", "roiqp_step") == nullptr);
}

TEST_CASE("keymap: FEC mode/deadline/overhead map to link.fec.*", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "wfbng", "fec_mode");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.mode") == 0);
    REQUIRE(e->type == FPVD_T_ENUM);

    e = fpvd_keymap_lookup("air", "wfbng", "fec_deadline_ms");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.deadlineMs") == 0);
    REQUIRE(e->type == FPVD_T_INT);

    e = fpvd_keymap_lookup("air", "wfbng", "fec_overhead_pct");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.fec.overheadPct") == 0);
    REQUIRE(e->type == FPVD_T_INT);
}

TEST_CASE("keymap: NULL args are safe (no deref) — pp_row_text reads with NULL domain/page",
          "[fpvd][keymap]") {
    /* pp_row_text() calls pp_settings_get(NULL, NULL, key) for rows whose
     * domain/page aren't wired yet. The provider must tolerate NULL like the
     * dummy/stub do, not strcmp(NULL). */
    REQUIRE(fpvd_keymap_lookup(nullptr, "wfbng", "gs_channel") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", nullptr, "gs_channel") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "wfbng", nullptr) == nullptr);
    REQUIRE(fpvd_keymap_lookup(nullptr, nullptr, nullptr) == nullptr);
}

extern "C" {
#include "cJSON.h"
}
#include <cstdlib>

static cJSON *fixture_defaults() {
    /* Subset of /defaults from the API doc, enough for our path tests. */
    const char *src =
      "{\"link\":{\"channel\":161,\"width\":20,\"txPowerDbm\":20,\"mcs\":2,"
      "\"fec\":{\"k\":8,\"n\":12},\"stbc\":false,\"ldpc\":true},"
      "\"video\":{\"codec\":\"h265\",\"resolution\":\"1920x1080\","
      "\"fps\":60,\"bitrate\":8192,\"rcMode\":\"cbr\",\"gopSize\":1.0,"
      "\"qpDelta\":-4,\"roi\":{\"enabled\":true,\"qp\":0,\"center\":0.4,"
      "\"steps\":2}},"
      "\"image\":{\"mirror\":false,\"flip\":false,\"rotate\":0},"
      "\"recording\":{\"enabled\":false,\"maxSeconds\":300,\"maxMB\":500},"
      "\"dynamicLink\":{\"enabled\":false,\"safe\":{\"mcs\":1,"
      "\"bitrateKbps\":2000}}}";
    return cJSON_Parse(src);
}

TEST_CASE("snapshot: read int and string and bool", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    REQUIRE(root != nullptr);
    char *v;
    v = fpvd_snapshot_read_string(root, "video.fps", FPVD_T_INT);
    REQUIRE(std::strcmp(v, "60") == 0); free(v);
    v = fpvd_snapshot_read_string(root, "video.codec", FPVD_T_ENUM);
    REQUIRE(std::strcmp(v, "h265") == 0); free(v);
    v = fpvd_snapshot_read_string(root, "link.stbc", FPVD_T_BOOL);
    REQUIRE(std::strcmp(v, "off") == 0); free(v);
    v = fpvd_snapshot_read_string(root, "link.ldpc", FPVD_T_BOOL);
    REQUIRE(std::strcmp(v, "on") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: nested path", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "link.fec.k", FPVD_T_INT);
    REQUIRE(std::strcmp(v, "8") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: bitrate kbps -> M suffix", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.bitrate", FPVD_T_BITRATE_KBPS);
    REQUIRE(std::strcmp(v, "8M") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: missing path returns empty", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.absent", FPVD_T_INT);
    REQUIRE(v != nullptr);
    REQUIRE(std::strcmp(v, "") == 0); free(v);
    cJSON_Delete(root);
}

TEST_CASE("snapshot: float", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.gopSize", FPVD_T_FLOAT);
    REQUIRE(std::strcmp(v, "1") == 0); free(v);   /* int seconds, no fractional */
    cJSON_Delete(root);
}

TEST_CASE("snapshot: percent_to_frac", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "video.roi.center", FPVD_T_PERCENT_TO_FRAC);
    REQUIRE(std::strcmp(v, "40") == 0); free(v);   /* 0.4 → 40 */
    cJSON_Delete(root);
}

TEST_CASE("snapshot: seconds_from_min divides", "[fpvd][snapshot]") {
    cJSON *root = fixture_defaults();
    char *v = fpvd_snapshot_read_string(root, "recording.maxSeconds", FPVD_T_SECONDS_FROM_MIN);
    REQUIRE(std::strcmp(v, "5") == 0); free(v);    /* 300 / 60 = 5 */
    cJSON_Delete(root);
}

TEST_CASE("patch: build sparse body — flat int", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.fps", "90", FPVD_T_INT);
    REQUIRE(body != nullptr);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"video":{"fps":90}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: build sparse body — nested int", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("link.fec.k", "7", FPVD_T_INT);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"link":{"fec":{"k":7}}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: build sparse body — bool", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("link.stbc", "on", FPVD_T_BOOL);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"link":{"stbc":true}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: bitrate M-suffix parsed to kbps int", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.bitrate", "15M", FPVD_T_BITRATE_KBPS);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"video":{"bitrate":15000}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: seconds_from_min multiplies", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("recording.maxSeconds", "5",
                                        FPVD_T_SECONDS_FROM_MIN);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"recording":{"maxSeconds":300}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: percent_to_frac divides by 100", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.roi.center", "40",
                                        FPVD_T_PERCENT_TO_FRAC);
    /* cJSON formats doubles; accept either "0.4" or "0.40" (or scientific). */
    char *s = cJSON_PrintUnformatted(body);
    std::string out(s);
    /* Confirm it's a numeric value ~= 0.4 by checking key presence and
     * that it parses back. */
    REQUIRE(out.find("\"center\":") != std::string::npos);
    cJSON *parsed = cJSON_Parse(s);
    REQUIRE(parsed != nullptr);
    cJSON *video = cJSON_GetObjectItemCaseSensitive(parsed, "video");
    cJSON *roi = cJSON_GetObjectItemCaseSensitive(video, "roi");
    cJSON *center = cJSON_GetObjectItemCaseSensitive(roi, "center");
    REQUIRE(cJSON_IsNumber(center));
    REQUIRE(center->valuedouble >= 0.39);
    REQUIRE(center->valuedouble <= 0.41);
    cJSON_Delete(parsed);
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("patch: enum stored as string", "[fpvd][patch]") {
    cJSON *body = fpvd_build_patch_body("video.codec", "h264", FPVD_T_ENUM);
    char *s = cJSON_PrintUnformatted(body);
    REQUIRE(std::string(s) == R"({"video":{"codec":"h264"}})");
    free(s);
    cJSON_Delete(body);
}

TEST_CASE("lock: matches exact locked paths", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.mcs") == true);
    REQUIRE(fpvd_is_locked_path("link.txPowerDbm") == true);
    REQUIRE(fpvd_is_locked_path("link.width") == true);
    REQUIRE(fpvd_is_locked_path("video.bitrate") == true);
    REQUIRE(fpvd_is_locked_path("video.qpDelta") == true);
    /* old key no longer exists in the schema */
    REQUIRE(fpvd_is_locked_path("link.txpower") == false);
}

TEST_CASE("lock: matches subtrees", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.fec.k") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.n") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.mode") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.deadlineMs") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.overheadPct") == true);
    REQUIRE(fpvd_is_locked_path("video.roi.enabled") == true);
    REQUIRE(fpvd_is_locked_path("video.roi.center") == true);
}

TEST_CASE("lock: does not match unrelated", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.channel") == false);
    REQUIRE(fpvd_is_locked_path("video.fps") == false);
    REQUIRE(fpvd_is_locked_path("video.codec") == false);
    REQUIRE(fpvd_is_locked_path("image.mirror") == false);
    REQUIRE(fpvd_is_locked_path("video.resilience") == false);
    REQUIRE(fpvd_is_locked_path("osd.enabled") == false);
    REQUIRE(fpvd_is_locked_path("dynamicLink.compute") == false);
    REQUIRE(fpvd_is_locked_path("dynamicLink.maxMcs") == false);
}

TEST_CASE("lock: prefix overshoot is not a match", "[fpvd][lock]") {
    /* "link.widthful" is not a child of "link.width". */
    REQUIRE(fpvd_is_locked_path("link.widthful") == false);
}

TEST_CASE("http: GET against impossible host returns transport failure",
          "[fpvd][http]") {
    fpvd_http_result_t r = fpvd_http_get("http://127.0.0.1:1/nope");
    REQUIRE(r.status == 0);
    fpvd_http_result_free(&r);
}

TEST_CASE("endpoint: keymap entries carry endpoint + row kind", "[fpvd][endpoint]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(e->endpoint == FPVD_EP_AIR);
    REQUIRE(e->kind == FPVD_ROW_PLAIN);

    /* Shared link rows: GS endpoint, drone-first orchestration. */
    e = fpvd_keymap_lookup("gs", "wfbng", "gs_channel");
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_SHARED);
    REQUIRE(std::strcmp(e->path, "link.channel") == 0);

    e = fpvd_keymap_lookup("gs", "wfbng", "bandwidth");
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_SHARED);
    REQUIRE(std::strcmp(e->path, "link.width") == 0);

    /* GS card power: plain dBm int on the GS side. */
    e = fpvd_keymap_lookup("gs", "link", "rx_power");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txPowerDbm") == 0);
    REQUIRE(e->type == FPVD_T_INT);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_PLAIN);

    /* Drone TX power: renamed path, dBm. */
    e = fpvd_keymap_lookup("gs", "wfbng", "txpower");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txPowerDbm") == 0);
    REQUIRE(e->type == FPVD_T_INT);
    REQUIRE(e->endpoint == FPVD_EP_AIR);
    REQUIRE(e->kind == FPVD_ROW_PLAIN);

    /* Beamforming toggle: client-owned handshake. */
    e = fpvd_keymap_lookup("gs", "link", "beamforming");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.beamforming.enabled") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_BEAMFORM);
}

TEST_CASE("endpoint: routing helpers map AIR and GS trees", "[fpvd][endpoint]") {
    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_AIR), "/air/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_AIR), "/air/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(FPVD_EP_AIR),  "/air/config") == 0);

    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_GS), "/gs/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_GS), "/gs/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(FPVD_EP_GS),  "/gs/config?pending=true") == 0);
}

TEST_CASE("keymap: pixelpilot rows route to EP_GS as staged rows", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e;
    // screen_mode is the only remaining staged pixelpilot row.
    e = fpvd_keymap_lookup("gs", "display", "screen_mode");
    REQUIRE(e != nullptr);
    REQUIRE(e->endpoint == FPVD_EP_GS);
    REQUIRE(e->kind == FPVD_ROW_STAGED);
    REQUIRE(std::strcmp(e->path, "pixelpilot.screenMode") == 0);
    REQUIRE(e->type == FPVD_T_STRING);

    // video_scale now routes to runtime-cfg; rtp_jitter_ms was removed.
    REQUIRE(fpvd_keymap_lookup("gs", "display", "video_scale") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "display", "rtp_jitter_ms") == nullptr);

    // dvr rows are now owned by runtime-cfg, not the fpvd keymap
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_reenc_bitrate") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_mode") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_max_size") == nullptr);

    // color correction stays unmapped (handled by the unavailable rule)
    REQUIRE(fpvd_keymap_lookup("gs", "display", "color_correction") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "rec_enabled") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "rec_fps") == nullptr);
}

TEST_CASE("runtime-config keys are no longer fpvd-staged but are available") {
    // gs/dvr/* dropped from the fpvd keymap (now owned by runtime-cfg)
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_mode") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_max_size") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "dvr", "dvr_reenc_bitrate") == nullptr);
    // color-correction keys were never in the keymap
    REQUIRE(fpvd_keymap_lookup("gs", "display", "color_correction") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "display", "cc_gain") == nullptr);
    REQUIRE(fpvd_keymap_lookup("gs", "display", "cc_offset") == nullptr);
    // ...but the provider reports all six as available (so rows are not greyed)
    pp_settings_register_fpvd();
    REQUIRE(pp_settings_is_available("gs", "dvr", "dvr_mode"));
    REQUIRE(pp_settings_is_available("gs", "display", "cc_gain"));
    // a still-staged display row remains available too
    REQUIRE(pp_settings_is_available("gs", "display", "screen_mode"));
}

static int plan(fpvd_row_kind_t kind, fpvd_endpoint_t ep, const char *path,
                fpvd_type_t type, const char *value, bool reachable,
                const char *mac, fpvd_step_t *steps, char *err) {
    return fpvd_plan_steps(kind, ep, path, type, value, reachable, mac,
                           steps, FPVD_PLAN_MAX, err, 160);
}

TEST_CASE("plan: plain AIR row is patch+apply on /air", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_PLAIN, FPVD_EP_AIR, "video.fps", FPVD_T_INT, "90",
                 true, nullptr, s, err);
    REQUIRE(n == 2);
    REQUIRE(std::strcmp(s[0].method, "PATCH") == 0);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"video":{"fps":90}})");
    REQUIRE(s[0].gs_side == false);
    REQUIRE(std::strcmp(s[1].method, "POST") == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(s[1].body[0] == '\0');
}

TEST_CASE("plan: plain AIR row rejected when drone unreachable", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_PLAIN, FPVD_EP_AIR, "video.fps", FPVD_T_INT, "90",
                 false, nullptr, s, err);
    REQUIRE(n == -1);
    REQUIRE(std::string(err) == "Drone unreachable");
}

TEST_CASE("plan: plain GS row is patch+apply on /gs", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_PLAIN, FPVD_EP_GS, "link.txPowerDbm", FPVD_T_INT, "25",
                 false /* GS rows work regardless */, nullptr, s, err);
    REQUIRE(n == 2);
    REQUIRE(std::strcmp(s[0].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"link":{"txPowerDbm":25}})");
    REQUIRE(s[0].gs_side == true);
    REQUIRE(std::strcmp(s[1].url_path, "/gs/apply") == 0);
    REQUIRE(s[1].gs_side == true);
}

TEST_CASE("plan: staged row is a single GS patch", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_STAGED, FPVD_EP_GS, "pixelpilot.dvr.osd", FPVD_T_BOOL,
                 "on", true, nullptr, s, err);
    REQUIRE(n == 1);
    REQUIRE(std::strcmp(s[0].method, "PATCH") == 0);
    REQUIRE(std::strcmp(s[0].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"pixelpilot":{"dvr":{"osd":true}}})");
}

TEST_CASE("plan: shared row online is drone-first, GS retried", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_SHARED, FPVD_EP_GS, "link.channel", FPVD_T_INT, "100",
                 true, nullptr, s, err);
    REQUIRE(n == 4);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"link":{"channel":100}})");
    REQUIRE(s[0].retries == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(std::strcmp(s[2].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[2].body) == R"({"link":{"channel":100}})");
    REQUIRE(s[2].retries == 3);
    REQUIRE(s[2].gs_side == true);
    REQUIRE(std::strcmp(s[3].url_path, "/gs/apply") == 0);
    REQUIRE(s[3].retries == 3);
}

TEST_CASE("plan: shared row offline degrades to GS-only", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_SHARED, FPVD_EP_GS, "link.channel", FPVD_T_INT, "100",
                 false, nullptr, s, err);
    REQUIRE(n == 2);
    REQUIRE(std::strcmp(s[0].url_path, "/gs/config") == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/gs/apply") == 0);
}

TEST_CASE("plan: beamforming enable carries remoteMac and stbc=false", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "on", true, "84:fc:14:6c:36:e6", s, err);
    REQUIRE(n == 4);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) ==
        R"({"link":{"beamforming":{"enabled":true,"remoteMac":"84:fc:14:6c:36:e6"},"stbc":false}})");
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(std::strcmp(s[2].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[2].body) == R"({"link":{"beamforming":{"enabled":true}}})");
    REQUIRE(std::strcmp(s[3].url_path, "/gs/apply") == 0);
}

TEST_CASE("plan: beamforming disable restores stbc", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "off", true, "84:fc:14:6c:36:e6", s, err);
    REQUIRE(n == 4);
    REQUIRE(std::string(s[0].body) ==
        R"({"link":{"beamforming":{"enabled":false},"stbc":true}})");
    REQUIRE(std::string(s[2].body) == R"({"link":{"beamforming":{"enabled":false}}})");
}

TEST_CASE("plan: beamforming rejected offline or without MAC", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    REQUIRE(plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "on", false, "aa:bb:cc:dd:ee:ff", s, err) == -1);
    REQUIRE(std::string(err) == "Drone unreachable");
    err[0] = '\0';
    REQUIRE(plan(FPVD_ROW_BEAMFORM, FPVD_EP_GS, "link.beamforming.enabled",
                 FPVD_T_BOOL, "on", true, nullptr, s, err) == -1);
    REQUIRE(std::string(err) == "GS card MAC unknown");
}

TEST_CASE("plan: bad value yields error not steps", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    REQUIRE(plan(FPVD_ROW_PLAIN, FPVD_EP_AIR, "video.fps", FPVD_T_INT,
                 "notanint", true, nullptr, s, err) == -1);
    REQUIRE(err[0] != '\0');
}

TEST_CASE("plan: oversized value is rejected, not truncated", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    std::string big(300, 'x');
    int n = fpvd_plan_steps(FPVD_ROW_PLAIN, FPVD_EP_GS, "pixelpilot.screenMode",
                            FPVD_T_STRING, big.c_str(), true, nullptr,
                            s, FPVD_PLAN_MAX, err, sizeof err);
    REQUIRE(n == -1);
    REQUIRE(err[0] != '\0');
}

TEST_CASE("plan: dynamicLink enable is drone-first both sides, GS retried", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_DLINK, FPVD_EP_AIR, "dynamicLink.enabled", FPVD_T_BOOL,
                 "on", true, nullptr, s, err);
    REQUIRE(n == 4);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"dynamicLink":{"enabled":true}})");
    REQUIRE(s[0].retries == 0);
    REQUIRE(std::strcmp(s[1].url_path, "/air/apply") == 0);
    REQUIRE(std::strcmp(s[2].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[2].body) == R"({"dynamicLink":{"enabled":true}})");
    REQUIRE(s[2].retries == 3);
    REQUIRE(s[2].gs_side == true);
    REQUIRE(std::strcmp(s[3].url_path, "/gs/apply") == 0);
    REQUIRE(s[3].retries == 3);
}

TEST_CASE("plan: dynamicLink disable is drone-first both sides", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_DLINK, FPVD_EP_AIR, "dynamicLink.enabled", FPVD_T_BOOL,
                 "off", true, nullptr, s, err);
    REQUIRE(n == 4);
    REQUIRE(std::strcmp(s[0].url_path, "/air/config") == 0);
    REQUIRE(std::string(s[0].body) == R"({"dynamicLink":{"enabled":false}})");
    REQUIRE(std::strcmp(s[2].url_path, "/gs/config") == 0);
    REQUIRE(std::string(s[2].body) == R"({"dynamicLink":{"enabled":false}})");
}

TEST_CASE("plan: dynamicLink rejected when drone unreachable", "[fpvd][plan]") {
    fpvd_step_t s[FPVD_PLAN_MAX]; char err[160] = {0};
    int n = plan(FPVD_ROW_DLINK, FPVD_EP_AIR, "dynamicLink.enabled", FPVD_T_BOOL,
                 "on", false, nullptr, s, err);
    REQUIRE(n == -1);
    REQUIRE(std::string(err) == "Drone unreachable");
}

TEST_CASE("keymap: dynamicLink enabled is an orchestrated DLINK row", "[fpvd][keymap]") {
    const fpvd_keymap_entry_t *e = fpvd_keymap_lookup("air", "dlink", "enabled");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "dynamicLink.enabled") == 0);
    REQUIRE(e->type == FPVD_T_BOOL);
    REQUIRE(e->endpoint == FPVD_EP_AIR);
    REQUIRE(e->kind == FPVD_ROW_DLINK);
    /* the other dlink rows stay plain drone-only writes */
    e = fpvd_keymap_lookup("air", "dlink", "safe_mcs");
    REQUIRE(e->kind == FPVD_ROW_PLAIN);
}
