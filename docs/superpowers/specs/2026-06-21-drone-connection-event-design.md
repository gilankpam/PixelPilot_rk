# Drone connection event: single-source, subscribe-able connection state

- **Date:** 2026-06-21
- **Status:** Design — pending review
- **Scope:** PixelPilot_rk only. No fpvd changes required.

## Problem

The GS menu decides whether the drone is connected by probing it: `drone_reachable`
is set **solely** from whether `GET /air/config` (the `/air/*` proxy → the drone's own
HTTP) returns 2xx (`src/gsmenu/settings_fpvd.c:604-612`, comment `:600-603`). That probe:

- is a heavyweight config round-trip used as a liveness ping (1.5 s connect / 5 s total
  timeout per GET, `:457-458`), conflating "drone HTTP server responsive" with "link up";
- is sampled only on the menu's combined refresh cadence — **3 s** visible, **60 s**
  hidden, 2 s when the GS daemon is down (`:784`) — bundled sequentially with
  `/gs/config` and `/gs/status` (`:578-580`);
- duplicates work fpvd already does authoritatively.

Meanwhile fpvd's `ConnectionMonitor` already tracks the drone via the wfb-ng **tunnel**
plus an HTTP heartbeat, as a debounced `disconnected → armed → connected` state machine,
and **exposes it at `GET /gs/status` under `.connection`** — which PixelPilot **already
fetches every refresh** (`settings_fpvd.c:569,579`) and currently throws away (it reads
only `beamforming`/`txpower` out of that response).

## Goals

- **A — robustness:** source drone connection state from fpvd's `.connection` (the real
  link signal) instead of the `/air` probe. Single source of truth.
- **B — latency:** a **dedicated, lightweight `/gs/status`-only poll** on its own short
  interval, independent of the heavy config refresh and of menu visibility, so connection
  changes are reflected within ~1 s in the background.
- **Global event:** expose connection state as a process-wide **publish/subscribe**
  service any PixelPilot component can subscribe to (the menu is the first subscriber;
  an OSD indicator / auto-DVR are plausible future ones).

## Non-goals

- No fpvd changes — `.connection` already exists and the monitor is enabled by default.
- Not a generic multi-topic event bus — a connection-state pub/sub, designed to extend.
- Leave the RTP `g_stream_up` *video*-presence detector (`rtp_video_receiver.cpp`) alone;
  it answers a different question (are video packets flowing) and drives IDR/restream.
- No OSD connection indicator in this change (it becomes a trivial future subscriber).

## fpvd surface (verified)

`GET /gs/status` → `.connection` =
`{ "enabled": bool, "state": "disconnected"|"armed"|"connected", "reason": str, "sinceMs": int|null }`
(`gs/fpvdgs/connection_monitor.py:99-103`). The monitor is **enabled by default**
(`ConnectionMonitorConfig.enabled = True`, `:32`) and `build_status` always includes the
`connection` block when the supervisor passes it (`status.py:66-67`, `supervisor.py:187`).
`armed` is the brief "tunnel seen, confirming over HTTP" window.

## Design

### New service: `conn_state` (`src/conn_state.{h,c}`)

C with an `extern "C"` header so both the C gsmenu and C++ OSD/main can use it; pthreads +
libcurl + cJSON, mirroring `settings_fpvd.c`. Lives at `src/` top level (a process-wide
service, not a gsmenu internal).

```c
typedef enum {
    CONN_UNKNOWN = 0,     // no data yet / fpvd or GS unreachable / .connection absent
    CONN_DISCONNECTED,
    CONN_ARMED,
    CONN_CONNECTED,
} conn_state_e;

typedef struct {
    conn_state_e state;
    char         reason[64];   // fpvd .reason (disconnect cause); "" if none
    long         since_ms;     // .sinceMs, -1 if n/a
    uint64_t     updated_ms;   // local monotonic ms of last successful poll
} conn_state_t;

typedef void (*conn_state_cb)(const conn_state_t *st, void *ud);

void         conn_state_start(const char *base_url, int interval_ms);
void         conn_state_stop(void);
conn_state_t conn_state_get(void);                          // current snapshot (by value)
int          conn_state_subscribe(conn_state_cb cb, void *ud);  // returns token >= 0;
                                                                // fires cb once w/ current state
void         conn_state_unsubscribe(int token);
```

**Poller thread:** GETs `{base_url}/gs/status` every `interval_ms` (default **1000 ms**)
with short curl timeouts (~800 ms connect / ~1500 ms total) so a stall can't blow the
cadence (localhost, normally sub-ms). Parses `.connection` → enum. Non-200, missing
`.connection`, or parse failure → `CONN_UNKNOWN`. Fires subscribers **only on a state
change**, plus once at subscribe time with the current state. Honors a stop flag; joined
on `conn_state_stop()`.

