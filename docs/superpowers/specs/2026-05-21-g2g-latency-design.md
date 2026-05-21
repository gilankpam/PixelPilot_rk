# Glass-to-Glass Latency Telemetry (GS side)

**Status:** design approved, pending implementation plan
**Date:** 2026-05-21
**Branch target:** new feature branch off `master`

## Goal

Surface variable glass-to-glass latency on the PixelPilot ground station OSD by
consuming the `waybeam_venc` RTP sidecar (drone-side per-frame metadata channel)
plus three new GS-local timestamps inside the existing video pipeline. Publish a
per-segment breakdown so the OSD (and downstream charting) can attribute
latency to capture/encode/wire/decode/display.

Fixed offsets (camera sensor exposure+readout, display panel response) are
out of scope; they are constants per hardware and can be added as an
`osd.json` constant in a follow-up if desired.

## Non-goals

- Drone-side changes. The sidecar protocol is already implemented in
  `waybeam_venc/include/rtp_sidecar.h` and `waybeam_venc/src/rtp_sidecar.c`.
  This work is GS-side only.
- True absolute glass-to-glass (would require modelling sensor + panel).
- Multi-stream / multi-SSRC support. PixelPilot is single-stream today.
- Reactive use of the latency signal (e.g. adaptive bitrate). Telemetry only.

## Architecture

```
                              ┌──────────────────────┐
                              │  latency_probe       │
                              │  thread              │
   waybeam venc  ◄─── UDP ───►│  ─ subscribe (2s)    │
   (drone, 5602)              │  ─ sync (1s→5s)      │
                              │  ─ recv MSG_FRAME    │
                              │  ─ clock offset est. │
                              │  ─ frame matcher     │──► osd_publish_uint_fact("video.latency.*")
                              └──────────┬───────────┘
                                         │ joins by (ssrc, rtp_ts)
   gstrtpreceiver  ──► pad probe ───►  FrameTimings ring
   (marker bit @                       (deque<=64, TTL=500ms,
   last RTP pkt)                        std::mutex)
                                         ▲
   mpp decoder hook  ──── pop oldest awaiting-decode, stamp decode_done_us
   display thread hook  ── pop oldest awaiting-display, stamp display_submit_us
```

Four moving parts; all new code lives in `src/latency_probe.{hpp,cpp}`. The
existing pipeline gets three small hooks (pad probe extension, post-decode
call, post-commit call) and one config branch.

### Why FIFO matching (and not GstMeta side-channels)

By the time a buffer reaches the decoder or the display thread, the RTP
timestamp is no longer present — we only have a raw H.264/265 access unit
and, later, a decoded MppFrame. We do **not** carry `rtp_timestamp` through
the pipeline. Instead, the matcher relies on PixelPilot's single-stream
in-order property: the N-th frame to cross the marker-bit boundary is the
N-th frame the decoder produces and the N-th frame the display submits.

The ring is a `std::deque<FrameTimings>` (cap 64). On each downstream hook,
we pop the oldest slot whose corresponding timestamp is still zero. The TTL
(500 ms) catches the rare case where a decoder or display drop desyncs the
deque — orphans age out and the deque self-resyncs on the next clean frame.

This costs ~150 lines of new code, no GstMeta touches, no decoder API
changes. The TTL/cap gives bounded recovery on the rare desync.

## Components & files

| File | New/Modified | Responsibility |
|---|---|---|
| `src/latency_probe.hpp` | new | Public API; singleton accessor. |
| `src/latency_probe.cpp` | new | Thread, UDP socket, sync estimator, frame matcher, OSD publishing. |
| `src/gstrtpreceiver.cpp` | modified | In `udp_last_hop_probe` (line 785), parse RTP header (12B fixed) and on marker=1 call `latency_probe::record_arrival(...)`. |
| `src/main.cpp` | modified | Init/teardown probe; call `record_decode_done()` right after the successful `decode_get_frame` (around line 317); call `record_display_submit()` right after `drmModeAtomicCommit` (around line 453). |
| `osd.json` schema | modified | New section `latency_probe: { enable: false, host: <drone IP>, port: 5602 }`. |
| `tests/test_latency_probe.cpp` | new | Unit tests for clock-offset estimator, RTP header parser, frame matcher. |
| `tests/test_latency_probe_integration.cpp` | new | "Fake waybeam" loopback: emits canned MSG_FRAME + RTP, asserts published facts. |

### Public API (`latency_probe.hpp`)

