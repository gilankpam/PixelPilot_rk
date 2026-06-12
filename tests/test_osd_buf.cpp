#include <catch2/catch_test_macros.hpp>
#include <set>

#include "../src/osd_buf.hpp"

/* The Cairo OSD refresh path picks its paint target from osd_buf_switch,
 * which the LVGL flush callback can park on ANY buffer index — including 2,
 * since LVGL double-buffers into osd_bufs[1] and osd_bufs[2]. The next paint
 * index must therefore stay in bounds for every possible current index, not
 * just the 0/1 ping-pong the pre-triple-buffer code assumed. */

TEST_CASE("next paint buffer stays in bounds for every current index", "[osd_buf]") {
    const unsigned count = 3;
    for (unsigned cur = 0; cur < count; ++cur) {
        unsigned next = osd_next_paint_buf(cur, count);
        INFO("cur=" << cur);
        REQUIRE(next < count);
    }
}

TEST_CASE("next paint buffer is never the buffer queued for scanout", "[osd_buf]") {
    const unsigned count = 3;
    for (unsigned cur = 0; cur < count; ++cur) {
        INFO("cur=" << cur);
        REQUIRE(osd_next_paint_buf(cur, count) != cur);
    }
}

TEST_CASE("repeated refreshes cycle through all buffers", "[osd_buf]") {
    const unsigned count = 3;
    /* Start from index 2 — where the LVGL flush callback leaves
     * osd_buf_switch after a menu session that ends on its second buffer. */
    unsigned cur = 2;
    std::set<unsigned> visited;
    for (int i = 0; i < 6; ++i) {
        cur = osd_next_paint_buf(cur, count);
        REQUIRE(cur < count);
        visited.insert(cur);
    }
    REQUIRE(visited.size() == count);
}

TEST_CASE("two-buffer config keeps the legacy ping-pong", "[osd_buf]") {
    REQUIRE(osd_next_paint_buf(0, 2) == 1);
    REQUIRE(osd_next_paint_buf(1, 2) == 0);
}
