# rsDeck UI/Network Core Split — Handoff & Next Steps

**Status:** Stages 0–3 are implemented and pass static diagnostics. The tree now runs
with the UI loop on `loopTask` (core 1), the network task on core 0 when
`RSDECK_UI_CORE_SPLIT=1`, and a single-task compatibility path when the flag is `0`.
An on-device build and sustained-traffic soak test remain.

Build flag: `-DRSDECK_UI_CORE_SPLIT=1` in `platformio.ini`. Set it to `0` to get the
single-task superloop (every guard compiles to a no-op). Flip this to A/B test.

> Build/flash needs **esptool v5** (`merge-bin` hyphen syntax). Already installed
> (5.3.1) in the PlatformIO penv. If you hit `invalid choice: 'merge-bin'`, run
> `python3 -m pip install --upgrade 'esptool>=5'`.

---

## Why

The UI goes sluggish/unresponsive under network load. Root cause: single‑threaded
superloop — LVGL render + input share `loop()` with `rns.loop()` (Reticulum crypto), so
UI latency == loop cycle time, which grows with traffic. Confirmed with the user it's
**load‑dependent**, not an idle leak.

**Fix:** move the Reticulum/radio/network stack to a FreeRTOS task pinned to **core 0**;
keep LVGL + input on the Arduino `loopTask` (**core 1**). Render never waits on crypto.

## Shared subsystems that cross the two cores (must be serialized)

1. **SPI bus** — display (LovyanGFX), SX1262 radio, and SD all share FSPI (SCK 40 / MOSI
   41 / MISO 38). Guarded by `CoreSync::spiBusMutex` (recursive). **DONE.**
2. **Reticulum/LXMF/AnnounceManager data + flash/SD persistence** — one "backend" lock,
   `CoreSync::rnsMutex` (recursive). **DONE.**
3. **Network → UI status** — lock‑free `CoreSync::netStatus` snapshot + a deferred‑toast
   bridge (`requestToast`/`takePendingToast`). **DONE (infra).**

**Lock ordering (keep it consistent to stay deadlock‑free): always `rnsMutex → spiBusMutex`.**
The display flush takes `spiBusMutex` only (never `rnsMutex`), so there's no inversion.

---

## What's DONE

### New module
- `src/platform/CoreSync.h` / `.cpp` — mutexes (`spiBusMutex`, `rnsMutex`), RAII guards
  (`SpiBusGuard`, `RnsGuard` blocking, `RnsTryGuard` try‑lock), the `NetStatus` atomic
  snapshot, and the toast bridge. All no‑ops when the flag is 0.
- `CoreSync::begin()` is called first thing in `setup()` and fails explicitly if mutex
  allocation is unsuccessful.

### Stage 1 — SPI bus guarded (complete)
- `src/hal/Display.cpp` — `lvgl_flush_cb`, panel sleep, and panel wake wrapped in
  `SpiBusGuard`.
- `src/radio/SX1262.cpp` — all **5** SPI primitives wrapped: `singleTransfer`,
  `executeOpcode`, `executeOpcodeRead`, `writeBuffer`, `readBuffer` (guard taken *after*
  `waitOnBusy()`, around the transaction only).
- `src/storage/SDStore.cpp` — runtime methods guarded (recursive mutex handles the
  nesting: `writeString→writeAtomic`, `ensureDir` recursion, etc.). `begin()` is
  intentionally unguarded (boot, pre‑task).
- Runtime `openDir`+iterate paths in `MessageStore`, `IdentityManager`, and Settings,
  plus raw SD mirror copies in `ReticulumManager`, hold `SpiBusGuard` for the complete
  `File` lifetime.

### Stage 2 — complete
- `src/platform/CoreSync.*` — `NetStatus` + toast bridge added.
- `main.cpp` `announceWithName()` — refactored: guards `rns.announce` with `RnsGuard`,
  delivers flash/toast via the snapshot bridge (safe from either core).
- `main.cpp` loop split landed:
  - Added `networkLoopStep()` + `uiLoopStep()`.
  - Added `networkTask()` pinned to core 0 under `RSDECK_UI_CORE_SPLIT`.
  - `loop()` now dispatches split mode (`uiLoopStep()` only) vs stock mode
    (`uiLoopStep()` + `networkLoopStep()`).
  - Network-owned status updates now publish to `CoreSync::netStatus`.
  - UI status tick reads `CoreSync::netStatus` and refreshes UI under `RnsTryGuard`.
  - Added setup spawn after all callbacks, screens, and boot routing are initialized:
    `xTaskCreatePinnedToCore(networkTask, "netstack", 16384, ..., 0)`.
  - Task allocation failure falls back to the single-task network loop and restores
    the legacy radio-wait LVGL yield callback.

### Stage 3 — fine-grained backend ownership (complete)
- Pure LVGL input/navigation runs without the backend lock.
- Backend actions use blocking `RnsGuard`; refresh paths use `RnsTryGuard` and keep the
  previous frame when the backend is busy.
- Nodes and Contacts render from UI-owned snapshots and commit delayed actions by stable
  destination hash instead of mutable vector index.
- Network-originated message/status events publish atomics; only the UI core touches
  LVGL and audio.
- Deferred name-cache and known-destination persistence use independent throttle state.

---

## What REMAINS

Build both flag values and run the on-device soak plan below. Static analysis cannot
validate RF timing, task stack headroom, display integrity, or watchdog behavior under
real sustained traffic.

---

## Build / flash / on‑device test plan

```bash
pio run                      # compile (flag on)
make flash PORT=/dev/tty...  # or use the Ratspeak web flasher with rsdeck-merged.bin
```

Watch the serial `[HEART]` line — it already prints `loop=<ms>` (max loop time) and
`heap=/min=`. Test sequence:

1. **Regression (flag on):** confirm it boots, UI works, radio RX/TX works, SD
  reads/writes, and no display corruption occurs.
2. Load the device (WiFi on + TCP hub + announce traffic).
   - UI should stay smooth (scroll/type) while `[HEART] loop=` on the **UI core** stays
     low. The heavy time now lives in the network task.
   - Confirm status bar (wifi/tcp/ble/peers) still updates — it's snapshot‑driven now.
   - Confirm announce (Enter on Home) still flashes the TX indicator + toasts, and that
     auto/boot announces work (they run on the network core via the bridge).
   - Send/receive an LXMF message; open a chat mid‑traffic.
   - Watch for **stack overflow** panic (raise `netstack` stack if so) and for any
     **`Interrupt wdt timeout`** (would indicate an SPI collision — check every radio/SD
     path is guarded).
3. Set flag to 0, rebuild — must behave exactly like today (sanity).

## Reference

- Full running context is in Claude project memory: `ui-core-split.md`,
  `esptool-v5-required.md`.
- LSP "file not found / Arduino.h" errors in the editor are noise (clangd lacks the
  PlatformIO `-Isrc` include path); the real `pio run` is the source of truth.