```cpp
namespace latency_probe {

// Lifecycle. Safe to call when disabled — both no-op.
bool start(const std::string& host, uint16_t port);
void stop();

// Cheap-to-check gate for hot paths.
extern std::atomic<bool> active;

// Hot-path hooks. All no-ops when active==false.
void record_arrival(uint32_t ssrc, uint32_t rtp_ts,
                    uint64_t gs_recv_us, bool marker);
void record_decode_done(uint64_t gs_decode_us);
void record_display_submit(uint64_t gs_display_us);

} // namespace latency_probe
```

The `active` atomic lets the three hot-path hooks return in ~one load when
the feature is off; we don't want any cost on the default-disabled path.

## Data flow (per frame)

1. **Drone:** sensor → encoder → packetiser → kernel sendmsg. Sidecar emits
   `MSG_FRAME` immediately after the final `sendmsg`, carrying `capture_us`,
   `frame_ready_us`, `last_pkt_send_us`, plus `(ssrc, rtp_timestamp)`.
2. **GS pad probe:** every RTP packet hits `udp_last_hop_probe`. Parse
   header bytes 0–11; if marker bit (header[1] & 0x80) is set, look up
   the ring by `(ssrc, rtp_ts)`. If a slot already exists (sidecar arrived
   first), stamp `gs_recv_last_us=now`. Otherwise push a new
   `FrameTimings{ssrc, rtp_ts, gs_recv_last_us=now}` slot. (Both `MSG_FRAME`
   and the marker-bit packet can race; we handle either ordering.)
3. **GS decoder hook:** after `decode_get_frame` succeeds, pop the oldest
   ring slot whose `gs_decode_done_us == 0` and stamp it with `now`.
4. **GS display hook:** after `drmModeAtomicCommit`, pop the oldest slot
   whose `gs_display_submit_us == 0` and stamp it.
5. **Probe thread:** when `MSG_FRAME` arrives, look up the slot by
   `(ssrc, rtp_ts)`. If found, copy sender-side fields and mark
   `sidecar_seen=true`. If not found, create a new slot with sender-side
   fields populated and `gs_recv_last_us=0` (wire half may arrive next).
   Then check publish condition: all five timestamps non-zero AND
   `sidecar_seen` true → compute segments, publish, remove. Independently,
   the probe thread runs a periodic TTL sweep (every 100 ms) to evict
   slots older than 500 ms.

`FrameTimings` ring entry:

```cpp
struct FrameTimings {
    uint32_t ssrc;
    uint32_t rtp_ts;
    uint64_t gs_recv_last_us;     // stamped by pad probe on marker=1
    uint64_t gs_decode_done_us;   // 0 until popped by decode hook
    uint64_t gs_display_submit_us;// 0 until popped by display hook
    uint64_t capture_us;          // from MSG_FRAME (drone clock)
    uint64_t frame_ready_us;      // from MSG_FRAME (drone clock)
    uint64_t last_pkt_send_us;    // from MSG_FRAME (drone clock)
    bool     sidecar_seen;
    uint64_t inserted_us;         // for TTL
};
```

One `std::mutex` protects the ring. Writers: pad probe, decode hook, display
hook, probe thread (on MSG_FRAME) and probe thread (on TTL sweep). All
critical sections are an O(64) deque scan, holding the lock for single-digit
microseconds — well below any frame-rate budget.

## Clock-offset estimator

```cpp
struct SyncSample { uint64_t offset_us; uint64_t rtt_us; uint64_t taken_us; };

class ClockOffset {
    std::array<SyncSample, 16> samples_{};
    size_t count_ = 0;
    size_t next_  = 0;        // ring write index
    int64_t  best_offset_us_ = 0;
    uint64_t best_rtt_us_    = UINT64_MAX;
    mutable std::mutex m_;
public:
    void add_sample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4);
    void get(int64_t& offset_us, uint64_t& rtt_us) const;
};
```

- `t1` = probe send time (GS clock)
- `t2` = drone recv time (drone clock)
- `t3` = drone reply send time (drone clock)
- `t4` = probe recv time (GS clock)
- `rtt_us = (t4 - t1) - (t3 - t2)`
- `offset_us = ((t2 - t1) + (t3 - t4)) / 2`  *(GS_clock + offset ≈ drone_clock)*

On every `add_sample`: store in ring; if new `rtt_us < best_rtt_us_`, or if
the current best's slot has been overwritten, rescan all 16 to pick the new
min-RTT sample. Min-RTT pick (NTP-style) because lower RTT means less
queueing asymmetry and a more accurate offset estimate.

