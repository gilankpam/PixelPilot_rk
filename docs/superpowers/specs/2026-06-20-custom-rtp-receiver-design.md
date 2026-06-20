# Custom RTP receiver — removing GStreamer from the live video path

- **Date:** 2026-06-20
- **Status:** Design (awaiting review)
- **Branch:** `feat/custom-rtp-receiver`

## 1. Goal & motivation

Replace GStreamer on the **live RTP video path** with a custom, hand-rolled receiver
that feeds the Rockchip MPP decoder directly, to **own the receive→decode path
end-to-end and cut latency**. GStreamer stays only for DVR mp4 file playback.

The specific latency lever: today the live path runs
`udpsrc → rtph265depay → h265parse (alignment=au) → appsink`, and `alignment=au`
can hold a finished frame until the *next* frame begins, because it derives the
access-unit boundary from the bitstream (needs lookahead). A custom depayloader
emits a complete Annex-B frame the instant the **RTP marker bit** of its last
packet arrives — zero lookahead — and also removes the appsink pull-thread hop and
a buffer copy.

## 2. Scope

**In scope**
- New custom RTP receiver for the live path (H265 only), feeding MPP directly.
- Move the existing side-channels (last-hop IP, RTP-gap→IDR, stream presence /
  decode-stall, restream, latency probe, IDR feedback) off GStreamer.
- Collapse the system to **H265 everywhere**: delete the runtime MPP decoder-reinit
  machinery, codec selection, and H264 mp4 detection.
- A validation test bench (golden parity, unit, fuzz, on-target A/B).

**Out of scope**
- DVR mp4 playback stays on GStreamer (`filesrc → qtdemux → h265parse → appsink`
  + seek/rate/pause). It is offline file playback and gains nothing from removal.
- The DVR **re-encoder** (`mpp_encoder`, `--dvr-reenc-codec`). It is a separate
  *recording* subsystem (encode direction). In the production `--dvr-mode raw`
  config it is inactive and recordings are H265 — consistent with H265-only
  playback. **Consequence to accept:** existing H264 `.mp4` recordings will no
  longer play back, and switching `--dvr-mode` to re-encode (H264) would produce
  files the H265-only player cannot open.
- A jitter / reorder buffer. Production runs `--rtp-jitter-ms 0` (disabled); the
  custom path matches today's in-order passthrough behavior.

## 3. Decisions (from brainstorming)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Motivation = latency / full control | Own the receive→decode path; remove `alignment=au` holdback + thread hop + copy. |
| D2 | Live path only; **keep gst for DVR** | DVR is offline; reimplementing an mp4 demux + seek is the hardest, least-rewarding part. |
| D3 | **In-order passthrough**, no reorder buffer | Matches production (`--rtp-jitter-ms 0`); lowest latency; existing seq-gap→IDR recovery covers loss. |
| D4 | **Hard cutover**, validated by offline test bench | No runtime fallback; the bench (golden + unit + fuzz + on-target) is the safety net. |
| D5 | Approach A — hand-roll depayloader (RFC 7798) | Only option that delivers "own the path end-to-end"; fully unit-testable in isolation. |
| D6 | **H265 only** — drop H264 across the whole system | FPV link is H265; lets us delete the mid-stream MPP reinit (itself a stall risk). |

## 4. Validated assumption — RTP marker bit (load-bearing)

The entire latency premise (D1/D5) depends on the air-side encoder setting the RTP
marker bit on the last packet of every frame. This was **measured**, not assumed.

Method: stop fpvd (frees UDP 5600), bind `0.0.0.0:5600` with a Python UDP socket
while a manual `wfb_rx` fed it, capture 6 s, group packets by RTP timestamp, check
the marker bit per group.

Result (GS `10.18.0.1`, 2026-06-20):

