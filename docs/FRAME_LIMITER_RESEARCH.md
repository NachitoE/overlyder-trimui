# Frame limiter research (deferred)

This document collects the research that led to the implementation. The
read-only FPS HUD came first; the frame limiter that follows from this research
is now implemented in [`FRAME_LIMITER.md`](FRAME_LIMITER.md). The accepted
direction was a global swap-pacing shim (`LD_PRELOAD`) so it works for **both
emulators and PortMaster games**, injected with a single app-owned
`/etc/ld.so.preload` line (see §5).

## 1. Question

Does the official TrimUI firmware already provide a frame-rate limiter we can
reuse, and if not, what is the least invasive way to cap frames on the Smart Pro
to smooth out games or save battery?

## 2. What the official repositories actually contain

* <https://github.com/trimui/firmware_smartpro> — firmware releases; the git
  tree at `main` contains **only `README.md`**.
  (<https://api.github.com/repos/trimui/firmware_smartpro/git/trees/main?recursive=1>)
* <https://github.com/trimui/assets_smartpro> — SD base package; also **only
  `README.md`** in git; content ships as release ZIPs.

Release notes (raw READMEs) mention only these performance-related items:

| Version | Note (verbatim) |
| --- | --- |
| firmware 1.0.2 | "CPU dynamic clock in MainUI and games.", "Fix Main UI high CPU usage issue." |
| SD patch 20231115 | "CPU dynamic clock in each Emus, named 'cpufreq.sh'. a. In most Emus, cpu clock will lock 1.008GHz in minimal. b. In 3D and big Emus, cpu clock will go performance governer." |
| SD base 20231125 | "Disable NDS frameskip, lock CPU-freq to 1.6GHz, modify keymapping." |
| SD base 20240413 | "Fallback PPSSPP CPU clock setting to default ..." |
| firmware 1.0.3 | "Fn key editor. multi-seletion for cpu mode/LED/Silent/..." and "add 1.8GHz cpufreq option" |

No global FPS cap, frame limiter, vsync toggle or frame-pacing control appears
in any official note.

A check of the extracted stock rootfs (v1.1.1, `trimui_tg5040.awimg`, ext4 at
offset `0x1141c00`) found:

* `/usr/trimui/osd/widgets/`: `static_cpu_freq`, `static_temperature`,
  `slider_backlight`, etc. — **no FPS widget**.
* `/usr/trimui/bin/sdlteartest` has `render fps %d` strings: a diagnostic
  tearing/FPS program, not a user setting.
* `trimui_osdd` has `canvasfps`, `fps = %.1f`, `print_fps` — instrumentation of
  the OSD daemon's own canvas, not game FPS.
* No script writes a refresh-rate or vsync sysfs node.

**Conclusion (fact): the official firmware has no system-wide frame limiter.**

## 3. What does exist (and what we can reuse)

### 3.1 CPU performance/power (not frame pacing)

Stock firmware:

```sh
# /usr/trimui/bin/preload.sh
echo ondemand  > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
echo 408000    > /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq
echo 2000000   > /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq
```

Fn-key editor scripts on the SD card:

* `com.trimui.cpusave.sh`: `ondemand`, min 1008000, max 1200000 (low power).
* `com.trimui.cpuperformance.sh`: `performance`, min/max 2000000.

CrossMix (`/mnt/SDCARD/System/usr/trimui/scripts/cpufreq.sh`) uses frequency
IDs 0…8 = 408/600/816/1008/1200/1416/1608/1800/2000 MHz and accepts
`interactive|ondemand|performance|powersave|conservative`, e.g.:

```sh
cpufreq.sh ondemand 1 5       # 600–1416 MHz, all cores
cpufreq.sh powersave 0 2      # 408–816 MHz
```

These can reduce heat/battery use but they do **not** produce even frame
spacing; a game that drops below the target simply runs slower. They are not a
substitute for a limiter whose purpose is smooth frame pacing.

### 3.2 Emulator-level frame controls (not universal)

* RetroArch `retroarch.cfg` (CrossMix base): `video_vsync = "true"`,
  `video_swap_interval = "1"`, `video_refresh_rate = "60.000000"`,
  `video_frame_delay = "0"`, `fps_show = "false"`.
  `video_swap_interval` can hold a frame for 2/3 vsyncs (~30/~20 fps), but
  this only covers RetroArch cores.
* PPSSPP `ppsspp.ini`: `FrameSkip`, `AutoFrameSkip`, `FrameRate` (0 = default).
  Again, standalone emulator only.
* `Emus/**/*.sh` launchers (263 of 286 found in the installed system) source
  `common_launcher.sh` / `load_launcher.sh`; PortMaster ports do **not**, so a
  launcher-only solution would miss ports.

### 3.3 Telemetry available on the device (used by the HUD)

| Metric | Node |
| --- | --- |
| CPU temp | `/sys/class/thermal/thermal_zone0/temp` (`cpu_thermal_zone`) |
| GPU temp / DDR temp | thermal zones 1 / 2 |
| Battery temp | zone 3 or `/sys/class/power_supply/axp2202-battery/temp` (÷10) |
| CPU freq / governor | `/sys/devices/system/cpu/cpufreq/policy0/*` |
| GPU state | `/sys/kernel/debug/sunxi_gpu/dump` → `idle:on;|idle:off;`, `Frequency:700MHz` |
| CPU usage | `/proc/stat` (aggregate delta) |
| Display refresh | `/sys/class/disp/disp/attr/sys` → `ref_fps:60`, `fps:60.9` |
| FPS | count `/dev/fb0` `yoffset` changes (see `FPS_HUD.md`) |

There is **no GPU devfreq node** on this firmware (`/sys/class/devfreq` is
empty); GPU usage can only be estimated from the debugfs idle flag.

## 4. Real projects that solve this elsewhere

* **`esp1esp1/fpsvita`** (<https://github.com/esp1esp1/fpsvita>) — PS Vita
  plugin with exactly the UX shape requested here: an overlay showing frame
  rate, frametime and CPU time (Mini in any corner / Full box), a frame limit
  of Default / 60 / 30 / 20 switched live, and a separate 40/45/50 limiter.
  It documents the key caveat: a game that ties its simulation to frame count
  will run fast/slow at a non-native limit. This is the main UX reference for
  the FPS counter and the limiter's value list.
* **`endjynn/frame-pacer`** (<https://github.com/endjynn/frame-pacer>) —
  Linux FPS limiter + HUD using `LD_PRELOAD` shims for GLX/EGL/Vulkan/Proton,
  with a config file for global/per-game limits. This is the technical model
  for a global swap-pacing shim on the Smart Pro (intercept `eglSwapBuffers`,
  `SDL_RenderPresent`, `glXSwapBuffers`, and fb `FBIOPAN_DISPLAY`, then sleep
  to the next frame deadline with `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`).
* **`danklammer/MinUI-Zero`** — a performance-focused MinUI fork that lowers
  clocks in a closed loop using the *measured game frame rate*. Different
  goal (find the lowest stable clock) but reinforces that measuring frames is
  the right signal; it does not expose a frame cap.
* **Eden/BananaNative `hle_fpslock.c`** and Dolphin's `FrameLimiter` — show
  the classic accumulator/pacing algorithm and the caveat that whole-vsync
  limits (60/30/20) map cleanly to a 60 Hz panel while 40/45 do not.

## 5. Injection options and the current constraint

The shim has to be loaded by **every game process**. Findings from the device:

* `/mnt/SDCARD/System/starts/*.sh` hooks are **executed**, not sourced
  (`for f in ...; do $f; done` in `/usr/trimui/bin/runtrimui.sh`). Exporting
  `LD_PRELOAD` from the app-owned `overLyder.sh` hook therefore does **not**
  reach MainUI or the games it launches.
* `/etc/ld.so.preload` does not exist on this firmware; the root fs is an
  overlay and writable. A single pointer line to a library **inside the app**
  would cover all dynamically linked games (emulators and ports) and can be
  removed on uninstall/disable. The shim would no-op for non-game processes
  (check `/proc/self/exe`, gate on a runtime marker file).
* Patching emulator launch scripts or PortMaster's `control.txt` would work
  for some targets but edits other apps' files — rejected by the user's
  constraint that the feature stay contained in OverLyder.
* A CPU-profile-only approach is safe but does not smooth frames — rejected.

**Status:** the single `ld.so.preload` pointer was accepted, the FPS HUD was
reviewed first, and the limiter was then implemented following that plan.

## 6. Decisions taken

1. One app-owned `/etc/ld.so.preload` line, created by `install.sh` and
   removed by `uninstall.sh`; other entries are preserved.
2. Cap values: **Off / 30 / 20** — whole divisions of the device's 60 Hz
   panel. 40/45 were left out because they cannot be evenly paced on 60 Hz;
   the fpsvita caveat about frame-count-dependent games is documented.
3. The shim is a Zig-built `liboverlyder_pace.so` (`-shared -fPIC`) and was
   verified with two instrumented programs (fbdev panning and
   `SDL_RenderPresent`) plus an install/uninstall round trip — see
   `FRAME_LIMITER.md` §5. Testing against a real RetroArch core and a
   PortMaster port is still desirable.
4. Static binaries and other present paths cannot be intercepted; this is
   documented as a limitation rather than worked around.

## 7. Sources

* <https://github.com/trimui/firmware_smartpro> and
  <https://raw.githubusercontent.com/trimui/firmware_smartpro/main/README.md>
* <https://github.com/trimui/assets_smartpro> and
  <https://raw.githubusercontent.com/trimui/assets_smartpro/main/README.md>
* <https://github.com/cizia64/CrossMix-OS> (`cpufreq.sh`, `retroarch.cfg`,
  PPSSPP configs, OSD CPU presets)
* <https://github.com/guiopen/tsp-optimization> (community notes on stock
  per-emulator `cpufreq.sh` and Fn-key overrides)
* <https://github.com/esp1esp1/fpsvita>
* <https://github.com/endjynn/frame-pacer>
* <https://github.com/danklammer/MinUI-Zero>
* On-device inspection over SSH (`192.168.100.13`, kernel 4.9.191, 2026-11-27)
