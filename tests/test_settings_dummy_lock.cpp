#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "gsmenu/settings.h"
}

TEST_CASE("dummy lock: FEC mode always editable; swfec frees deadline/overhead "
          "and locks the compute knobs", "[caps][dummy]") {
    pp_settings_register_dummy();

    /* Dynamic Link ON + swfec. */
    pp_settings_set("air", "dlink", "enabled", "on");
    pp_settings_set("air", "wfbng", "fec_mode", "swfec");

    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_overhead_pct") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_n") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == true);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == true);

    /* Switch to rs: compute knobs editable; deadline/overhead locked (hidden). */
    pp_settings_set("air", "wfbng", "fec_mode", "rs");
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_mode") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_blocks_per_frame") == false);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_deadline_ms") == true);
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == true);

    /* Dynamic Link OFF: nothing locked. */
    pp_settings_set("air", "dlink", "enabled", "off");
    REQUIRE(pp_settings_is_locked("air", "wfbng", "fec_k") == false);
    REQUIRE(pp_settings_is_locked("air", "dlink", "compute_base_redundancy") == false);
}
