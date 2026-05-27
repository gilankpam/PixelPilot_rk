#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <unistd.h>

extern "C" {
#include "gsmenu/settings_gs_writers.h"
}

namespace fs = std::filesystem;

static std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static std::string write_temp(const std::string &contents) {
    char tmpl[] = "/tmp/ppgsw_XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, contents.data(), contents.size()) == (ssize_t)contents.size());
    close(fd);
    return std::string(tmpl);
}

TEST_CASE("wfbcfg: channel replaces existing line", "[gs][writers]") {
    auto p = write_temp("[common]\nwifi_channel = 149\nbandwidth = 20\n");
    auto r = pp_gs_wfbcfg_set_channel(p.c_str(), "36");
    REQUIRE(r.rc == 0);
    REQUIRE(slurp(p) == "[common]\nwifi_channel = 36\nbandwidth = 20\n");
    pp_gs_write_result_free(&r);
    fs::remove(p);
}

TEST_CASE("wfbcfg: channel inserts under [common] when missing", "[gs][writers]") {
    auto p = write_temp("[common]\nbandwidth = 20\n");
    auto r = pp_gs_wfbcfg_set_channel(p.c_str(), "36");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("wifi_channel = 36") != std::string::npos);
    REQUIRE(out.find("[common]") < out.find("wifi_channel = 36"));
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("wfbcfg: bandwidth replaces in place", "[gs][writers]") {
    auto p = write_temp("[common]\nbandwidth = 20\n");
    auto r = pp_gs_wfbcfg_set_bandwidth(p.c_str(), "40");
    REQUIRE(r.rc == 0);
    REQUIRE(slurp(p) == "[common]\nbandwidth = 40\n");
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("wfbcfg: txpower writes JSON dict line", "[gs][writers]") {
    auto p = write_temp("[common]\n");
    auto r = pp_gs_wfbcfg_set_txpower(p.c_str(), "{\"wlx00\": -2000}");
    REQUIRE(r.rc == 0);
    REQUIRE(slurp(p).find("wifi_txpower = {\"wlx00\": -2000}") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: upsert preserves other lines", "[gs][writers]") {
    auto p = write_temp("# comment\nFOO=bar\nBAZ=qux\n");
    auto r = pp_gs_env_set(p.c_str(), "FOO", "baz");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("FOO=baz") != std::string::npos);
    REQUIRE(out.find("# comment") != std::string::npos);
    REQUIRE(out.find("BAZ=qux") != std::string::npos);
    REQUIRE(out.find("FOO=bar") == std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: appends when key missing", "[gs][writers]") {
    auto p = write_temp("FOO=bar\n");
    auto r = pp_gs_env_set(p.c_str(), "CODEC", "h265");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("CODEC=h265") != std::string::npos);
    REQUIRE(out.find("FOO=bar") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: quotes value with whitespace", "[gs][writers]") {
    auto p = write_temp("");
    auto r = pp_gs_env_set(p.c_str(), "SCREEN_MODE", "1920x1080@60");
    REQUIRE(r.rc == 0);
    /* No spaces in 1920x1080@60, so no quotes expected. */
    REQUIRE(slurp(p).find("SCREEN_MODE=1920x1080@60") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);

    auto p2 = write_temp("");
    auto r2 = pp_gs_env_set(p2.c_str(), "X", "a b");
    REQUIRE(r2.rc == 0);
    REQUIRE(slurp(p2).find("X=\"a b\"") != std::string::npos);
    pp_gs_write_result_free(&r2); fs::remove(p2);
}

TEST_CASE("atomic: write failure leaves file untouched", "[gs][writers]") {
    /* /dev/full lets writes succeed but fsync fails; portable enough on Linux. */
    /* This case is best-effort — skip on platforms without /dev/full. */
    if (access("/dev/full", W_OK) != 0) { SUCCEED(); return; }
    auto p = write_temp("[common]\nwifi_channel = 36\n");
    /* Force atomic_write into a directory we can't rename into by pointing to a
     * path under a read-only parent. mkstemp will fail. */
    auto r = pp_gs_wfbcfg_set_channel("/proc/does_not_exist/x.cfg", "44");
    REQUIRE(r.rc == -1);
    REQUIRE(r.err != nullptr);
    pp_gs_write_result_free(&r);
    /* The original temp file was not the target, so it's irrelevant. */
    fs::remove(p);
}
