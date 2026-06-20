# DVR Re-encode Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse the re-encode DVR's four config knobs (codec/resolution/fps/bitrate) to a single bitrate knob — h265, screen resolution, fps capped at the display refresh, OSD always burned in — and replace the constant-fps pacer with one decimating loop.

**Architecture:** The `FrameProcessor` two-thread pacer (processor + 60 Hz timer with repeat/grace/re-anchor + `last_copy` swap) becomes a single loop driven by decoded-frame arrival, with a frame-rate cap (decimator). Output is variable-fps ≤ cap; the existing PTS-delta muxing keeps duration correct. Resolution = screen mode dims (OSD already rendered there, so the single-pass `imcomposite` always matches). Removed CLI flags are accepted-and-ignored for launcher compatibility.

**Tech Stack:** C++17, Rockchip MPP (encode), librga (RGA 2D: resize + `imcomposite`), spdlog, Catch2 (host unit tests), aarch64 Buildroot cross-build.

## Global Constraints

- Re-encode codec is always **h265** (`VideoCodec::H265`).
- Re-encode resolution = screen mode dims: `output_list->mode.hdisplay × output_list->mode.vdisplay`.
- fps cap = `output_list->mode.vrefresh` (frames decimated above it; input at/below passes through as VFR).
- Re-encode bitrate stays configurable; default **8000 kbps**; `--dvr-reenc-bitrate` and gsmenu bitrate row are kept.
- Re-encode **always** burns in the OSD (no toggle). An OSD-free copy comes from `raw` / the raw side of `both`.
- Removed CLI flags (`--dvr-reenc-codec`, `--dvr-reenc-fps`, `--dvr-reenc-resolution`, `--dvr-osd`) are **accepted-and-ignored** with a one-time deprecation log — never a parse error.
- Raw DVR path is **unchanged**.
- Rate control stays **CBR**.
- The aarch64 cross-build is the device compile+link gate (host can't build these files). Command:
  ```
  printf '%s\n' \
    'cd /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam' \
    'export PIXELPILOT_OVERRIDE_SRCDIR=/home/gilankpam/Projects/drone/PixelPilot_rk' \
    'export DEFCONFIG=radxa_zero3_defconfig' \
    'export LD_LIBRARY_PATH=/lib:$LD_LIBRARY_PATH' \
    './build.sh pixelpilot-rebuild; echo "PPBUILD_DONE rc=$?"' \
    | nix-shell /home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/shell.nix 2>&1 | tail -5
  ```
  Success = `PPBUILD_DONE rc=0` + `Build completed successfully!`.
- Host unit-test build: `nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null && cmake --build build-test --target <target> -j4 && ./build-test/<target>"`.
- GS deploy/record/verify: branch `feat/dvr-reencode-simplify`; SSH `root@10.18.0.1` (see `reference_dvr_recording_test` / `reference_gs_deploy` memory): binary-only atomic swap + `kill $(pidof pixelpilot)`; record via `kill -USR1`; find new file by highest sequence (GS clock is 2017, so `ls -t` misleads); analyze clips on the x86 host with `ffprobe`/`ffmpeg`.

---

### Task 1: fps-cap decimator (pure, host-tested)

**Files:**
- Create: `src/dvr_fps_cap.h`
- Test: `tests/test_dvr_fps_cap.cpp`
- Modify: `CMakeLists.txt` (add `dvr_fps_cap_tests` target, after the `dvr_timing_tests` block)

**Interfaces:**
- Produces: `class FpsCap { explicit FpsCap(int cap_fps); bool should_emit(int64_t now_us); };` — `should_emit` returns true if a frame captured at `now_us` (monotonic microseconds) should be encoded, advancing internal state. `cap_fps <= 0` means "no cap" (always emit). Consumed by Task 2.

- [ ] **Step 1: Write the failing test**

`tests/test_dvr_fps_cap.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "../src/dvr_fps_cap.h"

// Drive a stream of input frames spaced `step_us` apart through the cap and
// return how many were emitted over `count` frames.
static int emitted(int cap_fps, int64_t step_us, int count) {
    FpsCap cap(cap_fps);
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (cap.should_emit((int64_t)i * step_us)) n++;
    return n;
}

TEST_CASE("first frame always emits", "[fps_cap]") {
    FpsCap cap(60);
    REQUIRE(cap.should_emit(0) == true);
}

TEST_CASE("input at the cap passes through unchanged", "[fps_cap]") {
    // 60 fps in, cap 60 -> all 60 frames in one second emit.
    REQUIRE(emitted(60, 16667, 60) == 60);
}

TEST_CASE("input below the cap passes through unchanged", "[fps_cap]") {
    // 30 fps in, cap 60 -> all 30 frames emit (no padding/repeats).
    REQUIRE(emitted(30, 33333, 30) == 30);
}

TEST_CASE("90 fps input decimates to ~60", "[fps_cap]") {
    // 90 frames over ~1s at the cap of 60 -> ~60 emitted (allow small jitter).
    int n = emitted(60, 11111, 90);
    REQUIRE(n >= 58);
    REQUIRE(n <= 62);
}

TEST_CASE("120 fps input decimates to ~60", "[fps_cap]") {
    int n = emitted(60, 8333, 120);
    REQUIRE(n >= 58);
    REQUIRE(n <= 62);
}

TEST_CASE("the decimator never sustains above the cap", "[fps_cap]") {
    // Even very fast input (240 fps) stays within ~the cap over a second.
    int n = emitted(60, 4166, 240);
    REQUIRE(n <= 62);
}

TEST_CASE("cap_fps <= 0 means no cap (emit everything)", "[fps_cap]") {
    REQUIRE(emitted(0, 11111, 50) == 50);
}

TEST_CASE("a long gap resyncs instead of bursting", "[fps_cap]") {
    FpsCap cap(60);
    REQUIRE(cap.should_emit(0) == true);
    // ... silence ...
    REQUIRE(cap.should_emit(5'000'000) == true);   // 5s later: emit
    REQUIRE(cap.should_emit(5'005'000) == false);  // 5ms later: dropped (no backlog burst)
}
```

- [ ] **Step 2: Wire the CMake target and run the test to verify it fails**

In `CMakeLists.txt`, immediately after the `dvr_timing_tests` block, add:
```cmake
    add_executable(dvr_fps_cap_tests
      tests/test_dvr_fps_cap.cpp)
    target_include_directories(dvr_fps_cap_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(dvr_fps_cap_tests Catch2::Catch2WithMain)
```
Run: `nix-shell shell-sim.nix --run "cmake -S . -B build-test >/dev/null 2>&1 && cmake --build build-test --target dvr_fps_cap_tests -j4" 2>&1 | tail -5`
Expected: FAIL — `fatal error: ../src/dvr_fps_cap.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

`src/dvr_fps_cap.h`:
```cpp
#ifndef DVR_FPS_CAP_H
#define DVR_FPS_CAP_H

#include <cstdint>

// Frame-rate cap / decimator for the re-encode pacer. Emits at up to cap_fps:
// input above the cap is decimated (e.g. 90 -> ~60), input at or below passes
// through unchanged (variable-fps, made correct by PTS-delta muxing). The
// accumulator advances the target slot by one interval per emit so the rate
// tracks the cap without the undershoot of a naive "reset to now" throttle; a
// long gap resyncs the slot so silence can't cause a catch-up burst. No
// tolerance is used, so the rate never sustains above the cap (encoder-safe).
class FpsCap {
public:
    explicit FpsCap(int cap_fps)
        : interval_us_(cap_fps > 0 ? 1000000 / cap_fps : 0) {}

    bool should_emit(int64_t now_us) {
        if (interval_us_ <= 0) return true;          // no cap
        if (now_us < next_us_) return false;         // too soon -> drop
        next_us_ += interval_us_;
        if (next_us_ < now_us)                        // first frame or long gap
            next_us_ = now_us + interval_us_;         // resync, no backlog burst
        return true;
    }

private:
    int64_t interval_us_;
    int64_t next_us_ = INT64_MIN;
};

#endif // DVR_FPS_CAP_H
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `nix-shell shell-sim.nix --run "cmake --build build-test --target dvr_fps_cap_tests -j4 >/dev/null 2>&1 && ./build-test/dvr_fps_cap_tests" 2>&1 | tail -3`
Expected: `All tests passed (... assertions in 8 test cases)`.

- [ ] **Step 5: Commit**

```bash
git add src/dvr_fps_cap.h tests/test_dvr_fps_cap.cpp CMakeLists.txt
git commit -m "feat(dvr): add fps-cap decimator for re-encode pacer"
```

---

### Task 2: Collapse the FrameProcessor pacer (single loop + cap + screen-res)

**Files:**
- Modify: `src/frame_processor.h`
- Modify: `src/frame_processor.cpp`
- Modify: `src/main.cpp` (the two `new FrameProcessor(...)` call sites + `reenc_target_dims`)

**Interfaces:**
- Consumes: `FpsCap` from `src/dvr_fps_cap.h`.
- Produces: new constructor `FrameProcessor(MppEncoder *enc, int cap_fps, uint32_t enc_w, uint32_t enc_h)`. Removes `set_fps`, `set_resolution`, `EncResolution` usage, `__TIMER_THREAD__`, `timer_loop`. `push_latest`, `set_osd_blend`, `set_color_correction`, `set_color_correction_enabled`, `drain_decoder_refs`, `shutdown` keep their signatures.

This is device-only code; its test cycle is the cross-build gate (behavioral verification is Task 6).

- [ ] **Step 1: Update the header**

In `src/frame_processor.h`:
- Add `#include "dvr_fps_cap.h"`.
- Change the constructor declaration to:
  ```cpp
  FrameProcessor(MppEncoder *enc, int cap_fps, uint32_t enc_w, uint32_t enc_h);
  ```
- Delete declarations: `void set_fps(int fps);`, `void set_resolution(EncResolution r);`, `static void *__TIMER_THREAD__(void *p);`, `void timer_loop();`.
- Delete members that existed only for the timer/repeat path: `std::atomic<long> interval_ns;`, `std::atomic<int> target_res_{1};`, `std::mutex ready_mtx_;`, `std::condition_variable ready_cv_;`, `bool ready_fresh_{false};`, `MppBuffer last_copy = nullptr;`, `FrameProcFrame last_meta;`.
- Add members: `uint32_t enc_w_ = 0, enc_h_ = 0;` and `int cap_fps_ = 0;`.
- Keep: `running`, `mtx`, `cv_`, `copy_mtx_`, `pending`, `hold_grp`, `proc_copy_`, `blend_rgba_`, `proc_meta_`, the OSD-blend members, and the color-correction members.

- [ ] **Step 2: Rewrite the constructor and remove the timer entry point**

In `src/frame_processor.cpp`, replace the constructor:
```cpp
FrameProcessor::FrameProcessor(MppEncoder *enc, int cap_fps, uint32_t enc_w, uint32_t enc_h)
    : encoder(enc), enc_w_(enc_w), enc_h_(enc_h), cap_fps_(cap_fps) {
    mpp_buffer_group_get_internal(&hold_grp, MPP_BUFFER_TYPE_DRM);
}
```
Delete the `__TIMER_THREAD__` function and the entire `timer_loop()` function.
In the destructor, delete the `if (last_copy) { mpp_buffer_put(last_copy); last_copy = nullptr; }` line.
Delete the `set_color_correction`-adjacent `set_fps`/`set_resolution` definitions if any exist in the .cpp (they are header-inline today, so likely nothing to delete in the .cpp).

- [ ] **Step 3: Rewrite `process_loop` to a single capped loop**

Replace the body of `FrameProcessor::process_loop()` with:
```cpp
void FrameProcessor::process_loop() {
    FpsCap cap(cap_fps_);

    while (running) {
        FrameProcFrame fresh;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv_.wait(lock, [&]{ return pending.buffer != nullptr || !running; });
            if (!running) break;
            if (pending.buffer) { fresh = pending; pending.buffer = nullptr; }
        }
        if (!fresh.buffer) continue;

        if (!dvr_enabled || !encoder) { fresh.release(); continue; }

        // fps cap: drop frames that arrive faster than the display refresh.
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        if (!cap.should_emit(now_us)) { fresh.release(); continue; }

        // ── Resize decoded frame -> screen-res NV12 in proc_copy_ ──
        {
            std::lock_guard<std::mutex> copy_lock(copy_mtx_);
            uint32_t dst_w = enc_w_, dst_h = enc_h_;
            uint32_t dst_hs = align_up(dst_w, 16), dst_vs = align_up(dst_h, 16);
            size_t dst_sz = (size_t)dst_hs * dst_vs * 3 / 2;
            if (hold_grp && !proc_copy_)
                mpp_buffer_get(hold_grp, &proc_copy_, dst_sz);
            if (proc_copy_) {
                // (KEEP the existing color-correct / RGA resize/copy body here
                //  verbatim from the current implementation — lazy CC init, the
                //  color_gl_.process() path, and the imcopy/imresize fallback —
                //  writing into proc_copy_ at dst_w/dst_h/dst_hs/dst_vs, then
                //  setting proc_meta_ to those dims, then fresh.release().)
            } else {
                fresh.release();
                continue;
            }
        }
        if (!proc_copy_) continue;

        // ── Composite OSD (single-pass imcomposite; KEEP the current block) ──
        // (KEEP the existing OSD-blend block verbatim: snapshot osd_info_, and
        //  if valid, imcomposite(nv12, osd, nv12, DST_OVER) with the 3-pass
        //  fallback.)

        // ── Hand the composited buffer to the encoder; take a fresh one next ──
        struct timespec nts;
        clock_gettime(CLOCK_MONOTONIC, &nts);
        uint64_t pts_ms = (uint64_t)nts.tv_sec * 1000 + nts.tv_nsec / 1000000;
        encoder->push_frame(proc_copy_, proc_meta_.width, proc_meta_.height,
                            proc_meta_.hor_stride, proc_meta_.ver_stride,
                            proc_meta_.fmt, pts_ms);
        proc_copy_ = nullptr;   // ownership transferred; next iter allocates fresh
    }

    // Release pending decoder buffer on exit
    std::lock_guard<std::mutex> lock(mtx);
    pending.release();
}
```
Notes for the implementer:
- The resize body and the OSD-blend block already exist in the current `process_loop` — move them in unchanged; only the surrounding structure (cap, no timer, direct push + `proc_copy_ = nullptr`) changes.
- `encoder->push_frame` takes ownership of `proc_copy_`'s single ref (the encoder calls `mpp_buffer_put` when done); do **not** add an `mpp_buffer_inc_ref`. This preserves the buffer-race fix (each pushed frame is its own pool buffer, never reused while in flight).
- Remove the old "Timing diagnostics" block and the publish/swap block entirely.

- [ ] **Step 4: Update the two construction sites in `main.cpp`**

Replace `reenc_target_dims(uint32_t&,uint32_t&)` so it returns the screen mode dims (used by the reenc DVR's `set_video_params`):
```cpp
static void reenc_target_dims(uint32_t &w, uint32_t &h) {
    w = output_list ? output_list->mode.hdisplay : 1920;
    h = output_list ? output_list->mode.vdisplay : 1080;
}
```
At both `frame_proc = new FrameProcessor(...)` sites (startup wiring ~line 1566 and `dvr_set_mode` ~line 742), change to:
```cpp
uint32_t rw, rh; reenc_target_dims(rw, rh);
int cap = output_list ? output_list->mode.vrefresh : 60;
frame_proc = new FrameProcessor(reencoder, cap, rw, rh);
```
(Leave the `reenc_params.fps` field and `args.video_framerate = reenc_params.fps` for the DVR muxer's nominal fallback as-is for now; the muxer uses real PTS deltas regardless.)

- [ ] **Step 5: Cross-build to verify it compiles and links**

Run the Global-Constraints cross-build command.
Expected: `PPBUILD_DONE rc=0`.

- [ ] **Step 6: Commit**

```bash
git add src/frame_processor.h src/frame_processor.cpp src/main.cpp
git commit -m "refactor(dvr): collapse re-encode pacer to a single capped loop"
```

---

### Task 3: Fix codec to h265 and drop the codec/fps/resolution config

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: the FrameProcessor from Task 2 (already screen-res + cap, ignores `reenc_params.fps/resolution`).
- Produces: `dvr_reenc_set_codec`, `dvr_reenc_set_fps`, `dvr_reenc_set_resolution`, `dvr_reenc_get_codec`, `dvr_reenc_get_fps`, `dvr_reenc_get_resolution` are removed (Task 5's gsmenu cleanup depends on these being gone).

Device-only; test cycle is the cross-build gate.

- [ ] **Step 1: Hardcode the codec**

Change the `reenc_params` declaration so the codec is h265 regardless of input:
```cpp
MppEncoderParams reenc_params;          // bitrate/fps fields still used; codec forced below
```
At program start (right after args are parsed, near where `reenc_params` is finalized — search for the `--dvr-reenc-codec` handling), set:
```cpp
reenc_params.codec = VideoCodec::H265;
```

- [ ] **Step 2: Make the removed flags accept-and-ignore**

Replace the `__OnArgument("--dvr-reenc-codec")`, `__OnArgument("--dvr-reenc-fps")`, and `__OnArgument("--dvr-reenc-resolution")` blocks with no-op consumers that swallow their value and log once:
```cpp
__OnArgument("--dvr-reenc-codec") {
    (void)__OnArgvalue;   // deprecated: re-encode codec is always h265
    spdlog::warn("--dvr-reenc-codec is deprecated and ignored (codec is h265)");
    continue;
}
__OnArgument("--dvr-reenc-fps") {
    (void)__OnArgvalue;   // deprecated: fps follows input, capped at display refresh
    spdlog::warn("--dvr-reenc-fps is deprecated and ignored (fps follows input, capped at display refresh)");
    continue;
}
__OnArgument("--dvr-reenc-resolution") {
    (void)__OnArgvalue;   // deprecated: resolution follows the screen mode
    spdlog::warn("--dvr-reenc-resolution is deprecated and ignored (resolution = screen mode)");
    continue;
}
```
(Use whatever value-fetch macro the surrounding `__OnArgument` blocks use — match the existing `--dvr-reenc-bitrate` block's style for consuming the argument value.)

- [ ] **Step 3: Remove the obsolete C control + getter functions**

In the `extern "C"` block, delete these functions entirely: `dvr_reenc_set_fps`, `dvr_reenc_set_codec`, `dvr_reenc_set_resolution`, `dvr_reenc_get_fps`, `dvr_reenc_get_codec`, `dvr_reenc_get_resolution`. Keep `dvr_reenc_set_bitrate` and `dvr_reenc_get_bitrate`. Update the help text block (the `--dvr-reenc-*` lines) to drop codec/fps/resolution and keep bitrate.

- [ ] **Step 4: Cross-build**

Run the cross-build command. Expected: `PPBUILD_DONE rc=0`. (If the link fails on a removed `dvr_reenc_*` symbol, it's still referenced by gsmenu — that's expected and fixed in Task 5; to keep this task's build green, do Task 5 before re-running, or temporarily keep the getters. Prefer: implement Task 5 in the same review cycle.)

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat(dvr): fix re-encode codec to h265, drop codec/fps/resolution config"
```

---

### Task 4: OSD always-on for re-encode

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/osd.cpp`

**Interfaces:**
- Produces: removes the `dvr_osd` global, `--dvr-osd` flag, `dvr_reenc_set_osd`, `dvr_reenc_get_osd`. OSD is published to the recorder whenever `frame_proc != nullptr`.

Device-only; both files must change together to stay compilable.

- [ ] **Step 1: Remove `dvr_osd` from main.cpp**

- Delete the global `bool dvr_osd = false;`.
- Replace the `__OnArgument("--dvr-osd")` block with an accept-and-ignore:
  ```cpp
  __OnArgument("--dvr-osd") {
      spdlog::warn("--dvr-osd is deprecated and ignored (re-encode always burns in the OSD)");
      continue;
  }
  ```
- Delete `dvr_reenc_set_osd` and `dvr_reenc_get_osd` from the `extern "C"` block.
- Remove the `--dvr-osd` line from the help text.

- [ ] **Step 2: Update osd.cpp gating**

- Delete `extern bool dvr_osd;`.
- At both `set_osd_blend` call sites (in `my_flush_cb` and the Cairo refresh path), change the guard from `if (dvr_osd && frame_proc)` to `if (frame_proc)`.

- [ ] **Step 3: Cross-build**

Run the cross-build command. Expected: `PPBUILD_DONE rc=0`.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp src/osd.cpp
git commit -m "feat(dvr): always burn in OSD for re-encode (drop --dvr-osd)"
```

---

### Task 5: Remove the obsolete gsmenu rows

**Files:**
- Modify: `src/gsmenu/settings_fpvd.c` (drop the `dvr_osd` and any reenc codec/fps/resolution entries)
- Modify: `src/gsmenu/pages/pixelpilot.c` (drop the corresponding rows / `dvr_osd` reference)
- Modify: `src/gsmenu/settings_dummy.c` (drop the `dvr_osd` default entry)

**Interfaces:**
- Consumes: the removal of `dvr_reenc_set_*`/`get_*` C functions from Tasks 3–4 (the gsmenu rows that called them must go).

- [ ] **Step 1: Find every gsmenu reference**

Run: `grep -rnE "dvr_osd|dvr_reenc_(set|get)_(codec|fps|resolution|osd)|pixelpilot.dvr.osd" src/gsmenu`
This lists the rows/entries to remove.

- [ ] **Step 2: Remove the rows/entries**

Delete: the `dvr_osd` row in `src/gsmenu/pages/pixelpilot.c` (the block referencing `"gs","dvr","dvr_osd"`), the `{ "gs","dvr","dvr_osd", ... }` line in `src/gsmenu/settings_fpvd.c`, the `{ "dvr_osd", "on" }` line in `src/gsmenu/settings_dummy.c`, and any equivalent reenc codec/fps/resolution rows/entries found in Step 1. Keep the bitrate row. Keep `dvr_mode`.

- [ ] **Step 3: Build host gsmenu tests (compile gate for the gsmenu C)**

Run: `nix-shell shell-sim.nix --run "cmake --build build-test --target settings_fpvd_tests -j4" 2>&1 | tail -5`
Expected: builds clean (no reference to removed symbols/keys).
Then run the cross-build command (full device link). Expected: `PPBUILD_DONE rc=0`.

- [ ] **Step 4: Commit**

```bash
git add src/gsmenu/
git commit -m "feat(gsmenu): drop re-encode codec/fps/resolution/osd rows"
```

---

### Task 6: On-device verification

**Files:** none (verification only).

This is the behavioral test for the device-only changes in Tasks 2–5.

- [ ] **Step 1: Deploy the built binary to the GS**

```bash
BIN=/home/gilankpam/Projects/drone/sbc-groundstations-gilankpam/output/radxa_zero3_defconfig/build/pixelpilot-custom/pixelpilot
M=$(md5sum "$BIN"|cut -d' ' -f1)
scp -o ConnectTimeout=10 -o ServerAliveInterval=3 "$BIN" root@10.18.0.1:/tmp/pixelpilot.new
ssh -o ConnectTimeout=10 -o ServerAliveInterval=3 root@10.18.0.1 "sh -s $M" <<'EOS'
[ "$(md5sum /tmp/pixelpilot.new|cut -d' ' -f1)" = "$1" ] || { echo MISMATCH; exit 1; }
cp /tmp/pixelpilot.new /usr/bin/pixelpilot.new && mv -f /usr/bin/pixelpilot.new /usr/bin/pixelpilot
kill "$(pidof pixelpilot)"; sleep 5
echo "pid=$(pidof pixelpilot) md5=$(md5sum /proc/$(pidof pixelpilot)/exe|cut -d' ' -f1)"
EOS
```
Expected: running-exe md5 matches; the deprecation warnings for the still-present `--dvr-reenc-codec/fps/resolution`/`--dvr-osd` launch args appear in `/var/log/pixelpilot.log` and pixelpilot still starts.

- [ ] **Step 2: Record ~20 s and capture the new file**

```bash
ssh -o ConnectTimeout=10 -o ServerAliveInterval=3 root@10.18.0.1 'sh -s' <<'EOS'
sleep 2; P=$(pidof pixelpilot); T0=$(cut -d. -f1 /proc/uptime)
kill -USR1 "$P"; sleep 20; kill -USR1 "$P"; sleep 1
T1=$(cut -d. -f1 /proc/uptime); echo "elapsed=$((T1-T0))s alive=$(pidof pixelpilot)"
ls /media/dvr/*.mp4 | sort | tail -1
EOS
```
Expected: process stays alive (no crash); a new highest-sequence file appears.

- [ ] **Step 3: Pull and verify fps, duration, OSD**

```bash
cd /tmp && scp -o ConnectTimeout=10 -o ServerAliveInterval=3 root@10.18.0.1:/media/dvr/<newfile>.mp4 /tmp/v.mp4
DUR=$(ffprobe -v error -show_entries format=duration -of default=nk=1:nw=1 /tmp/v.mp4)
NF=$(ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of default=nk=1:nw=1 /tmp/v.mp4)
python3 -c "print(f'{$NF} frames / {$DUR}s = {$NF/$DUR:.1f} fps')"
ffmpeg -y -loglevel error -ss 5 -i /tmp/v.mp4 -frames:v 120 -vf "crop=1240:64:680:1016,format=rgb24,geq=r='if(gt(2*g(X,Y)-r(X,Y)-b(X,Y),60),255,0)':g='if(gt(2*g(X,Y)-r(X,Y)-b(X,Y),60),255,0)':b='if(gt(2*g(X,Y)-r(X,Y)-b(X,Y),60),255,0)',signalstats,metadata=print:file=/tmp/g.txt" -f null -
grep YAVG /tmp/g.txt | awk -F= '$2<25{c++} END{print (c+0)" OSD-drop frames of 120"}'
```
Expected: **fps ≈ min(input, 60)** (≈59 at 60 fps input), **duration ≈ elapsed** (PTS intact), **0 OSD-drop frames**. Optionally `Read` an extracted frame to confirm the OSD looks correct.

- [ ] **Step 4: Confirm resolution = screen mode**

```bash
ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 /tmp/v.mp4
```
Expected: `1920,1080` (the `--screen-mode`).

- [ ] **Step 5: Final commit / PR readiness**

No code change here; if all checks pass, the branch is ready to push and open a PR (see Execution Handoff).

---

## Self-Review

**Spec coverage:** codec=h265 → Task 3 Step 1; resolution=screen → Task 2 Steps 3–4; fps cap → Tasks 1 + 2; bitrate kept → unchanged (verified by omission, Task 3 keeps `--dvr-reenc-bitrate`); OSD always-on → Task 4; accept-and-ignore flags → Tasks 3–4; gsmenu cleanup → Task 5; pacer collapse → Task 2; raw path unchanged → no task touches it; verification → Task 6. All covered.

**Placeholder scan:** The "KEEP the existing … block verbatim" notes in Task 2 Step 3 reference concrete existing code (the resize and OSD-blend blocks already in `process_loop`) rather than unwritten work — they're move-don't-rewrite instructions, not TODOs. No "TBD"/"add error handling"/"write tests for the above".

**Type consistency:** `FpsCap(int)` / `should_emit(int64_t)` consistent between Task 1 (definition) and Task 2 (use). New constructor `FrameProcessor(MppEncoder*, int cap_fps, uint32_t enc_w, uint32_t enc_h)` consistent between Task 2 Step 1 (header) and Step 4 (call sites). `reenc_target_dims(uint32_t&, uint32_t&)` consistent.
