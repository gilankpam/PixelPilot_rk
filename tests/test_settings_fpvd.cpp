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