| Metric | Value |
|---|---|
| RTP packets (6 s) | 9,475 |
| Payload type | 97 (single, consistent — HEVC) |
| Frames | 357 |
| Frames with exactly one marker, on the last packet | **357 / 357 = 100%** |
| Zero-/multi-marker frames | 0 / 0 |
| Frame rate | ~60 fps (RTP ts step 1500 @ 90 kHz) |
| Packets/frame | 17–44, median 23 |

Conclusions:
- Marker-bit emit is **validated**; the zero-lookahead latency win is achievable.
- Frames are **FU-fragmented across ~23 packets** (median) — FU reassembly
  (RFC 7798 type 49) is the hot path and the primary test-bench target.
- Caveat: this was a clean local capture (no RF loss). A degraded link can drop the
  marker-carrying packet itself — handled by the timestamp-change fallback (§6.1).

## 5. Architecture

Split the current `GstRtpReceiver` (which does both live and DVR) by responsibility:

```
                         ┌─────────────────────────────────────────────┐
   air → wfb → UDP/unix  │  RtpVideoReceiver  (NEW, no gstreamer)       │
   ────────────────────► │   recv thread → HevcDepayloader → frame cb   │
                         │   + side-channels: last-hop, gap→IDR,        │
                         │     stream-presence, latency probe,          │
                         │     restream(sendto)                         │
                         └───────────────┬─────────────────────────────┘
                                         │  NEW_FRAME_CALLBACK (unchanged)
                                         │
   recorded .mp4 (H265)  ┌───────────────┴─────────────┐  →  feed_packet_to_decoder()
   ────────────────────► │  GstFilePlayer (gst, slim)   │  →  MPP (always HEVC) → DRM
                         │   filesrc→qtdemux→h265parse→ │
                         │   appsink + seek/rate/pause  │
                         └──────────────────────────────┘
```

**New files**
- `src/hevc_depayloader.{h,cpp}` — pure logic, the unit-tested crux. No I/O, no gst,
  no MPP. RTP HEVC payloads in → Annex-B access units out.
- `src/rtp_video_receiver.{h,cpp}` — live receiver: socket + recv thread, drives the
  depayloader, hosts the side-channel code and the `idr_*` / `restream_*` C-API
  (moved verbatim from `gstrtpreceiver.cpp`; signatures unchanged so gsmenu keeps
  working).

**Repurposed**
- `src/gstrtpreceiver.{h,cpp}` → **`GstFilePlayer`** (DVR file playback + transport
  controls only). The appsink pull loop stays here for files. The `extern "C"`
  playback-control entry points keep their linkage.

**Deleted**
- Live gst elements: `udpsrc`/`appsrc`, `rtpjitterbuffer`, `tee`, `rtph265depay`,
  `h265parse`, `valve`, `udpsink`, the appsrc buffer-pool + socket→appsrc reader,
  and the `GstNetAddressMeta` pad probes.
- All H264 (see §7).

**Preserved contracts** (blast radius stays inside these modules): the
`NEW_FRAME_CALLBACK` feeding MPP, the `idr_*` / `restream_*` C-API used by gsmenu,
and the `extern "C"` DVR playback-control functions.

## 6. Component design

### 6.1 `HevcDepayloader` (the crux)

Pure — no sockets, threads, gst, or MPP. The receiver parses the 12-byte RTP header
once and passes payload + `{marker, rtp_ts}`.

```cpp
class HevcDepayloader {
public:
    using FrameCallback = std::function<void(const uint8_t* au, size_t len)>;
    explicit HevcDepayloader(FrameCallback on_access_unit);

    bool on_payload(const uint8_t* payload, size_t len, bool marker, uint32_t rtp_ts);
    void on_discontinuity();  // seq gap from receiver → drop partial FU/AU
    void reset();             // clear param-set cache + pending AU (stream restart)
};
```

RFC 7798 packet types (HEVC NAL header = 2 bytes; `type = (b0 >> 1) & 0x3F`):

