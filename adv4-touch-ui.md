# AD4 Touch-UI: Klipper control from the SoC touchscreen

Goal: drive Klipper (print start/stop, temp targets, move, emergency stop)
**from the printer's own touchscreen**, with the SoC as a standalone control
panel that talks to the host (Orange Pi/RPi) over the same Ethernet link that
carries the MCU bridge. No soldering; the SoC already owns the display + touch
+ network.

## 1. What the SoC gives us (verified from the running rootfs)

| Capability | Detail (from `dmesg` + app env in the rootfs) |
|---|---|
| Display | framebuffer **`/dev/fb0`**, **480×854 portrait**, `rot270`. `QT_QPA_PLATFORM=linuxfb:tty=/dev/fb0:size=480x854:mmsize=156x244:offset=0`, `QWS_DISPLAY=transformed:rot270:LinuxFB:mmWidth156:mmHeight244:0`. A secondary `size=480x800` mode also appears (88 launches vs 694 for 480×854). **Not 640×800** — that resolution does not exist in the image. The panel is an 854×480 landscape display rotated 270° to portrait. `mmsize=156x244` is a Qt DPI-scaling hint only, not a verified physical size. |
| Touch | **TSC2007** driver → `/dev/input/event0` (also `event1`). `QWS_MOUSE_PROTO=TSLIB:/dev/input/event0`, `TSLIB_TSDEVICE=/dev/input/event0` |
| GUI toolkit | **Qt 4.8.6** (`/opt/qt-4.8.6`, `touchkit.so`, tslib generic plugin). The stock app is a Qt4 linuxfb program |
| Stock app | `/opt/PROGRAM/software/2.0.2/adventurer4-arm` (Qt4, unstripped ARM ELF, symbols present) |
| Calibration | `TSLIB_CALIBFILE=/opt/tslib-1.4/etc/pointercal` (already exists in rootfs) |
| Network | `eth0` (gmac0 GbE) — same port that carries the MCU bridge |
| **No local control API** | The stock app's network code is `cloudprinter/polarclient.cpp` + socketio (cloud print jobs). It does **not** expose a local Klipper/Moonraker control socket. → **we build the UI app.** |

So: the hardware + display + touch + Qt runtime all exist and are proven. The
missing piece is purely an **application** that (a) renders to `/dev/fb0`,
(b) reads TSLIB touches, and (c) talks to the host's Moonraker API.

## 2. Architecture

```
[SoC touch app]  --http/websocket (Moonraker API)-->  [Host: Moonraker on eth0]
      │  (Qt4 linuxfb, /dev/fb0, TSLIB touch)              │
      └── reads/writes print state                          └--> klippy --> MCU (via bridge)
```
- The SoC app is a **client** to Moonraker (JSON-RPC over HTTP + websocket).
  Moonraker already exposes everything Klipper needs: `gcode script`, `objects
  query` (temperatures, position), `emergency_stop`, `print`, `firmware
  restart`, event subscriptions (real-time temp/progress updates).
- The SoC stays **offline** (only talks to the host) — same constraint as the
  bridge. One cable, two jobs: MCU bridge (port 5000) + UI control (Moonraker
  port 7125, or Moonraker's default 7125).
- Touch input: reuse the stock **TSLIB + pointercal** (already calibrated in
  rootfs). No new driver work.

## 3. Two viable implementations (pick one)

### Option A — Replace the stock Qt4 app with a small Qt4 Klipper panel (native)
- A Qt 4.8.6 C++ (or PyQt4) app using the `linuxfb` platform plugin, exactly
  like the stock app. Reads TSLIB touches (same env: `QT_QPA_PLATFORM=linuxfb
  tty=/dev/fb0:size=480x854`, `QWS_DISPLAY=transformed:rot270:LinuxFB:...`,
  `QWS_MOUSE_PROTO=TSLIB:/dev/input/event0`).
- Talks to Moonraker via the SoC's bundled **curl 7.55.1 + jsoncpp** (both
  already in rootfs: `/opt/cloud/curl-7.55.1`, `/opt/cloud/jsoncpp`). WebSocket
  optional — plain HTTP polling at ~2 Hz is enough for temps/progress.
- **Pros:** native, no extra big deps, matches the proven stock stack, tiny.
- **Cons:** Qt4 C++ is dated; you write the UI yourself.

