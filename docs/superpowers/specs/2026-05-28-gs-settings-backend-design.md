# GS-Side Settings Backend

**Status:** Design
**Date:** 2026-05-28

## Summary

Add a real settings backend that writes ground-station–side configuration on the SBC where PixelPilot runs. Plugs into the existing `pp_settings_provider_t` seam (used today by `settings_fpvd` for the drone-side HTTP API) via a new top-level **router** provider that owns both children and dispatches by domain.

Scope is bounded to five rows:

- **Camera > Codec** — already writes drone-side via fpvd; now also writes GS-side `/etc/default/pixelpilot` (`CODEC=...`).
- **Link > Channel** — already writes drone-side via fpvd; now also writes GS-side `/etc/wifibroadcast.cfg` (`wifi_channel`) and restarts `wifibroadcast.service`.
- **Link > Bandwidth** — already writes drone-side via fpvd; now also writes GS-side `/etc/wifibroadcast.cfg` (`bandwidth`) and restarts `wifibroadcast.service`.
- **Link > RX Power** — new GS-only row; writes per-NIC `wifi_txpower` map in `/etc/wifibroadcast.cfg` and restarts `wifibroadcast.service`.
- **Display > HDMI Mode** — GS-only; writes `/etc/default/pixelpilot` (`SCREEN_MODE=...`); takes effect on next PixelPilot restart.

Everything else (APFPV, GS Wi-Fi hotspot, DVR, network helpers) is out of scope — those will follow a separate spec.

## Goals

- Single registration seam: page code keeps calling `pp_settings_set_async("air"|"gs", page, key, value)`. Router decides where the bytes land.
- Drone-first ordering on fanned-out writes: fpvd succeeds → then GS write. fpvd fails → GS untouched.
- Per-row busy state and toast UX is reused from the existing fpvd path; gs_local exposes the same `on_done` contract.
- Atomic, crash-safe file writes for `/etc/wifibroadcast.cfg` and `/etc/default/pixelpilot`.
- RX power UX matches the old `gsmenu.sh`: a single 1–100 % slider, mapped per-NIC into the driver-specific txpower range.
- Channel and HDMI mode dropdowns enumerate live options from `iw list` / `drm_info` at startup, with hardcoded fallbacks if parsing fails.
- Sim build is unchanged — keeps the dummy provider; dummy seed is extended to cover the new GS keys so widgets render correctly without a backend.

## Non-goals

- **Live apply of codec / HDMI mode.** Both write to `/etc/default/pixelpilot` and surface a "Applies on next restart" toast. Live pipeline rebuild and DRM remodeset are explicitly out of scope; future work.
- **APFPV, hotspot, NetworkManager dispatcher logic** that the old `gsmenu.sh` carried — separate effort.
- **External-edit drift detection.** gs_local re-reads the snapshot after its own writes and at startup; it does not watch the file. Same limitation as fpvd's snapshot model.
- **Authentication / privilege separation.** PixelPilot runs as root on the GS (the old script edited these paths directly). No sudo wrapper.
- **Per-NIC RX power rows.** Single aggregate slider; the writer fans out across detected `wlx*` NICs.

## Decisions captured in brainstorming

| Decision | Choice |
|---|---|
| GS codec target | `/etc/default/pixelpilot` (`CODEC=...`) |
| Fan-out ordering for shared rows | Drone first, then GS (abort GS write on fpvd failure) |
| RX power UX | Single 1–100 % slider, per-NIC mapping done in the writer (mirrors old script) |
| HDMI mode target | `/etc/default/pixelpilot` (`SCREEN_MODE=...`) |
| Codec / HDMI mode application | Save now, toast "Applies on next restart". Live-apply is follow-up work. |
| Architecture | Composite router provider with static fan-out table (Approach A from brainstorm) |

## Architecture

Three new units plus the registration seam:

```
            pp_settings_* (api, unchanged)
                      │
                      ▼
            ┌─────────────────────┐
            │  settings_router    │  ◀── pp_settings_register_router()
            │  - dispatch by      │
            │    domain           │
            │  - fan-out table    │
            └────────┬────────────┘
                     │
        ┌────────────┴────────────┐
        ▼                         ▼
┌──────────────────┐    ┌──────────────────────┐
│ settings_fpvd    │    │ settings_gs_local    │
│ (existing)       │    │ (new)                │
│ HTTP → 10.5.0.10 │    │ file edits + systemd │
└──────────────────┘    └──────────────────────┘
```