`wire_ms` computation:

```
adjusted_send_us = last_pkt_send_us - offset_us   // → GS clock domain
wire_us = gs_recv_last_us - adjusted_send_us
wire_us = max(wire_us, 0)                         // clamp; defends against
                                                  // momentary offset drift
```

Sync schedule (probe thread):
- `MSG_SYNC_REQ` every 1 s for the first 10 s, then every 5 s.
- `MSG_SUBSCRIBE` every 2 s as keepalive (sidecar TTL is 5 s).
- Both go through the same UDP socket; responses are demuxed by `msg_type`.

## Published OSD facts

All `osd_publish_uint_fact` (or `_int_fact` for signed) unless noted. Units
are milliseconds for segments, microseconds for diagnostics.

| Fact name | Type | Source |
|---|---|---|
| `video.latency.capture_to_encode_ms` | uint | `(frame_ready_us - capture_us) / 1000`, drone-local. Skipped if `capture_us == 0`. |
| `video.latency.encode_to_send_ms`    | uint | `(last_pkt_send_us - frame_ready_us) / 1000`, drone-local. |
| `video.latency.wire_ms`              | uint | `(gs_recv_last_us - (last_pkt_send_us - offset_us)) / 1000`, clamped ≥0. |
| `video.latency.decode_ms`            | uint | `(gs_decode_done_us - gs_recv_last_us) / 1000`. |
| `video.latency.display_ms`           | uint | `(gs_display_submit_us - gs_decode_done_us) / 1000`. |
| `video.latency.total_ms`             | uint | sum of the five above. |
| `video.latency.clock_offset_us`      | int  | from `ClockOffset::get()`. |
| `video.latency.clock_rtt_us`         | uint | from `ClockOffset::get()`. |
| `video.latency.wire_clamp_count`     | uint | cumulative count of `max(wire_us,0)` clamps. |

Existing `video.decode_and_handover_ms`, `video.decoder_feed_time_ms`,
`video.frame_interval_ms` are untouched — current OSD configs continue to
work.

## Configuration

New section in `osd.json` (the existing top-level config file):

```json
{
  "latency_probe": {
    "enable": false,
    "host":   "10.5.0.10",
    "port":   5602
  }
}
```

Defaults: `enable=false`. When `enable=true` and `host` is reachable, the
probe thread starts; otherwise the section is silently ignored. Missing
section → disabled.

## Error handling

| Condition | Behavior |
|---|---|
| `enable=false` / section missing | `start()` never called. Hot-path hooks short-circuit on `active.load()==false`. |
| Host unresolvable / port closed | Non-fatal. Log once. Probe thread keeps retrying SUBSCRIBE indefinitely. |
| No `MSG_FRAME` after 10 s subscribed | Warning log every 30 s. No facts published. Sync still runs; `clock_offset_us`/`clock_rtt_us` still publish (useful on their own). |
| `MSG_FRAME` arrives, no matching ring slot | Create new slot with sender-side fields populated, `gs_recv_last_us=0`. TTL evicts if wire half never shows. |
| Marker-bit packet arrives, no later `MSG_FRAME` | Slot lives until TTL eviction. |
| Decoder skips a frame (NAL discard / error) | Oldest awaiting-decode slot ages out via TTL. Deque self-resyncs within 500 ms. |
| Display drops a frame | Same — TTL eviction. |
| `wire_us` computes negative | Clamp to 0, increment `video.latency.wire_clamp_count`. Indicates stale offset; self-corrects on next sync. |
| Protocol version mismatch (`buf[4] != 1`) | Drop, log once. |
| Sender wraps `rtp_timestamp` (uint32, every ~13 h at 90 kHz) | Matcher keys on full `(ssrc, rtp_ts)`; wrap is invisible — we look up by exact value, never compare deltas across wrap. |

`wb_monotonic_us` on the drone uses `CLOCK_MONOTONIC_RAW`; GS uses
`CLOCK_MONOTONIC`. They tick at the same rate (the difference is NTP
slewing, which `RAW` skips). Offset estimation absorbs any constant bias.
Not a problem in practice — noted for future readers.

## Testing

### Unit tests (`tests/test_latency_probe.cpp`, GoogleTest style)

1. `ClockOffset_PicksMinRtt` — feed 5 samples with varied RTT, assert
   `best_offset_us_` is from the lowest-RTT sample.