### Option B — A lightweight framebuffer UI (no Qt), e.g. pygame/SDL on linuxfb
- A Python SDL2 (`pip install pysdl2`) or C SDL app writing directly to
  `/dev/fb0`, touch via `/dev/input/event0` (raw evdev, no TSLIB needed).
- **Pros:** modern, easier to iterate, small binary.
- **Cons:** the rootfs is built around Qt4/tslib; a brand-new SDL stack is
  extra to ship (but the SoC is a full ARM Linux, so `pip`/`apt` work).

**Recommendation:** **Option A (PyQt4/Qt4 + curl + jsoncpp, HTTP-poll
Moonraker)** — it reuses *everything already in the rootfs* (Qt4, tslib,
curl, jsoncpp, pointercal) so the only new artifact is one small app binary
/ script you drop onto the SoC. Lowest risk, matches the vendor's own proven
display path.

## 4. Minimal UI (what to build first)

A single 480×800 portrait screen:
1. **Status header** — state (Idle/Printing/Heating/Error), progress %, elapsed.
2. **Temps** — hotend / bed actual vs target, with +/- 5° stepper buttons
   (→ `M104`/`M140` via `gcode script`).
3. **Big buttons** — `HOME (G28)`, `SET Z 0`, `START`, `CANCEL`, `FIRMWARE
   RESTART`.
4. **Emergency STOP** — full-width red, sends Moonraker `emergency_stop`,
   always visible.
5. **Move** — X/Y/Z jog buttons (→ `G91`/`G1` relative moves) for first-contact
   alignment.
6. **Live feed** — websocket (or poll) on Moonraker events for temps + position.

All actions map 1:1 to Moonraker endpoints:
| UI action | Moonraker call |
|---|---|
| Start | `{"method":"print.start"}` or `gcode script: M100`/`SD_CARD_PRINT` |
| Stop | `{"method":"print.cancel"}` |
| E-STOP | `{"method":"emergency_stop"}` |
| Temp +/- | `gcode script: M104 S…` / `M140 S…` |
| Home | `gcode script: G28` |
| Joystick | `gcode script: G91\nG1 X… F…` |
| Read temps/pos | `{"method":"objects.query","params":{"objects":{"toolhead":null,"extruder":null,"heater_bed":null}}}` + `events.subscribe` |

## 5. How to deploy to the SoC (no eMMC mutation)

- Build the app **cross** (or on an identical ARM box), producing one
  self-contained binary/script + any `.so` it needs.
- Push to the SoC's **RAM** or a USB stick (not eMMC) so the stock rootfs
  stays pristine: e.g. `/tmp/ad4ui/` or a mounted stick at `/mnt`.
- Launch it with the **stock env** (copy the `QT_QPA_PLATFORM`, `TSLIB_*`,
  `QWS_MOUSE_PROTO`, `LD_LIBRARY_PATH` block from the stock app — it's in the
  rootfs `dmesg`/env dump). A wrapper `ad4ui-start.sh` sets those and runs the
  app.
- **Kill the stock `adventurer4-arm`** first (it holds `/dev/fb0` and
  `event0`): `pkill -f adventurer4-arm`, then start `ad4ui`. This is the same
  "stop vendor, run ours" pattern as `ad4-bridge.sh`.
- Recovery: kill `ad4ui`, `AD4_RESTART_VENDOR=1 ad4-bridge.sh stop` → stock UI
  returns. Nothing on eMMC changed.

## 6. Next concrete steps (to build the UI)

1. Confirm Moonraker is up on the host and reachable from the SoC:
   `curl -s http://<host>:7125/printer/info` from the SoC.
2. Write `ad4-klipper-ui/`:
   - `ad4ui.py` (PyQt4) **or** `ad4ui.cpp` (Qt4 C++) — the panel above.
   - `ad4ui-start.sh` — sets the stock Qt4/tslib/fb env, kills vendor app, runs UI.
   - `Makefile` (C++ path) or `requirements` (PyQt4 path).
3. Cross-compile / bundle, push to SoC `/tmp/ad4ui/`, run `ad4ui-start.sh`.
4. Iterate on the screen against a live `G28`/`M104`/`print`.

> Build decision needed from you: **Option A (Qt4, reuse rootfs — recommended)**
> vs **Option B (SDL/pygame framebuffer)**. I'll scaffold the chosen one once
> the host is reachable from the SoC.
