#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <sys/stat.h>
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
    REQUIRE(slurp(p2).find("X='a b'") != std::string::npos);
    pp_gs_write_result_free(&r2); fs::remove(p2);
}

TEST_CASE("env: single-quotes value with embedded quote", "[gs][writers]") {
    auto p = write_temp("");
    auto r = pp_gs_env_set(p.c_str(), "MSG", "say \"hi\"");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("MSG='say \"hi\"'") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("env: single-quotes value with embedded single quote escaped", "[gs][writers]") {
    auto p = write_temp("");
    auto r = pp_gs_env_set(p.c_str(), "MSG", "it's");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    /* Standard sh escape: 'it'\''s' */
    REQUIRE(out.find("MSG='it'\\''s'") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("wfbcfg: append inserts newline when source lacks trailing newline", "[gs][writers]") {
    /* No trailing newline, no [common] header — forces the append branch. */
    auto p = write_temp("foo = bar");
    auto r = pp_gs_wfbcfg_set_channel(p.c_str(), "36");
    REQUIRE(r.rc == 0);
    auto out = slurp(p);
    REQUIRE(out.find("foo = bar\nwifi_channel = 36") != std::string::npos);
    pp_gs_write_result_free(&r); fs::remove(p);
}

TEST_CASE("atomic: write failure on missing parent leaves target absent", "[gs][writers]") {
    /* Parent dir does not exist -> mkstemp fails -> target is not created. */
    std::string bogus = "/tmp/ppgsw_nonexistent_dir/x.cfg";
    fs::remove_all("/tmp/ppgsw_nonexistent_dir");
    auto r = pp_gs_wfbcfg_set_channel(bogus.c_str(), "44");
    REQUIRE(r.rc == -1);
    REQUIRE(r.err != nullptr);
    REQUIRE_FALSE(fs::exists(bogus));
    pp_gs_write_result_free(&r);
}

TEST_CASE("atomic: existing target untouched when rename fails", "[gs][writers]") {
    /* Target file exists with known content. Force atomic_write into a failure
     * by pointing at a tmp slot under a non-existent dir — mkstemp(NULL parent)
     * will fail before rename, so the original target stays as-is. */
    auto orig = write_temp("[common]\nwifi_channel = 36\n");
    /* Move it under a path whose .tmpXXXXXX sibling cannot be created. We can't
     * easily simulate that without root permissions, so instead simulate by
     * making the dir read-only. */
    auto dir = std::string("/tmp/ppgsw_ro_") + std::to_string(getpid());
    fs::create_directory(dir);
    auto target = dir + "/wfb.cfg";
    std::ofstream(target) << "[common]\nwifi_channel = 36\n";
    REQUIRE(chmod(dir.c_str(), 0500) == 0);  /* r-x: no write */
    auto r = pp_gs_wfbcfg_set_channel(target.c_str(), "44");
    /* Restore so cleanup works. */
    chmod(dir.c_str(), 0700);
    REQUIRE(r.rc == -1);
    REQUIRE(slurp(target).find("wifi_channel = 36") != std::string::npos);
    REQUIRE(slurp(target).find("wifi_channel = 44") == std::string::npos);
    pp_gs_write_result_free(&r);
    fs::remove(target);
    fs::remove(dir);
    fs::remove(orig);
}
