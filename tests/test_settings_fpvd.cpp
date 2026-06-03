#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "gsmenu/settings_fpvd_internal.h"
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
    REQUIRE(e->type == FPVD_T_BITRATE_KBPS);

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
      "{\"link\":{\"channel\":161,\"width\":20,\"txpower\":1,\"mcs\":2,"
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
    REQUIRE(fpvd_is_locked_path("link.txpower") == true);
    REQUIRE(fpvd_is_locked_path("link.width") == true);
    REQUIRE(fpvd_is_locked_path("video.bitrate") == true);
    REQUIRE(fpvd_is_locked_path("video.qpDelta") == true);
}

TEST_CASE("lock: matches subtrees", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.fec.k") == true);
    REQUIRE(fpvd_is_locked_path("link.fec.n") == true);
    REQUIRE(fpvd_is_locked_path("video.roi.enabled") == true);
    REQUIRE(fpvd_is_locked_path("video.roi.center") == true);
}

TEST_CASE("lock: does not match unrelated", "[fpvd][lock]") {
    REQUIRE(fpvd_is_locked_path("link.channel") == false);
    REQUIRE(fpvd_is_locked_path("video.fps") == false);
    REQUIRE(fpvd_is_locked_path("video.codec") == false);
    REQUIRE(fpvd_is_locked_path("image.mirror") == false);
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

TEST_CASE("endpoint: keymap entries carry the right endpoint + applyTo", "[fpvd][endpoint]") {
    const fpvd_keymap_entry_t *e;

    e = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(e->endpoint == FPVD_EP_AIR);

    e = fpvd_keymap_lookup("gs", "wfbng", "gs_channel");
    REQUIRE(e->endpoint == FPVD_EP_LINK);
    REQUIRE(std::strcmp(e->apply_to, "both") == 0);

    e = fpvd_keymap_lookup("gs", "wfbng", "bandwidth");
    REQUIRE(e->endpoint == FPVD_EP_LINK);
    REQUIRE(std::strcmp(e->apply_to, "both") == 0);

    /* GS card power: percent slider -> GS link.txpower, GS-only apply. */
    e = fpvd_keymap_lookup("gs", "link", "rx_power");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txpower") == 0);
    REQUIRE(e->type == FPVD_T_RXPOWER);
    REQUIRE(e->endpoint == FPVD_EP_LINK);
    REQUIRE(std::strcmp(e->apply_to, "gs") == 0);

    /* Drone TX power (1..63 driver units): air endpoint. */
    e = fpvd_keymap_lookup("gs", "wfbng", "txpower");
    REQUIRE(e != nullptr);
    REQUIRE(std::strcmp(e->path, "link.txpower") == 0);
    REQUIRE(e->endpoint == FPVD_EP_AIR);
}

TEST_CASE("endpoint: routing helpers pick air vs link paths", "[fpvd][endpoint]") {
    const fpvd_keymap_entry_t *air = fpvd_keymap_lookup("air", "camera", "fps");
    REQUIRE(std::strcmp(fpvd_write_path(air->endpoint), "/air/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(air->endpoint), "/air/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(air->endpoint),  "/air/config") == 0);

    const fpvd_keymap_entry_t *lnk = fpvd_keymap_lookup("gs", "wfbng", "gs_channel");
    REQUIRE(std::strcmp(fpvd_write_path(lnk->endpoint), "/link") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(lnk->endpoint), "/link/apply") == 0);
    REQUIRE(std::strcmp(fpvd_read_path(lnk->endpoint),  "/link") == 0);
}

TEST_CASE("path helpers route EP_CONFIG to /config and /apply", "[fpvd][endpoint]") {
    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_CONFIG), "/config") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_CONFIG), "/apply")  == 0);
    REQUIRE(std::strcmp(fpvd_read_path (FPVD_EP_CONFIG), "/config") == 0);
    // existing groups unchanged
    REQUIRE(std::strcmp(fpvd_write_path(FPVD_EP_LINK), "/link") == 0);
    REQUIRE(std::strcmp(fpvd_apply_path(FPVD_EP_AIR),  "/air/apply") == 0);
}
