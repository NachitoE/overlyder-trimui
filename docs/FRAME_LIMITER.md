# Global frame limiter

The official TrimUI firmware has no system-wide FPS cap (see
[`FRAME_LIMITER_RESEARCH.md`](FRAME_LIMITER_RESEARCH.md)). OverLyder adds one
with a small `LD_PRELOAD` shim that paces the moment a game presents a frame.
It works for **emulators and PortMaster games** because it is loaded into every
game process, not into a particular launcher.

It is deliberately separate from the FPS counter: the counter is read-only and
has no performance controls.

## 1. What it does

A cap of **30** or **20 fps** sleeps until the next frame deadline before each
present, so frames are evenly spaced. On this device the panel runs at 60 Hz
(`/sys/class/disp/disp/attr/sys` → `ref_fps:60`), so 30 and 20 are whole
divisions of the refresh rate and pace cleanly.

`Off` (default) does nothing at all: no sleeps, no extra work beyond a cached
check of a marker file.

Uses:

* smooth out a game that runs at an unstable 40–60 fps by holding a steady 30;
* save battery / reduce heat in games that do not need 60 fps.

**Caveat (important):** a cap below a game's own rate changes its speed when
the game advances its simulation per frame rather than by elapsed time. This
is the same limitation documented by `fpsvita`. `Off` is always safe; try 30
then 20 per game. There is no per-game profile storage in this version.

## 2. How it is injected

`install.sh` writes exactly one line to `/etc/ld.so.preload`:

```
/mnt/SDCARD/Apps/OverLyder/lib/liboverlyder_pace.so
```

The library itself lives **inside the app**. `install.sh` preserves other
entries and backs the file up; `uninstall.sh` removes only our line (and the
file if it becomes empty). No emulator script, PortMaster file or system
setting is modified. A restart of the app (or the device) re-reads the config.

The shim is a no-op unless both are true:

1. `/tmp/overlyder_fps_limit` contains `30` or `20` (written by `overlayd` from
   `overlay.cfg` while the app is enabled); and
2. `/proc/self/exe` is under `/mnt/SDCARD/` but **not** under
   `/mnt/SDCARD/Apps/OverLyder/` (i.e. a game, not our UI/daemon/firmware).

`OVERLYDER_PACE_TEST=<fps>` forces pacing for any process; it exists only for
the test harness.

## 3. Hooked functions

| Symbol | Used by |
| --- | --- |
| `eglSwapBuffers` | EGL / Vulkan renderers, many standalone ports |
| `glXSwapBuffers` | GLX ports |
| `SDL_RenderPresent` | SDL2 software renderer (common in ports and some emulators) |
| `SDL_GL_SwapWindow` | SDL2 OpenGL windows |
| `ioctl(fd, FBIOPAN_DISPLAY, …)` on `/dev/fb0` | emulators that present by panning the framebuffer |

The real symbol is resolved with `dlsym(RTLD_NEXT, ...)` and always called.
The pacing algorithm uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` to
a rolling deadline (`period = 1/target`), clamps accumulated debt after a long
stall, and is thread-safe with an atomic compare-and-swap on the deadline.

## 4. Configuration

In the **in-game overlay** (hold the hotkey): a new **Limitador FPS** row below
**Contador FPS**. Press A to cycle:

```
Off  →  30 fps  →  20 fps  →  Off
```

The current value is shown on the right of the row. `config_save()` writes it
to `/mnt/SDCARD/System/etc/overlay.cfg`:

```
fps_limit=0     # 0 = off, 30 or 20
```

Because the panel is a child of `overlayd`, changing it there must not restart
the daemon. As with the HUD, the UI touches `/tmp/overlyder_hud_sync`; when the
panel closes the daemon updates `/tmp/overlyder_fps_limit`. Games launched
afterwards pick the new cap at process start; a game already running keeps the
rate it was started with until relaunched.

## 5. Validation (device, 2026-11-27)

### 5.1 Install / uninstall

`sh install.sh` on a device with no `ld.so.preload`:

```
Installing app...
Installing autostart hook...
Registering frame limiter shim...
started
```

Afterwards `/etc/ld.so.preload` contains the single app-owned line, and
`/proc/self/maps` of an ordinary shell process contains
`liboverlyder_pace.so` — the system still works normally. `uninstall.sh`
removed the line and the app directory; the shim was no longer mapped.
Reinstall restored everything.

### 5.2 Pacing accuracy

Two instrumented test programs (200 presents each, no artificial delay) were
placed under `/mnt/SDCARD/Apps/OverLyderTest/` so they pass the game-path gate.
One calls `FBIOPAN_DISPLAY`, the other calls `SDL_RenderPresent` through
`dlsym(RTLD_DEFAULT, ...)`:

| Marker | `FBIOPAN_DISPLAY` | `SDL_RenderPresent` |
| --- | --- | --- |
| none (Off) | 60.35 fps | 389,638 fps (uncapped) |
| `30` | **30.06 fps** | **30.08 fps** |
| `20` | **20.04 fps** | **20.07 fps** |

With marker `30` but the same binary copied to `/tmp` (not a game path), the
rate stayed at 59.69 fps: the path gate works.

### 5.3 Overlay row

Driven through `vpad` with a FIFO (11 D-pad presses → `activate sel=11
kind=1 action=7`):

```
after A#1: fps_limit=30
after A#2: fps_limit=20
after A#3: fps_limit=0
after A#4: fps_limit=30
...
after panel close: marker=30
```

The panel stayed open during the changes and the marker appeared only after it
closed (`overlayd` is blocked in `run_overlay()` while the panel is up).

## 6. Limitations / risks

* `ld.so.preload` affects every dynamically linked process on the device. The
  shim is intentionally tiny, has no SDL dependency, and returns immediately
  when the marker is absent; if the library were missing, the loader would
  print warnings but still run programs. Keep the library installed whenever
  the pointer line exists (uninstall removes both).
* Statically linked games cannot be intercepted.
* Only the four swap symbols above are hooked. A game using a different
  present path (e.g. a raw DRM path) will not be capped.
* The cap applies at process start. Changing the setting while a game is
  already running requires relaunching the game.
* A cap is not an emulator setting: for frame-count-dependent games the safe
  choice is `Off`.

## 7. Files changed for this feature

* `src/pace_shim.c` (new), `src/common.h`, `src/util.c`, `src/overlay_ui.c`,
  `src/overlayd.c`
* `scripts/build-win.ps1`, `scripts/build-docker.sh`, `install.sh`,
  `uninstall.sh`, `app/updater.sh`, `app/defaults/overlay.cfg`
* `app/lang/{keys,es,en,pt}.json` + regenerated `src/lang_keys.h`