2. `ClockOffset_RingEvictionReselects` — fill 16, evict the best, assert
   the next-best is picked on the rescan.
3. `RtpHeader_ParsesSsrcTimestampMarker` — synthetic 12-byte headers,
   including marker=0/1; version!=2 must be rejected.
4. `Matcher_JoinsOnSidecarAfterArrival` — push arrival, then sidecar
   message, then decode+display stamps; assert publish.
5. `Matcher_JoinsOnArrivalAfterSidecar` — reverse order.
6. `Matcher_TtlEvictsOrphans` — half-populated slot ages out at 500 ms.
7. `Matcher_FifoDecodeDisplayPop` — three frames in, decode/display stamps
   bind to oldest pending in order.
8. `Matcher_DecodeSkipRecovers` — frame 2's decode never stamped; frame 3
   still publishes correctly after TTL purge.
9. `Matcher_RingCapBounded` — push 200 slots, assert size stays ≤64 and
   oldest are dropped.
10. `WireClampsNegative` — synthesise an apparent-negative wire delta,
    assert clamp + counter increment.

### Integration test (`tests/test_latency_probe_integration.cpp`)

Spawn a thread acting as the "fake waybeam":
- listens for `MSG_SUBSCRIBE` on a chosen port
- answers `MSG_SYNC_REQ` with a known synthetic offset
- emits `MSG_FRAME` for 30 synthetic frames at 60 Hz

Probe runs in another thread, fed synthetic `record_arrival/decode/display`
calls in lockstep. OSD test-double captures `osd_publish_uint_fact` calls
into a vector. Assertions: each of the eight `video.latency.*` facts is
published the expected number of times with values within tolerance of
the synthetic ground truth.

### Manual smoke test

Run against real drone. Sanity-check `video.latency.wire_ms` ≈ ping RTT/2.
Confirm `video.latency.total_ms` falls in the 60–200 ms range expected for
typical FPV configurations.

## Out of scope (follow-ups, not this spec)

- Fixed-offset `latency.sensor_panel_offset_ms` config knob for true G2G.
- OSD widget updates to chart latency segments (the data will flow once
  this lands; widget config is separate).
- Reactive use of latency for bitrate/keyframe control.
- Memory update: the existing project memory ("never compare timestamps
  across drone and GS") needs to be amended to reflect that the sidecar
  sync handshake makes bounded cross-comparison possible.

## TODO: drone-side MSG_FRAME rate-limit config (waybeam patch)

**Why:** waybeam currently emits one `MSG_FRAME` per encoded frame
(`star6e_video.c:156`, `maruko_pipeline.c:3215`), unconditionally while
subscribed. At 60 fps with the 80-byte frame trailer this is ~52 kbps
sustained across the wfb tunnel. Comfortable on a healthy link, but on
long-range / low-MCS conditions the tunnel shares budget with MAVLink and
this could starve control traffic. There's no current knob to throttle
it; the prior `outgoing.{backpressure,highWaterPct,lowWaterPct}` config
was deliberately rolled back in v0.9.2 (see `venc_ring.h:149-158`).

**What:** add `outgoing.sidecarFrameInterval` (uint, default 1 = every
frame, FT_UINT16, MUT_RESTART) to the `outgoing` config block. Wire it
into the per-frame call sites:

```c
// star6e_video.c near line 156
// maruko_pipeline.c near line 3215
if (cfg->outgoing.sidecar_frame_interval > 0 &&
    (state->sidecar.frame_id % cfg->outgoing.sidecar_frame_interval) == 0) {
    rtp_sidecar_send_frame_transport(&state->sidecar, ...);
}
```

(Use a per-stream counter; the existing `RtpSidecarSender::frame_id` is
the monotonic counter and is incremented inside `rtp_sidecar_send_frame_transport`
on actual sends, so a separate "candidate" counter is needed.)

**GS side:** no code change required. The matcher already gracefully
handles missing MSG_FRAMEs (orphan marker arrivals age out via TTL). The
OSD widgets read rolling values, so 12 Hz updates render fine.

**Sizing guidance:**
- `1` (default) — 60/sec, ~52 kbps, full per-frame fidelity.
- `5` — 12/sec, ~10 kbps. Good default for long-range FPV.
- `30` — 2/sec, ~2 kbps. Minimum useful: still gives wire_ms / clock-offset
  samples but `total_ms` updates feel laggy.

**Out of scope:** dynamic adaptation (link_controller adjusting interval
based on RSSI/load). The static knob is enough for v1.
