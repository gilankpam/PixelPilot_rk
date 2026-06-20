# Raw DVR Timing from RTP Timestamps — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the raw DVR derive each frame's MP4 duration from the stream's RTP timestamps instead of a declared fps, and remove the Raw FPS configuration.

**Architecture:** The HEVC depayloader already parses the per-frame RTP timestamp (90 kHz) and emits one access unit per frame; thread that timestamp through the receiver and frame callback to a new raw-timing entry on `Dvr`, which sets each sample's duration to the wrap-safe RTP-timestamp delta (with a 60 fps fallback). Then delete the `--dvr-framerate` flag/gate and the gsmenu "Raw FPS" row.

**Tech Stack:** C++17, custom RTP receiver + `HevcDepayloader`, minimp4, Rockchip MPP/RGA (device), Catch2 (host tests), aarch64 Buildroot cross-build.

**Spec:** `docs/superpowers/specs/2026-06-21-dvr-raw-fps-from-rtp-design.md`

## Global Constraints

- Raw frame duration = wrap-safe RTP-timestamp delta in 90 kHz units: `(uint32_t)(ts - last_ts)`. RTP video clock is assumed **90 kHz** (H.265 RTP standard).
- First frame / duplicate ts (`delta == 0`) / `delta > 90000` (>1 s) → nominal fallback `90000/60 = 1500`. The fallback fps (60) is an internal constant, NOT user config.
- `--dvr-framerate` becomes **accept-and-ignore** with a one-time `spdlog::warn` (still in launch args) — never a parse error; the raw-required startup gate is removed.
- gsmenu "Raw FPS" row (`rec_fps`) + its `settings_fpvd.c`/`settings_dummy.c` entries removed.
- Re-encode path unchanged. minimp4 `mp4_h26x_write_nal`'s 3rd arg is the per-sample duration in 90 kHz units (matches RTP delta directly).
- Device cross-build gate (success = `PPBUILD_DONE rc=0`):
  ```
  printf '%s\n' 'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' 'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' 'export DEFCONFIG=radxa_zero3_defconfig' 'export LD_LIBRARY_PATH=/lib:$LD_LIBRARY_PATH' './build.sh pixelpilot-rebuild; echo "PPBUILD_DONE rc=$?"' | nix-shell /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/shell.nix 2>&1 | tail -12
  ```
- Host test build/run: `nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null 2>&1 && cmake --build build-test --target <t> -j4 && ./build-test/<t>"`.
- Host sim build (gsmenu compile): `nix-shell shell-sim.nix --run "cmake -S . -B build-sim -DUSE_SIMULATOR=ON -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 && cmake --build build-sim --target pixelpilot -j4"`.
- GS deploy/record/verify: see `reference_dvr_recording_test` memory (SIGUSR1 record, GS clock=2017 so find files by sequence, pull to host for ffprobe).

---

### Task 1: `dvr_rtp_duration_90k` helper + host tests

**Files:**
- Modify: `src/dvr_timing.h` (add the function next to `dvr_frame_duration_90k`)
- Modify: `tests/test_dvr_timing.cpp` (add cases; built by the existing `dvr_timing_tests` target)

