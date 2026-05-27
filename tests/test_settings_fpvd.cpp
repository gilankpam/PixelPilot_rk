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
