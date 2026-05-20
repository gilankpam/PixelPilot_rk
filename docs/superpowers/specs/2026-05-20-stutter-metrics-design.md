# VideoStutterWidget — GS-local video stutter metrics

**Date:** 2026-05-20
**Status:** Approved for implementation

## Motivation

PixelPilot_rk already surfaces video FPS, bitrate, and decode latency in the OSD, but none of those numbers correlate well with the subjective "video is stuttering" feeling pilots experience in flight. The pilot needs an at-a-glance metric that:

1. Confirms a perceived stutter actually happened (objective evidence for a subjective feel).
2. Quantifies how bad it was, so the pilot can compare across flights / setups.
3. Operates entirely on the ground station clock — the GS clock is not synchronized with the drone, so any cross-clock metric is unusable.

Frame drops (visible glitches) are out of scope: they are already self-evident on screen and the pilot understands their cause. The widget targets the more ambiguous case where the video feels janky without an obvious glitch.

## Display format

A single OSD line:

```
STUT  18 ms · 3/s  ▲47
```

| Field | Meaning | Window |
|---|---|---|
| `18 ms` | Mean inter-frame interval at display | rolling 1s |
| `3/s` | Stutter event rate (frames whose interval exceeded 1.5× rolling median) | rolling 1s |
| `▲47` | Largest stutter-classified interval observed; reads `▲0` when no recent spike | decaying 10s (entry resets to 0 ten seconds after the spike) |

The peak indicator stays on screen long enough that the pilot can glance at it 5–10s after feeling a stutter and still read the magnitude, then resets to 0 so old hiccups do not haunt the readout. `▲0` is the resting state. Keeping the field always visible (rather than hiding it) avoids the OSD layout shifting when a spike comes and goes.

## Measurement point

The interval is sampled in `__DISPLAY_THREAD__` (`src/main.cpp` around line 452), immediately after `drmModeAtomicCommit` returns successfully. Interval = `now_monotonic - previous_commit_monotonic`. This timestamp represents the moment a frame actually becomes visible (modulo VSYNC), which is what the pilot's eye perceives.

`CLOCK_MONOTONIC` via `clock_gettime` is already in scope in this thread.

The first commit after startup or after a resolution-change reinit publishes no interval (no previous timestamp to subtract).

## Fact published

A new fact is published once per successful DRM commit, in the same block as the existing `video.displayed_frame` / `video.decode_and_handover_ms` publishes:

```cpp
osd_publish_uint_fact("video.frame_interval_ms", NULL, 0, interval_ms);
```

The widget is the only consumer. The fact is published unconditionally; widgets that do not subscribe pay only the cost of the fact-publish call, which is the same as the existing video facts.

## Stutter classification

A frame counts as a stutter event when:

```
interval_ms > 1.5 * median(recent_intervals)
```

`recent_intervals` is a fixed-size ring buffer of the last **120 intervals**, which represents ~2s at 60fps or ~4s at 30fps — enough samples for a stable median, short enough to adapt when stream FPS changes.

The 1.5× threshold is a code constant. It is intentionally not user-configurable (YAGNI); if it proves wrong in practice we tune the constant.

Median is computed by copying the ring into a `std::array<long, 120>` and using `std::nth_element` — O(n) and runs at most ~60 times/sec, negligible cost.

Before the ring has ≥ 30 samples, no frame is classified as a stutter (warm-up — avoids spurious classifications during startup).

## Widget internals

New class `VideoStutterWidget` in `src/osd.cpp`, placed immediately after `VideoDecodeLatencyWidget` (~line 1294). It extends `IconTplTextWidget` with `num_args = 3`.

State:

```cpp
RunningAverage avg_interval;       // (1000ms, 100ms) — gives `18 ms`
RunningAverage stutter_events;     // (1000ms, 100ms) — gives `3/s` via rate_per_second_over_last_ms(1000)
std::deque<long> recent;           // ring of last 120 intervals, for median
long  peak_ms = 0;                 // 0 == no peak to display
uint64_t peak_ts_ms = 0;           // monotonic ms when peak_ms was last set
```

`setFact(0, fact)` flow on each incoming `video.frame_interval_ms`:

1. Push interval into `recent`; pop front if size > 120.
2. `avg_interval.add(interval)`.
3. If `recent.size() >= 30`, compute median; if `interval > 1.5 * median`, `stutter_events.add(1)`.
4. Update peak in two steps:
   - **Expire first:** if `peak_ms != 0 && now - peak_ts_ms > 10_000`, reset `peak_ms = 0; peak_ts_ms = 0`.
   - **Then promote:** if `interval > peak_ms` *and* the frame is classified as a stutter (step 3), set `peak_ms = interval; peak_ts_ms = now`. Restricting peak updates to stutter-classified frames ensures the ▲ readout only shows genuine spikes, not the natural maximum of normal jitter.
5. Populate the three template args from `avg_interval.average_over_last_ms(1000)`, `stutter_events.rate_per_second_over_last_ms(1000)`, and `peak_ms`. `peak_ms == 0` renders as `▲0` (the resting state).

The widget reuses `RunningAverage` exactly as the existing video widgets do — no new sliding-window infrastructure.

## Wiring

Three small edits, all additive:

1. **`src/main.cpp`** — in `__DISPLAY_THREAD__`, just after the existing `video.displayed_frame` publish (line ~455): track `last_commit_ts_monotonic_ms`, compute and publish `video.frame_interval_ms`. Skip the publish on the first iteration / after `last_commit_ts == 0`.
2. **`src/osd.cpp`** — add the `VideoStutterWidget` class.
3. **`src/osd.cpp`** — add a parser branch in the widget loader (~line 1722) for `"VideoStutterWidget"`, mirroring the existing `"VideoDecodeLatencyWidget"` branch exactly.

Opt-in via OSD config: pilots who do not add the widget to their config pay only the cost of one extra fact publish per frame (a few dozen ns).

## Testing

**Unit-testable surface:** the classifier is a pure function and can be extracted as:

```cpp
bool is_stutter(long interval_ms, const std::deque<long>& recent, double factor = 1.5);
```

Tested independently of widget rendering:
- Empty ring → returns false (warm-up).
- Ring of 60 samples all = 16ms, new interval = 16ms → false.
- Ring of 60 samples all = 16ms, new interval = 25ms → true (25 > 24).
- Ring of 60 samples all = 16ms, new interval = 23ms → false (23 < 24).

The peak-expiry logic is similarly testable as a pure function.

**Manual verification:** synthetic stutter stream via gst-launch — `videotestsrc ! identity sleep-time=...` or a script that drops occasional UDP packets — confirms non-zero `3/s` and a visible `▲` value that decays after 10s.

## Out of scope

Deliberately excluded to keep this change tight:

- **GStreamer `rtpjitterbuffer` stats integration** — would give true RF-link drop counts; deferred.
- **Drop counter in this widget** — drops are visible as on-screen glitches; pilot does not need a number.
- **Color/alert thresholds** — no existing widget uses color cues; would require widening the widget infrastructure.
- **CLI flag** — purely OSD-config opt-in.
- **End-to-end "frame age"** (RTP ingress → DRM commit) — possible follow-up but adds plumbing through GStreamer→MPP PTS.

## Cross-clock constraint reminder

All timestamps used by this widget come from `CLOCK_MONOTONIC` on the ground station. No timestamp originating from the drone (RTP timestamp, MPP PTS forwarded from the drone) is compared against GS time. This is a hard constraint: GS and drone clocks are unsynchronized.