**Interfaces:**
- Produces: `int dvr_rtp_duration_90k(uint32_t ts, uint32_t last_ts, bool have_last, int fallback_fps)` — consumed by Task 2.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_dvr_timing.cpp`:
```cpp
TEST_CASE("rtp duration: first frame uses nominal fallback", "[dvr_timing]") {
    REQUIRE(dvr_rtp_duration_90k(123456, 0, /*have_last=*/false, 60) == 1500);
    REQUIRE(dvr_rtp_duration_90k(0, 0, false, 30) == 3000);
}
TEST_CASE("rtp duration: steady delta passes through (90 kHz)", "[dvr_timing]") {
    REQUIRE(dvr_rtp_duration_90k(1500, 0, true, 60) == 1500);   // 60 fps
    REQUIRE(dvr_rtp_duration_90k(3000, 0, true, 60) == 3000);   // 30 fps
}
TEST_CASE("rtp duration: 32-bit wrap is handled", "[dvr_timing]") {
    // last just below wrap, ts just after -> forward delta 1500
    REQUIRE(dvr_rtp_duration_90k(1000u, 0xFFFFFFFFu - 499u, true, 60) == 1500);
}
TEST_CASE("rtp duration: duplicate / oversized gap fall back to nominal", "[dvr_timing]") {
    REQUIRE(dvr_rtp_duration_90k(5000, 5000, true, 60) == 1500); // delta 0
    REQUIRE(dvr_rtp_duration_90k(200000, 0, true, 60) == 1500);  // delta > 90000
}
TEST_CASE("rtp duration: invalid fallback_fps still sane", "[dvr_timing]") {
    REQUIRE(dvr_rtp_duration_90k(0, 0, false, 0) == 1500);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `nix-shell shell-sim.nix --run "cmake --build build-test --target dvr_timing_tests -j4" 2>&1 | tail -5`
Expected: FAIL — `dvr_rtp_duration_90k` not declared.

- [ ] **Step 3: Implement** — add to `src/dvr_timing.h` (after `dvr_frame_duration_90k`):
```cpp
// Raw-DVR per-frame MP4 duration (90 kHz) from consecutive RTP timestamps
// (uint32, 90 kHz, wrap-safe forward delta). First frame, duplicate timestamp,
// or a gap over 1 s falls back to the nominal 1/fallback_fps.
inline int dvr_rtp_duration_90k(uint32_t ts, uint32_t last_ts, bool have_last, int fallback_fps) {
    const int nominal = (fallback_fps > 0) ? 90000 / fallback_fps : 1500;
    if (!have_last) return nominal;
    uint32_t d = ts - last_ts;                  // wrap-safe forward delta
    if (d == 0 || d > 90000) return nominal;    // duplicate ts / >1 s gap
    return (int)d;
}
```
(Add `#include <cstdint>` if not already present — it is.)

- [ ] **Step 4: Run to verify it passes**

Run: `nix-shell shell-sim.nix --run "cmake --build build-test --target dvr_timing_tests -j4 >/dev/null 2>&1 && ./build-test/dvr_timing_tests" 2>&1 | tail -2`
Expected: `All tests passed (... in 16 test cases)`.

- [ ] **Step 5: Commit**
```bash
git add src/dvr_timing.h tests/test_dvr_timing.cpp
git commit -m "feat(dvr): add dvr_rtp_duration_90k helper for raw timing"
```

---

### Task 2: Thread RTP timestamp depay→receiver→DVR and use it for raw duration

**Files:**
- Modify: `src/hevc_depayloader.h` (`FrameCallback` signature)
- Modify: `src/hevc_depayloader.cpp` (`flush_au` passes `cur_ts_`)
- Modify: `tests/test_hevc_depayloader.cpp` (callback lambda signature + assert ts propagates)
- Modify: `src/rtp_video_receiver.h` (`NEW_FRAME_CALLBACK` signature)
- Modify: `src/rtp_video_receiver.cpp` (depay callback + forward ts to `m_cb`)
- Modify: `src/main.cpp` (`g_video_frame_cb` signature + feed `dvr_raw`)
- Modify: `src/dvr.h` / `src/dvr.cpp` (`frame_rtp` entry + raw-timing duration)

**Interfaces:**
- Consumes: `dvr_rtp_duration_90k` (Task 1).
- Produces: `void Dvr::frame_rtp(std::shared_ptr<std::vector<uint8_t>> frame, uint32_t rtp_ts);`

Device-only except the depayloader (host-tested). Test cycle = `rtp_depay_tests` (host) + cross-build.

- [ ] **Step 1: Depayloader emits the RTP timestamp**

`src/hevc_depayloader.h`: change
`using FrameCallback = std::function<void(const uint8_t* au, size_t len)>;`
→ `using FrameCallback = std::function<void(const uint8_t* au, size_t len, uint32_t rtp_ts)>;`

`src/hevc_depayloader.cpp` `flush_au()`: both `cb_(out.data(), out.size())` and `cb_(au_.data(), au_.size())` calls become `cb_(out.data(), out.size(), cur_ts_)` / `cb_(au_.data(), au_.size(), cur_ts_)`.

- [ ] **Step 2: Update depayloader host tests (RED then GREEN)**

In `tests/test_hevc_depayloader.cpp`, update the capture lambda passed to `HevcDepayloader` to the 3-arg signature and record the ts. Add one assertion in an existing multi-frame test that the emitted `rtp_ts` equals the input RTP timestamp for that AU. Run:
`nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null 2>&1 && cmake --build build-test --target rtp_depay_tests -j4 && ./build-test/rtp_depay_tests" 2>&1 | tail -3`
Expected: builds (after the signature update) and passes, including the new ts assertion.

- [ ] **Step 3: Receiver forwards the timestamp**

`src/rtp_video_receiver.h`: change
`using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;`
→ `using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>, uint32_t rtp_ts)>;`

`src/rtp_video_receiver.cpp`: the lambda the receiver gives `HevcDepayloader` now takes `(const uint8_t* au, size_t len, uint32_t rtp_ts)`; where it builds the `shared_ptr<vector>` and calls `m_cb`, change to `m_cb(frame, rtp_ts)`.

- [ ] **Step 4: `Dvr::frame_rtp` + raw-timing duration**

`src/dvr.h`: add to `dvr_rpc` (the RPC struct): `uint32_t rtp_ts = 0; bool use_rtp_timing = false;`. Add member to `Dvr`: `uint32_t last_rtp_ts_ = 0; bool have_rtp_ts_ = false;`. Declare `void frame_rtp(std::shared_ptr<std::vector<uint8_t>> frame, uint32_t rtp_ts);`.

`src/dvr.cpp`:
- `#include "dvr_timing.h"` is already present.
- Implement:
```cpp
void Dvr::frame_rtp(std::shared_ptr<std::vector<uint8_t>> frame, uint32_t rtp_ts) {
    dvr_rpc rpc = { .command = dvr_rpc::RPC_FRAME, .frame = frame,
                    .rtp_ts = rtp_ts, .use_rtp_timing = true };
    enqueue_dvr_command(rpc);
}
```
- In the `RPC_FRAME` duration block, add the RTP branch FIRST (before the existing `rpc.pts_ms >= 0` re-encode branch):
```cpp
int dur;
if (rpc.use_rtp_timing) {
    dur = dvr_rtp_duration_90k(rpc.rtp_ts, last_rtp_ts_, have_rtp_ts_, 60);
    last_rtp_ts_ = rpc.rtp_ts;
    have_rtp_ts_ = true;
} else if (rpc.pts_ms >= 0) {
    /* ... existing re-encode block unchanged ... */
} else {
    /* ... existing nominal fallback unchanged ... */
}
```
- Reset in `start()` and `split()`: `have_rtp_ts_ = false;` (next segment's first frame uses nominal).

(The depay emits exactly one AU per frame, so every `frame_rtp` is a real frame — no VCL check needed for the raw path.)

- [ ] **Step 5: Feed the raw DVR with the timestamp in main.cpp**

`src/main.cpp` `g_video_frame_cb`: change the lambda to
`[](std::shared_ptr<std::vector<uint8_t>> frame, uint32_t rtp_ts){ ... }`,
keep the decoder-feed line unchanged, and change `dvr_raw->frame(frame)` →
`dvr_raw->frame_rtp(frame, rtp_ts)`.

- [ ] **Step 6: Cross-build**

Run the Global-Constraints cross-build. Expected: `PPBUILD_DONE rc=0`.

- [ ] **Step 7: Commit**
```bash
git add src/hevc_depayloader.h src/hevc_depayloader.cpp tests/test_hevc_depayloader.cpp src/rtp_video_receiver.h src/rtp_video_receiver.cpp src/dvr.h src/dvr.cpp src/main.cpp
git commit -m "feat(dvr): time raw recordings from RTP timestamps"
```

---

### Task 3: Remove the Raw FPS configuration

**Files:**
- Modify: `src/main.cpp` (`--dvr-framerate` accept-and-ignore; drop the raw-required startup gate; drop `video_framerate` plumbing into the raw `Dvr` if now unused)
- Modify: `src/gsmenu/pages/pixelpilot.c` (remove the "Raw FPS" `rec_fps` row)
- Modify: `src/gsmenu/settings_fpvd.c`, `src/gsmenu/settings_dummy.c` (remove `rec_fps` entries)
- Possibly: `src/simulator.c` (remove a dead `rec_fps`/framerate stub if present)

**Interfaces:** none new.

- [ ] **Step 1: Confirm the `rec_fps` ↔ `--dvr-framerate` wiring**

Run: `grep -rnE "rec_fps|dvr-framerate|video_framerate|dvr_set_video_framerate" src/`
Confirms every site to touch. (`video_framerate` is now only the raw `Dvr`'s nominal fallback, which the RTP path supersedes.)

- [ ] **Step 2: `--dvr-framerate` accept-and-ignore + drop the gate**

`src/main.cpp`: replace the `__OnArgument("--dvr-framerate")` body with:
```cpp
__OnArgument("--dvr-framerate") {
    (void)__ArgValue;
    spdlog::warn("--dvr-framerate is deprecated and ignored (raw DVR times from RTP timestamps)");
    continue;
}
```
Delete the startup gate block (the `if (... (DVR_MODE_RAW||DVR_MODE_BOTH) && video_framerate < 0)` that prints "--dvr-framerate must be provided when raw DVR is enabled" and exits). Remove the `--dvr-framerate` help-text line. Leave the raw `Dvr`'s `video_framerate` default as the harmless internal fallback, or drop the now-dead `video_framerate` global if nothing else reads it.

- [ ] **Step 3: Remove the gsmenu Raw FPS row**

`src/gsmenu/pages/pixelpilot.c`: delete the `pp_dropdown(... "Raw FPS", "gs","dvr","rec_fps", "30\n60\n90\n120")` row.
`src/gsmenu/settings_fpvd.c`: delete the `rec_fps` keymap entry.
`src/gsmenu/settings_dummy.c`: delete the `{ "rec_fps", ... }` default.
If `src/simulator.c` has a stub tied only to `rec_fps`/raw framerate that is now uncalled, remove it.

- [ ] **Step 4: Verify (all three)**

```
nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null 2>&1 && cmake --build build-test --target fpvd_tests -j4 && ./build-test/fpvd_tests" 2>&1 | tail -3
nix-shell shell-sim.nix --run "cmake -S . -B build-sim -DUSE_SIMULATOR=ON -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 && cmake --build build-sim --target pixelpilot -j4" 2>&1 | tail -4
```
then the Global-Constraints cross-build. Fix any `fpvd_tests` assertion that referenced `rec_fps` (update to a surviving key, like the earlier `dvr_osd` test fixes). Expected: all green + `PPBUILD_DONE rc=0`.

- [ ] **Step 5: Commit**
```bash
git add src/main.cpp src/gsmenu/
git commit -m "feat(dvr): drop Raw FPS config (raw timing now from RTP)"
```

---

### Task 4: On-device verification

**Files:** none (verification only).

- [ ] **Step 1: Deploy** the built binary (binary-only atomic swap + `kill $(pidof pixelpilot)`; verify running-exe md5; per `reference_dvr_recording_test`). Confirm the `--dvr-framerate` deprecation warning logs and pixelpilot still starts.

- [ ] **Step 2: Record a RAW clip.** The GS currently runs `--dvr-mode reencode`; to exercise raw, set `--dvr-mode both` (or `raw`) for the test (via the launch args / a manual run), then `kill -USR1 $(pidof pixelpilot)` to start, ~25 s, `kill -USR1` to stop. Find the new `*_raw.mp4` (highest sequence; GS clock is 2017 so don't use `ls -t`).

- [ ] **Step 3: Verify** (pull to host; retry scp — flaky link):
```bash
DUR=$(ffprobe -v error -show_entries format=duration -of default=nk=1:nw=1 raw.mp4)
NF=$(ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of default=nk=1:nw=1 raw.mp4)
ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 raw.mp4 | awk -F, '{if(p!=""){d=($1-p)*1000;s+=d;n++} p=$1} END{printf "mean gap=%.1fms (16.7=60fps), fps=%.1f, dur=%ss\n", s/n, n/($DUR), "'"$DUR"'"}'
```
Expected: duration ≈ the recorded wall-clock window, mean frame gap ≈ the source cadence (≈16.7 ms at 60 fps) — i.e. correct-speed playback **without** any `--dvr-framerate`. **Timebase sanity:** the raw RTP deltas should land near 1500 (60 fps); if the mean gap is wildly off (e.g. 2× or ½×), the RTP clock isn't 90 kHz — stop and rescale (see spec Risks).

- [ ] **Step 4:** If timing is correct, the branch is ready to finish (push + PR / merge per `finishing-a-development-branch`).

---

## Self-Review

**Spec coverage:** RTP-delta duration → Tasks 1+2; thread ts through depay/receiver/main/Dvr → Task 2; pure helper + tests → Task 1; remove `--dvr-framerate`/gate/gsmenu → Task 3; 90 kHz timebase verification → Task 4 Step 3. Re-encode untouched (no task edits it). All covered.

**Placeholder scan:** Task 2 Step 4's "existing re-encode block unchanged" / "existing nominal fallback unchanged" reference concrete current code in `dvr.cpp`'s `RPC_FRAME` (do not rewrite them — only add the RTP branch ahead of them). No "TBD"/"handle errors"/un-shown code.

**Type consistency:** `dvr_rtp_duration_90k(uint32_t, uint32_t, bool, int)` identical in Task 1 (def) and Task 2 Step 4 (use). `frame_rtp(shared_ptr<vector<uint8_t>>, uint32_t)` consistent (decl Task 2 Step 4, call Task 2 Step 5). Depay `FrameCallback`/receiver `NEW_FRAME_CALLBACK` 3-arg / 2-arg+ts signatures consistent across Steps 1, 3, 5.