**Delivery / thread-safety:** an internal mutex protects the current snapshot and the
subscriber list. Callbacks run on the poller thread; the bus snapshots the subscriber list
under the lock then invokes **after unlocking** (same notify-after-unlock / lock-order
discipline as `settings_fpvd.c`'s `notify_listener`). Subscribers must be brief and marshal
to their own thread — exactly the `lv_async_call` pattern gsmenu already uses.
`conn_state_get()` returns a value copy taken under the lock.

**Pure core split out for testing:** `conn_state_parse(const char *json, conn_state_t *out)`
performs the JSON → state mapping with no I/O, so parse + transition logic is host-testable
(Catch2 / `USE_SIMULATOR`) without a live GS.

### gsmenu integration (first subscriber)

Reachability mapping (no fallback):

| `.connection.state` | menu reachability |
|---|---|
| `connected` | reachable → rows unlocked |
| `armed` | not reachable → OFFLINE-locked (conservative; transient) |
| `disconnected` | not reachable → OFFLINE-locked |
| `UNKNOWN` (fpvd/GS unreachable, monitor off, pre-first-poll) | not reachable → OFFLINE-locked |

- `prov_is_reachable()` returns `conn_state_get().state == CONN_CONNECTED`. No `/air`
  fallback.
- **Delete `G.drone_reachable`** entirely. Its two readers both move to `conn_state_get()`:
  1. **Row-locking** — `prov_is_reachable` (`settings_fpvd.c:986-993`).
  2. **Apply-routing** — `run_job_unlocked` (`:679`) decides whether a shared/drone setting
     is pushed to the drone or applied GS-only; now `reachable = conn_state_get().state ==
     CONN_CONNECTED`.
- Remove the `/air` 2xx → `drone_reachable` logic (`:604-612`) and the on-502 flip
  (`:737`). `/air/config` **stays** in the slow refresh purely to populate `air_snapshot`
  (the drone's config values the menu displays/edits); its result no longer drives any
  reachability flag. The job error path still surfaces a relayed 502 as a "Drone
  unreachable" message to the user — it just no longer writes the (deleted) cache field.
- Subscribe at fpvd-provider init: the callback calls the existing `notify_listener()` →
  `lv_async_call` → `pp_page_reapply_lock_state` on the LVGL thread. Rows lock/unlock within
  ~1 s of a real link change, in the background, regardless of menu visibility.
- The slow refresh's notify return (`:616`) loses its `was_drone != drone_reachable` term;
  connection-driven re-renders now come from the conn_state subscription instead.

### Startup wiring

- `main.cpp` calls `conn_state_start(base_url, 1000)` at startup and `conn_state_stop()` on
  shutdown, gated on the **same condition that registers the fpvd settings provider** (PP
  can run with the dummy/stub provider and no fpvd). `base_url` is the same fpvd URL the
  menu uses (`FPVD_DEFAULT_URL` = `http://127.0.0.1:8080`, overridable from the same place).
- `/gs/status` is now fetched by two callers at different rates: the conn_state poller
  (~1 s, reads `.connection` only) and the slow config refresh (3 s/60 s, reads
  `beamforming`/`txpower`). The extra localhost GET/s is negligible; consolidating them is a
  possible later cleanup, intentionally deferred to keep the modules decoupled.

## Tradeoff (intended)

With no fallback, `UNKNOWN` → not reachable. So if fpvd's `ConnectionMonitor` is disabled,
the GS daemon is down, or during the ~1 s window before the first poll completes, drone/
shared rows are OFFLINE-locked. This is benign (monitor is default-on; the startup transient
self-clears within ~1 s) and is the deliberate consequence of making fpvd authoritative.

## Testing

- **Host unit tests (Catch2, `USE_SIMULATOR`):** drive `conn_state_parse` with
  connected/armed/disconnected payloads, missing `.connection`, non-JSON, and
  `sinceMs` present/null; assert the enum + `reason` + `since_ms` mapping. Drive the pub/sub
  with a counting subscriber: transitions fire exactly once per change, no fire on
  unchanged polls, subscribe delivers the current state immediately, unsubscribe stops
  delivery.
- **Simulator:** wire the existing `PP_SIM_DRONE_OFFLINE` / dummy-provider offline toggle
  (`settings_dummy.c`) to drive `conn_state` so menu lock/unlock is exercisable in the sim
  without a live GS.
- **On-device (GS):** deploy, toggle the drone link, confirm drone/shared rows lock within
  ~1 s and unlock on reconnect, in the background (menu both hidden and visible); confirm
  GS-daemon-down locks the rows; confirm an apply while disconnected routes GS-only.

## Files

- **New:** `src/conn_state.h`, `src/conn_state.c`, `test/conn_state_test.cpp` (or the
  repo's existing host-test location).
- **Modified:** `src/gsmenu/settings_fpvd.c` (delete `G.drone_reachable`, subscribe,
  re-route both readers, drop `/air` liveness role), `src/main.cpp` (start/stop the
  service), `CMakeLists.txt` (add source + test target).

## Decisions recorded

1. Standalone `conn_state` module (vs. extending `settings_fpvd`'s worker or reusing the
   OSD fact bus).
2. Poll interval 1000 ms (tunable constant; could be lifted to config later).
3. `armed` treated as not-reachable (conservative; transient).
4. **No fallback** — `UNKNOWN` → not reachable (per user). Enables deleting
   `G.drone_reachable` and single-sourcing both row-locking and apply-routing.
5. Connection-specific pub/sub, not a generic typed event bus (YAGNI; designed to extend).