### settings_router

Owns lifetime of both children. Exposes the `pp_settings_provider_t` to `pp_settings_register`.

**Read (`get`).** Consults the child indicated by domain (`"gs"` → gs_local; `"air"` → fpvd). For fanned-out keys, fpvd is canonical on read (drone is source of truth for channel / bandwidth / codec).

**Write (`set_async`).** Static fan-out table:

```c
typedef struct {
    const char *domain;    // "air"
    const char *page;      // "wfbng"
    const char *key;       // "channel"
    const char *gs_domain; // "gs"
    const char *gs_page;   // "wfbng"
    const char *gs_key;    // "channel"
} fanout_entry_t;

static const fanout_entry_t k_fanout[] = {
    { "air", "wfbng",  "channel", "gs", "wfbng", "channel" },
    { "air", "wfbng",  "width",   "gs", "wfbng", "bandwidth" },
    { "air", "camera", "codec",   "gs", "pp",    "codec" },
};
```

For each write:
1. If `(domain,page,key)` matches a fan-out row: call fpvd. On error, return immediately (drone-first). On success, call gs_local with the gs_*-mapped key. Aggregate the gs_local result into the on_done.
2. Else, dispatch by domain straight to the appropriate child.

**Snapshot listener.** Both children call up to the router; router fans the listener call once to the UI thread via `lv_async_call`. (Existing fpvd listener already uses this pattern.)

**`is_connected` / `is_locked`.** Aggregate: connected = both children connected (with GS-only rows checking just gs_local); locked = either child reports lock for that key.

### settings_gs_local

Mirrors fpvd's worker model with file I/O instead of HTTP.