| `type` | Structure | Handling |
|---|---|---|
| 0–47 | Single NAL unit | payload *is* the NAL → append with start code |
| 48 | Aggregation Packet (AP) | skip 2-byte AP header, loop: `u16 size` + that many bytes = one NAL |
| 49 | Fragmentation Unit (FU) | 2-byte hdr + 1-byte FU hdr (S/E/FuType); reassemble across packets |
| 50 / other | PACI / reserved | unsupported → drop + count |

- **FU reassembly:** on `S=1`, start a NAL — rebuild the 2-byte NAL header from the
  payload header with the type field replaced by `FuType`
  (`b0 = (b0 & 0x81) | (FuType << 1)`), append fragment. `S=0` appends. `E=1`
  completes. A new `S=1` mid-NAL, or `on_discontinuity()`, discards the partial NAL
  and marks the AU corrupt.
- **Assumption:** `sprop-max-don-diff = 0` → **no DONL/DOND fields** in AP/FU
  (standard single-layer low-latency HEVC). Asserted/documented; not configurable.
- **Parameter-set cache (replicates `config-interval=-1`):** cache the latest
  VPS(32)/SPS(33)/PPS(34). When an AU contains an IRAP slice (types 16–21, matching
  the existing `has_idr_frame`) without VPS+SPS+PPS already present, prepend the
  cached set. If none cached yet (cold join on a keyframe-less stream), emit as-is
  and let IDR recovery spin — same as today.
- **Emit timing (the win):** accumulate NALs sharing one `rtp_ts` into the current
  AU; **flush the instant `marker == true`** (zero lookahead). Fallback: a packet
  with a *new* `rtp_ts` while an AU is pending (lost marker packet) flushes the
  previous AU first.
- **Output:** 4-byte start codes; AU assembled directly into the buffer the callback
  consumes (one fill, no extra copy).
