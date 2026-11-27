# Global frame limiter

The official TrimUI firmware has no system-wide FPS cap (see
[`FRAME_LIMITER_RESEARCH.md`](FRAME_LIMITER_RESEARCH.md)). OverLyder adds one
with a small `LD_PRELOAD` shim that paces the moment a game presents a frame.
It works for **emulators and PortMaster games** because it is loaded into every
game process, not into a particular launcher.

It is deliberately separate from the FPS counter: the counter is read-only and
has no performance controls.

## 1. What it does

A cap of **5, 10, … 60 fps** (steps of 5) sleeps until the next frame deadline
before each present, so frames are evenly spaced. **No cap** is the default.
On this device the panel runs at 60 Hz
(`/sys/class/disp/disp/attr/sys` → `ref_fps:60`), so 30 and 20 are whole
divisions of the refresh rate and pace most cleanly; other values still work
but may not align perfectly with vsync.

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

With the experimental switch **off** there is **no `/etc/ld.so.preload` entry
at all**: `install.sh` only copies the library into the app, and `overlayd`
removes the line when the limiter is disabled (also on daemon exit). User
processes are completely untouched while the feature is off.

When the switch is enabled, `overlayd` adds exactly one line:

```
/mnt/SDCARD/Apps/OverLyder/lib/liboverlyder_pace.so
```

The library itself lives **inside the app**. Other preload entries are kept;
`uninstall.sh` removes only our line (and the file if it becomes empty). No
emulator script, PortMaster file or system setting is modified. A restart of
the app (or the device) re-reads the config.

The shim is a no-op unless both are true:

1. `/tmp/overlyder_fps_limit` contains `5`–`60` (written by `overlayd` from
   `overlay.cfg` while the app is enabled and the experimental switch is on);
   and
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

The limiter is **experimental** and starts disabled. In the CrossMix app there
is a **FPS limiter (experimental)** toggle (`fps_limit_enabled=`). While it is
off, the limiter row is **hidden** in the in-game overlay and no marker is
written. Turning it on from the app makes the row appear.

In the **in-game overlay** (hold the hotkey), **Limitador FPS** is a slider
below **Contador FPS**:

```
5 fps ← … ← 60 fps → Sin límite (no cap)
```

Left/Right move in 5 fps steps; rightmost position is **No cap** (the default).
The value is shown on the right of the row. The SD write is deferred until the
panel closes. `overlay.cfg` stores:

```
fps_limit_enabled=0   # experimental switch, set from the app
fps_limit=0           # 0 = no cap, else 5..60 in steps of 5
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

### 5.3 Experimental flag and slider

With `fps_limit_enabled=0`, 11 D-pad downs landed on `sel=12 kind=1 action=1`
(**Continuar**) and pressing A closed the panel: the limiter row did not exist.
Toggling **FPS limiter (experimental)** in the app wrote `fps_limit_enabled=1`.

With the flag on, 11 downs reached the slider (`sel=11`). Left/Right through
the values gave:

```
no cap → 60 → 55 → 60 → no cap      (config after close: fps_limit=0, no marker)
no cap → 60 → 55 → 50               (config after close: fps_limit=50, marker=50)
```

A binary doing 200 presents with marker `50` measured **48.74 fps**; the shim
accepts any 5–60 value, not just 30/20. The panel stayed open while changing
the slider and the marker appeared after it closed (`overlayd` is blocked in
`run_overlay()` while the panel is up).

### 5.4 Disabled = zero impact (verified)

With `fps_limit_enabled=0` / `fps_limit=0`:

* `overlayd` removes `/etc/ld.so.preload` completely and no marker exists.
* An instrumented 200,000-call `ioctl` loop ran at **1.83 M/s** with the old
  always-injected shim versus **2.05 M/s** with no preload at all — a small but
  real cost just from interposing `ioctl`.
* The design changed accordingly: the preload line is now added/removed by
  `overlayd` together with the marker. With the limiter off there is **no
  injection at all**, so every process uses the untouched system path.
* The shim was also simplified: the marker is read **once in the constructor**
  (`g_target`), so an injected-but-unconfigured shim would do one integer test
  per present — no `clock_gettime`, no file access, no extra syscalls. With a
  marker, the test programs still paced at **30.08** and **20.07 fps**.

### 5.5 Budokai Tenkaichi 3 (PortMaster) — not supported

Tested with the limiter at 30 and the marker active. The shim **was mapped**
into the game, but the cap had no effect:

* `bt3` is a Dolphin decomp (`ET_EXEC`) under `/mnt/SDCARD/Data/ports/bt3/bt3`.
  It imports `dlsym`, `dlopen`, `syscall`, `SDL_GL_SetSwapInterval` and
  `SDL_GL_SwapWindow`, but **no** `eglSwapBuffers`, `SDL_RenderPresent`,
  `glXSwapBuffers`, `ioctl` or DRM symbols.
* With temporary instrumentation, `SDL_GL_SwapWindow` was called **once** at
  startup; no `eglSwapBuffers`/`SDL_RenderPresent` call occurred in gameplay.
* It renders directly to `/dev/dri/renderD128`. The busiest ioctl there
  (`0xc0206440`) ran ~3180/s (~50 per frame) — a low-level PVR command
  submission, not a present. No ioctl ran once per frame; `syscall()` was not
  used for ioctls.
* Interposing `dlsym` to catch functions resolved from a specific `libEGL`
  handle crashed the game with SIGSEGV, so that route was discarded.

**Conclusion:** this port uses a private PVR/DRM path that never reaches the
hooked APIs. A generic `LD_PRELOAD` shim cannot cap it without per-port work.
Realistic options are a port-specific hook or a coarse CPU-frequency cap
(less heat/battery, but no smooth pacing). Temporary diagnostics were removed.

## 6. Limitations / risks

* With the limiter enabled, `ld.so.preload` affects every dynamically linked
  process. The shim is tiny, has no SDL dependency, and with no cap configured
  it is a single integer test per present. With the limiter disabled the line
  is removed entirely. If the library were missing while the line exists, the
  loader would warn but still run programs; uninstall removes both.
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