**State** (single mutex):
- `struct gs_snapshot snap` — cached values for the five keys (see below).
- `bool connected` — set false on consecutive file/exec errors so widgets can grey out (rare; mostly cosmetic since file I/O succeeds or it doesn't).
- Listener pointer + user data; fired via `lv_async_call`.
- Worker thread + job queue + per-key coalescing — pattern lifted from `settings_fpvd.c`.

**Snapshot:**

```c
struct gs_snapshot {
    char *wfbng_channel;   // "36"
    char *wfbng_bandwidth; // "20"
    int   rx_power_pct;    // 1..100
    char *codec;           // "h265"
    char *hdmi_mode;       // "1920x1080@60"
};
```

Re-read from disk after every successful write and at startup. Cheap.

**Per-job sequence** (worker thread):
1. Pick the writer for the key (`wifibroadcast_cfg_writer` or `pixelpilot_env_writer`).
2. Call writer; on failure return `on_done(-1, msg, ud)`.
3. If the writer requested a service restart (wifibroadcast), spawn `systemctl restart wifibroadcast.service` with 5 s timeout. Non-zero exit → `on_done(-1, "wifibroadcast restart failed", ud)` but **file is already written** — snapshot reflects the new value, row commits, toast surfaces the restart error so the user knows.
4. If the writer requested a "needs restart" notice (pixelpilot env) → `on_done(0, "Applies on next restart", ud)`. Widgets treat non-NULL message on success as a toast.
5. Refresh snapshot via re-read; call listener via `lv_async_call`.

### settings_gs_writers

Two writers in one file.

**`wifibroadcast_cfg_writer`** — handles `wfbng/channel`, `wfbng/bandwidth`, `link/rx_power`.
- Read `/etc/wifibroadcast.cfg` into memory.
- Regex-replace the target line (`^wifi_channel = .*`, `^bandwidth = .*`, `^wifi_txpower = .*`).
- If the line is missing, insert after the `[common]` header.
- For `rx_power`: enumerate `wlx*` interfaces via `/sys/class/net`, look up each driver via `udevadm info`, map percentage → driver-specific signed integer, format as JSON-ish `wifi_txpower = {"wlx...": <int>, ...}`. Driver ranges hard-coded from the old script (`rtl88xxau_wfb` -1000…-3000, `rtl88x2eu` 1000…2900). Unknown driver: skip that NIC.
- Atomic write via `mkstemp` in `/etc` + `fsync` + `rename`. Failed rename leaves original intact.
- Requests `systemctl restart wifibroadcast.service`.

**`pixelpilot_env_writer`** — handles `pp/codec`, `display/hdmi_mode`.
- Read `/etc/default/pixelpilot` into memory (or start with empty if missing).
- KEY=VALUE upsert (`CODEC=`, `SCREEN_MODE=`). Preserves unrelated lines, comments, and order.
- Quote values that contain whitespace or special chars (safe shell-source semantics).
- Atomic write via `mkstemp` + `fsync` + `rename`.
- Requests "needs restart" notice — no service action.

### settings_gs_enum

Startup-time enumeration via `popen`.

- **Channels** — `iw list` parsed for enabled channels; output normalized to `"<chan> (<freq> MHz)"` to match the old gsmenu format. Result cached in a static `char *`; freed at shutdown.
- **HDMI modes** — `drm_info -j /dev/dri/card0` parsed via cJSON (already vendored for fpvd), filters interlaced modes, sorts and uniques.

Exposed via new `pp_settings_get_options(domain, page, key)` returning newline-joined string or NULL. Pages may use this to override their hard-coded dropdown options:

```c
const char *modes = pp_settings_get_options("gs", "display", "hdmi_mode");
pp_dropdown(page, ..., modes ? modes : "1920x1080@60\n1280x720@60");
```

On parse failure (binary missing, malformed output) → NULL; pages fall back to their compile-time list. Logged once via `LV_LOG_WARN` at startup.

### Files

**New:**
- `src/gsmenu/settings_router.c`
- `src/gsmenu/settings_gs_local.c` + `settings_gs_local_internal.h`
- `src/gsmenu/settings_gs_writers.c`
- `src/gsmenu/settings_gs_enum.c`
- `tests/settings_router_test.cpp`
- `tests/settings_gs_local_test.cpp`
- `tests/settings_gs_local_integration_test.cpp` (tag `[gs-local]`)

**Modified:**
- `src/gsmenu/settings.h` — add `pp_settings_register_router(void)`, `pp_settings_get_options(...)`.
- `src/gsmenu/pages/link.c` — add the **RX Power** slider (`"gs","link","rx_power"`, 1–100).
- `src/gsmenu/pages/display.c` — switch HDMI Mode dropdown to use `pp_settings_get_options("gs","display","hdmi_mode")` (binding stays `"gs","display","hdmi_mode"`).
- `src/gsmenu/pages/camera.c` — no change; codec row keeps its `"air","camera","codec"` binding, fan-out is invisible to the page.
- `src/main.cpp` (or wherever today's `pp_settings_register_fpvd()` lives) — replace with `pp_settings_register_router()` in the device path. Sim stays on dummy.
- `src/gsmenu/settings_dummy.c` — seed the new GS keys (`rx_power=50`, `hdmi_mode="1920x1080@60"`, `codec="h265"` already exists for "air"; add a parallel "gs" entry).
- `CMakeLists.txt` — add new sources.

## Data flow

### Fanned-out write (channel)

```
user changes Channel
        │
        ▼
pp_dropdown widget → pp_row_set_busy(true)
                   → pp_settings_set_async("air","wfbng","channel","36")
        │
        ▼
router.set_async:
   - fan-out lookup → match: gs target ("gs","wfbng","channel")
   - enqueue job { fpvd_first=true, gs_target=..., key, value }
        │
   worker thread:
        │
   1. fpvd.set_async(...) ── HTTP PATCH+apply ─▶ ok
        │
   2. gs_local.set_async("gs","wfbng","channel","36")
        │
        ▼
   gs_local worker:
       wifibroadcast_cfg_writer.set("wifi_channel", "36")
         - read /etc/wifibroadcast.cfg
         - replace "^wifi_channel = .*"
         - tempfile + rename
       systemctl restart wifibroadcast.service  (5 s timeout)
       on success: refresh snapshot, fire listener
        │
        ▼
   router aggregates on_done → lv_async_call → widget
        │
        ▼
   pp_row_set_busy(false); widget re-reads snapshot for the new value
```

### GS-only write (RX power)

Same chain but the router skips fpvd entirely. Percentage→per-NIC integer mapping happens inside `wifibroadcast_cfg_writer`, which owns the NIC enumeration and driver-type lookup. Page code stays declarative.

### `/etc/default/pixelpilot` write (codec, HDMI mode)

For codec: fpvd write runs as usual (drone-side `video.codec`). Fan-out triggers `pixelpilot_env_writer.set("CODEC","h265")` on the GS. `on_done(0, "Applies on next restart", ud)` — widget treats non-NULL message on success as a toast.

HDMI mode is GS-only — same writer, same toast.

## Error handling

| Failure | Behavior |
|---|---|
| `/etc/wifibroadcast.cfg` read missing | Snapshot keys remain unset; widgets render placeholders. Don't crash. |
| `mkstemp` / `rename` failure | `on_done(-1, "Failed to write GS config: <strerror>", ud)`; snapshot untouched; widget rolls back. |
| `systemctl restart wifibroadcast` non-zero | File already written; snapshot reflects new value; row commits; toast surfaces the restart error. |
| fpvd fails on fanned-out write | GS write skipped; standard fpvd error toast. No GS state touched. |
| fpvd succeeds, gs_local fails | Drone change stands; toast says `"Drone applied; GS write failed: ..."`. GS snapshot stays at old value so next read sees the drift; user can retry. |
| `iw list` / `drm_info` missing or malformed | `pp_settings_get_options` returns NULL; pages fall back to compile-time options. Logged once at startup. |
| No `wlx*` NICs detected | RX power write: `on_done(-1, "No wfb NICs detected", ud)`; slider locked via existing lock mechanism. |
| Unknown NIC driver | Skip that NIC's entry. All NICs unknown → same as "no NICs". |
| `udevadm` missing | RX power write fails with `"udevadm not found"`; slider locked. |
| Rapid same-key writes | Coalesce to one final write + one restart (same pattern as fpvd's queue). |
| External edit to config files | Not detected; gs_local re-reads on its own writes and at startup. Matches old gsmenu's 10 s cache TTL behavior. |

## Testing

### Unit — `tests/settings_router_test.cpp`

- Domain dispatch: `"gs"` keys hit only the gs child; `"air"` keys hit only the fpvd child (using fake providers).
- Fan-out table: writes to fanned-out keys hit fpvd then gs; gs is **not** called when fpvd returns error; gs IS called with the same value when fpvd succeeds.
- Aggregate `on_done`: status reflects the worse of the two child statuses; messages concatenate in a readable form.
- Snapshot listener: forwarded once to the UI thread regardless of which child fired.
- `is_connected` / `is_locked` aggregation.

### Unit — `tests/settings_gs_local_test.cpp`

- File writers operate on a temp dir (CMake injects path via `PP_GS_CONFIG_ROOT` overriding `/etc`, `/etc/default`, `/config`). Tests don't touch the host filesystem.
- `wifibroadcast.cfg` writer:
  - Replaces `wifi_channel` line in place.
  - Adds the line under `[common]` if missing.
  - `wifi_txpower` JSON built correctly for one NIC, two NICs, mixed driver types; values clamp to driver ranges; unknown driver skipped.
  - Atomic write: forced rename failure leaves original intact.
- `pixelpilot` env writer: KEY=VALUE upsert; preserves unrelated lines and comments; quoted values handled.
- `systemctl` invocation mocked via `PP_GS_SYSTEMCTL` env var pointing to a fake binary that records its argv and exits 0 / non-zero per fixture.
- RX power percentage → driver mapping: boundary values (1, 50, 100) per driver type.
- Coalescing: rapid `set_async` for the same key produces one writer invocation.

### Unit — enumeration

- `iw list` parser fed canned outputs (real, degenerate, empty); produces expected channel list or NULL.
- `drm_info` parser fed canned outputs; filters interlaced modes; produces sorted, uniqued list.
- Mocked via subprocess replacement: `PP_GS_IW_BIN`, `PP_GS_DRM_INFO_BIN` env vars point to fixture binaries.

### Integration — `tests/settings_gs_local_integration_test.cpp` (tag `[gs-local]`)

End-to-end on a real filesystem layout (tempdir) with a fake `systemctl` exiting 0. Covers the full job: enqueue → write → restart → snapshot refresh → listener fired on UI thread.

### Manual verification (documented, not automated)

- Run on a real GS box: confirm channel change restarts wifibroadcast and the link recovers.
- Confirm HDMI mode write persists across reboot and takes effect.
- Confirm RX power slider changes per-NIC `iw dev <if> info` output after restart.

## Future work (explicitly out of scope)

- Live apply for codec (GStreamer/MPP pipeline rebuild) and HDMI mode (DRM remodeset + framebuffer + LVGL display resize).
- APFPV / GS Wi-Fi hotspot rows and the NetworkManager dispatcher script.
- DVR settings (`gs/dvr/*`) that were stubbed as noops in the old gsmenu.sh.
- External-edit watch (inotify on `/etc/wifibroadcast.cfg`, `/etc/default/pixelpilot`).
- Channel intersection between drone-supported and GS-supported lists.
