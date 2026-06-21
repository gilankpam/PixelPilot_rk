# AIO Widget: replace LATENCY tile with RTP jitter

**Date:** 2026-06-21
**Status:** Design — approved for spec review

## Goal

Replace the `LATENCY` metric tile on the AIO Widget strip with a `JITTER`
tile showing **RFC 3550 RTP interarrival jitter** in milliseconds — the
streaming-standard measure of link timing variation.

The AIO Widget redraws at 1 Hz (cairo). The jitter value is a continuously
smoothed scalar updated per received frame; the tile just renders the latest
value at draw time.

## Why RTP interarrival jitter (not frame-interval, not display dispersion)

Three candidates were considered and rejected in favour of RFC 3550:

- **Raw frame-interval max** (`video.frame_interval_ms`): an *absolute*
  inter-frame interval, not a variation. Tracks frame rate, not jitter, and is
  blind to sustained "smooth-but-laggy" link degradation.
- **Display interval dispersion** (stddev/MAD of display intervals):
  conflates link jitter with local decode/pacing and DRM/vsync commit cadence;
  noisier and a lagging signal.
- **RFC 3550 RTP interarrival jitter** (chosen): measures over-the-air timing
  variation directly, at the network layer.

RFC 3550 is the best fit for this FPV pipeline specifically because:

1. **No jitter buffer.** The custom RTP receiver has none
   (`main.cpp:905`), so nothing absorbs network jitter before display — RTP
   jitter maps almost directly to what the pilot sees, while still being the
   network-standard metric.
2. **Leading, actionable signal.** Rises as the link degrades, often before a
   visible freeze — earlier warning the pilot can act on (antenna, position,
   channel).
3. **Isolates the link.** Excludes local decode/pacing noise.
4. **Never blank.** The formula is a difference-of-differences, so the unknown
   GS↔drone clock offset cancels — no clock-sync dependency (unlike the
   `video.latency.total_ms` tile it replaces, which is suppressed until sync
   converges). See [[project_clock_constraints]].

## The computation (RFC 3550 §6.4.1)

Per consecutive frame-marker arrival `i`:

```
S_i  = rtp_ts_i  converted to microseconds   (sender RTP timestamp, 90 kHz)
R_i  = gs_recv_us_i                            (GS monotonic arrival time)
D    = (R_i − R_{i−1}) − (S_i − S_{i−1})
J   += (|D| − J) / 16                          // smoothed running estimate
```

`J` is reported in ms (`J / 1000`, rounded). It rises when frames arrive with
spacing that differs from how they were timestamped (link variation) and
settles toward 0 on an even link.

Numeric details:
- **RTP clock = 90 kHz** (the H.264/H.265 RTP video convention). One tick =
  `1e6 / 90000` µs. `S` conversion: `S_us = rtp_ts * 1e6 / 90000`.
- **Wrap-around**: `rtp_ts` is uint32 and wraps. Compute the timestamp delta as
  a signed 32-bit difference (`(int32_t)(rtp_ts_i − rtp_ts_{i-1})`) before
  converting — same trick `track_rtp_sequence` uses for seq numbers.
- **First sample / SSRC change**: no previous reference → emit nothing (or
  reset `J = 0` and skip). New SSRC resets all state.

## Components

### 1. `RtpJitterEstimator` — new pure logic unit

New header (e.g. `src/rtp_jitter.hpp`), pure and unit-testable, mirroring the
existing pure-logic style of `video_stutter.hpp` / `FrameMatcher` /
`ClockOffset`.

```cpp
class RtpJitterEstimator {
public:
    // Feed one frame-marker arrival. Returns current jitter estimate in ms.
    // Resets internal state on SSRC change. First sample after reset returns 0.
    double update(uint32_t ssrc, uint32_t rtp_ts, uint64_t gs_recv_us);
    double jitter_ms() const;   // latest estimate
private:
    bool     have_prev_ = false;
    uint32_t ssrc_      = 0;
    uint32_t prev_rtp_ts_   = 0;
    uint64_t prev_recv_us_  = 0;
    double   j_us_      = 0.0;  // RFC3550 J, in microseconds
    static constexpr double RTP_HZ = 90000.0;
};
```

Keeping `J` in µs internally avoids precision loss; convert to ms only at the
boundary.

### 2. Wire it into `latency_probe::on_rtp_buffer`

`on_rtp_buffer` (`latency_probe.cpp:230`) already runs per RTP packet, gates on
`marker=1` (one per frame), and holds `h.timestamp` + `gs_recv_us`. Add, right
after the existing `on_marker_arrival` call:

```cpp
double jms = s->jitter.update(h.ssrc, h.timestamp, gs_recv_us);
s->pub_u("video.rtp_jitter_ms", (uint64_t)llround(jms));
```

- `RtpJitterEstimator jitter;` becomes a member of `ProbeState`.
- Publishes via the same `pub_u` indirection the probe already uses, so the
  test publish-override path (`set_publish_overrides_for_test`) captures it.
- Runs on the receiver thread, same as the existing marker handling.

**New fact: `video.rtp_jitter_ms`** (uint, ms). Named to distinguish it from
the unrelated `--rtp-jitter-ms` *jitter-buffer* CLI knob (now ignored).

### 3. AIO Widget — swap the slot

In `src/osd.cpp`:

- Rename `SLOT_LATENCY` → `SLOT_JITTER` (the enum comment updated to
  `video.rtp_jitter_ms`).
- Default tagless matcher list (`osd.cpp:2358`): `video.latency.total_ms` →
  `video.rtp_jitter_ms`. Order is contractual with the enum — change in
  lockstep.
- `setFact`: `SLOT_JITTER` just stores the latest value (the estimate is
  already smoothed; no `RunningAverage` needed):
  ```cpp
  default: // SLOT_VIDEO_RES, SLOT_VIDEO_FPS, SLOT_JITTER
      args[idx] = fact;
  ```
  (No new case required — it falls through the existing `default`.)
- `draw_strip` (`osd.cpp:1692`): replace the LATENCY tile:
  ```cpp
  long jit = (long)arg_u(SLOT_JITTER);
  right.push_back(metric_tile("JITTER", std::to_string(jit), "ms",
                              aio::resolve_band(aio::Metric::Jitter, (double)jit), "888"));
  ```

### 4. New band — `aio::Metric::Jitter`

Add to the enum (`osd_aio_logic.hpp:10`) and `resolve_band`
(`osd_aio_logic.cpp`). Lower-is-better. **Proposed thresholds** (tunable):

| Band | RTP jitter | Meaning |
|------|-----------|---------|
| Good | ≤ 10 ms | clean link |
| Warn | ≤ 25 ms | noticeable link variation |
| Crit | > 25 ms | poor link, hitches likely |

## Data flow

```
RTP marker packet ──(pad probe)──► latency_probe::on_rtp_buffer
                                      │  RtpJitterEstimator::update(ssrc, rtp_ts, gs_recv_us)
                                      ▼
                            pub_u("video.rtp_jitter_ms", J)
                                      │  (OSD publish)
                                      ▼
                         AIOWidget::setFact(SLOT_JITTER)  ── stores latest
                                      │  (1 Hz cairo redraw)
                                      ▼
                    draw_strip: JITTER tile, ms, Metric::Jitter band
```

## Testing

Host sim / Catch2 (see [[reference_host_sim_test_build]]):

- **`RtpJitterEstimator` unit tests** (new):
  - Evenly spaced arrivals (R-gap == S-gap) → `J → 0`.
  - One late arrival injected → `J` rises, then decays back as spacing
    normalizes.
  - uint32 `rtp_ts` wrap-around handled (no spurious spike).
  - SSRC change resets state.
  - First sample after reset returns 0 (no previous reference).
- **`resolve_band(Metric::Jitter, …)` boundary tests**: 10/11, 25/26 ms.
- **Publish integration**: with `set_publish_overrides_for_test`, feed marker
  buffers through `on_rtp_buffer` and assert `video.rtp_jitter_ms` is emitted.

## Non-goals / notes

- **`video.latency.*` stays computed and published** by `latency_probe`. Only
  the AIO subscription changes; other OSD configs may still reference latency.
- **`VideoStutterWidget` is untouched** — the median-based stutter rate / 10 s
  peak widget remains available; this only changes the AIO tile.
- **Probe dependency**: jitter is published only while `latency_probe` is
  active (the pad-probe hook is gated on `latency_probe::active`). Unlike
  latency it needs *no* sidecar/clock-sync, but it does require the probe to be
  started via OSD config. If the probe is disabled, the JITTER tile shows
  `--`. Decoupling jitter from the probe lifecycle is possible but out of
  scope here.
- **Custom AIO configs**: anyone with an explicit per-slot `facts` list for
  their AIOWidget must swap `video.latency.total_ms` → `video.rtp_jitter_ms`
  themselves; the default injected list is handled.
- **90 kHz assumption**: hard-coded RTP video clock rate. If a non-90 kHz
  payload is ever used the conversion constant must change; acceptable for this
  H.264/H.265 pipeline.