- **Error handling (network-facing attack surface — gst's hardened parser is gone):**
  every read bounds-checked; truncated payload headers, AP sizes overrunning the
  buffer, short FU headers all drop-and-count. Malformed input degrades to
  "corrupt AU → discard → IDR recovery", never a crash or OOB read. Explicit fuzz
  target (§8).

### 6.2 `RtpVideoReceiver` (sockets, threading, side-channels)

```cpp
class RtpVideoReceiver {
public:
    using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>>)>;
    explicit RtpVideoReceiver(int udp_port);          // UDP (production: 5600)
    explicit RtpVideoReceiver(const char* unix_sock); // abstract AF_UNIX dgram (compat)
    void start(NEW_FRAME_CALLBACK on_frame);
    void stop();
private:
    void recv_loop();
    HevcDepayloader m_depay;
};
```

Recv loop (one thread does everything):

```
recvfrom(buf, &sender)                      ← MAX_PACKET_SIZE (4096), enlarged SO_RCVBUF
 1. update last-hop IP from `sender`        ← replaces GstNetAddressMeta (only on change)
 2. latency_probe::on_rtp_buffer(buf,…)     ← if active
 3. parse 12-byte RTP header (seq, marker, ts)
 4. stream-presence update (last_pkt_ms, up/down, decode-stall)
 5. seq gap? → m_depay.on_discontinuity() + request IDR
 6. restream enabled & target known? → sendto(buf)  ← replaces tee→valve→udpsink
 7. m_depay.on_payload(payload, len, marker, ts)
        └─ on marker → assemble AU into shared_ptr<vector> → on_frame(au) → MPP (+ DVR record)
 8. periodic tick (stream-presence timeout, restream retarget) — also fires on recv timeout
```

Side-channel mapping (removed from gst, mostly shrinks):

| Concern | Today (GStreamer) | Custom receiver |
|---|---|---|
| Last-hop sender IP | `GstNetAddressMeta` on udpsrc pad-probe + `gio` | `recvfrom()` source addr — frees `gstnet`+`gio` |
| RTP seq-gap → IDR | pad-probe parses header | recv loop already parses it |
| Stream up/down, decode-stall | pad-probe + pull-loop | recv loop + periodic tick |
| Restream | `tee→valve→udpsink`, `g_object_set` retarget | `sendto()` raw datagram; bool gates it |
| Latency probe | pad-probe → `on_rtp_buffer` | recv loop → `on_rtp_buffer` |
| IDR token send (port 11223) | plain UDP socket | unchanged |

The IDR / restream / stream-presence logic and the `idr_*` / `restream_*` C-API move
**verbatim** from `gstrtpreceiver.cpp`; only their trigger sites change
(pad-probe / pull-loop → recv loop).

**Threading decision.** Today gst decouples receive (udpsrc thread + queue) from
consume (appsink pull thread + a copy). The custom design collapses to a **single
recv→depay→feed thread** — one fewer hop and one fewer copy (part of the latency
win). Tradeoff: if `feed_packet_to_decoder` blocks during a decoder hiccup we are
not draining the socket. Mitigation: enlarge `SO_RCVBUF` to a few MB (≈ many frames
of slack; frames are ~23 packets), and a long MPP stall already triggers IDR
recovery. **Escape hatch** if field testing shows drops under stall: a bounded
single-frame queue + separate feed thread restores gst-like decoupling at the cost
of one cheap handoff per frame. **Recommendation: ship single-thread, keep the queue
in reserve.**

**Error handling:** `recvfrom` EINTR → retry; other errno → log + brief backoff;
socket setup failure → throw (as today). Malformed packets handled by the
depayloader. `stop()` flips a flag; the recv timeout (~200 ms) lets the loop observe
it, then join + close + `reset_stream_tracking()`.

### 6.3 `GstFilePlayer` (DVR, unchanged behavior)

Slimmed from today's `GstRtpReceiver`: `filesrc → qtdemux → h265parse → appsink`
plus the appsink pull loop and transport controls (`set_playback_rate`,
`fast_forward`, `fast_rewind`, `skip_duration`, `pause`, `resume`). H265 only —
`detect_mp4_codec` H264 branch removed. Feeds the same `NEW_FRAME_CALLBACK`.

### 6.4 `main.cpp` integration

`main.cpp` owns both objects feeding the same `cb` (→ `feed_packet_to_decoder` +
`dvr_raw->frame`):

```cpp
std::unique_ptr<RtpVideoReceiver> receiver;     // live (custom)
std::unique_ptr<GstFilePlayer>    file_player;  // DVR (gst)
```

`extern "C"` playback-control routing (existing functions in the `extern "C"` block
at `main.cpp:583`, decls in `main.h:5-11`):
- `switch_pipeline_source(source_type, source_path)` → `"file"`: `receiver->stop()` +
  start `GstFilePlayer` on the path; `"stream"`: stop the file player +
  `receiver->start(cb)`. **No MPP reinit** (always HEVC).
- `fast_forward` / `fast_rewind` / `skip_duration` / `normal_playback` /
  `pause_playback` / `resume_playback` → routed to `file_player` only (DVR-only
  operations; they move out of the receiver onto `GstFilePlayer`).

## 7. H265-only deletions

| Removed | Safe because |
|---|---|
| `reinit_mpp_decoder()` (main.cpp:805-831), `current_mpp_type`, `stream_mpp_type` (802-803) | MPP inits once as `MPP_VIDEO_CodingHEVC`, never switches |
| `mpp_reinit_mutex`, `mpp_reinit_pending` (108-109) + decode-loop pending checks (307,315) | no mid-stream reinit → decode thread loses a mutex + branch |
| Live-decoder codec branch `mpp_type = (codec==H264)?AVC:HEVC` (main.cpp:1528-1533) | live path is H265-only → fixed HEVC |
| `detect_mp4_codec()` H264 branch | DVR recordings are H265; `GstFilePlayer` hardcodes `h265parse` |
| RFC 6184 (H264 depay) | never written — depayloader is H265-only |

MPP init becomes a single fixed `mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC)`.
The decode thread keeps its real work (`info_change → init_buffer`,
`errinfo/discard → idr_request_decoder_issue`, good-frame → `idr_notify_decoded_frame`).

**Keep the `VideoCodec` enum intact, including `H264`.** The DVR *re-encoder*
(`dvr_reenc_set_codec` at main.cpp:644-653, `mpp_encoder`) still encodes to H264 and
uses `VideoCodec::H264` — it is a separate recording subsystem and is **not** touched.
The `--codec` CLI arg (main.cpp:1229-1237) stays parsed, but the live decoder + receiver
ignore it and always use HEVC (warn if `--codec h264` is passed). Move the enum +
`video_codec()` helper out of the old `gstrtpreceiver.h` into a small shared
`src/video_codec.h`.

**Preserved OSD contract:** the live frame callback keeps publishing the
`gstreamer.received_bytes` fact (main.cpp:1020) — `osd.cpp` matches that exact name
for its bitrate slot. The name is retained even though GStreamer no longer feeds the
live path.

## 8. Build changes (`CMakeLists.txt`)

- Add sources `src/hevc_depayloader.cpp`, `src/rtp_video_receiver.cpp`.
- Repurpose `src/gstrtpreceiver.cpp` → `src/gst_file_player.cpp`.
- **Drop** `gstreamer-net-1.0` (`gstnet`) and `gio-2.0` from pkg-config + link.
- **Keep** `gstreamer-1.0` + `gstreamer-app-1.0` (DVR `GstFilePlayer`).
- Test target: add depayloader tests; it already links gst for golden references.

## 9. Test bench (this *is* the safety net — D4, no runtime fallback)

1. **Golden parity (headline).** Capture real RTP to a fixture file (the Python
   bind-5600 method already validated; capture ≥1 IDR keyframe, FU fragmentation,
   and ideally an induced loss event). Feed it two ways on the host: a gst
   `appsrc→rtph265depay→h265parse→appsink` reference **and** `HevcDepayloader`.
   Assert the **emitted NAL sequence per AU matches** — compared NAL-by-NAL
   (type + payload + order), *not* raw bytes, so start-code formatting differences
   do not cause false diffs. Parameter-set reinsertion is aligned to gst's
   `config-interval=-1` and documented as the one intentional equivalence.
   Catch2, `USE_SIMULATOR` host build.
2. **Unit edge cases:** single-NAL / AP(48) / FU(49) across N packets; lost-middle
   fragment → discard; lost-End → next-Start recovers; marker → emit; missing-marker
   + ts-change → flush fallback; VPS/SPS/PPS cache + reinsert-before-IDR; malformed
   (truncated header, AP size overrun, short FU) → drop, no crash/OOB.
3. **Fuzz target:** `HevcDepayloader::on_payload` under libFuzzer/ASan feeding mutated
   payloads, asserting no crash/OOB.
4. **On-target A/B (before flashing for flight):** same hardware/link, current binary
   vs new binary; compare g2g latency probe + stutter metrics; verify IDR recovery
   (induce loss), restream, and DVR record→playback; soak before flight.

## 10. Rollout

Build → full host bench (golden + unit + fuzz) → flash a test unit → on-target A/B +
soak → adopt. No runtime fallback (D4), so this bench discipline replaces it.

## 11. Risks & open questions

- **Single-thread under decoder stall** (§6.2) — mitigated by `SO_RCVBUF`; escape
  hatch is the bounded queue. Resolve via on-target soak.
- **Marker-bit reliability under RF loss** — sender is correct (§4); the
  timestamp-change fallback covers dropped marker packets. Confirm with an induced
  packet-loss test in the golden fixture.
- **DONL/DOND absence** — assumed (`sprop-max-don-diff = 0`). If a future encoder
  enables it, AP/FU parsing needs a flag. Out of scope here; documented.
- **H264 DVR recordings unplayable** after cutover (§2) — accepted.
