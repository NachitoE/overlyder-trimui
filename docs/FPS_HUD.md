# Persistent FPS / performance HUD

The settings panel only exists while the hotkey is held. The FPS counter is a
small **resident process** (`fpshud`) that keeps a read-only overlay visible
during normal play. It never changes a clock, a game setting or any other
system state — everything it shows is a read, plus a count of framebuffer page
flips.

**The configuration submenu lives in the in-game overlay**, not in the
CrossMix app: open the panel with the hotkey, choose **Contador FPS / FPS
counter** and press A. The panel stays open while you change values.

This document covers the design, the configuration keys, where every number
comes from, and the validation performed on the real device on 2026-11-27.
The frame-limiter research is kept separate in [`FRAME_LIMITER_RESEARCH.md`](FRAME_LIMITER_RESEARCH.md).

## 1. Architecture

```
overlayd (daemon, boot)
 ├── fork → fpshud       (only when fps_hud=1; dies with the daemon)
 └── fork → overlay_ui   (only while the hotkey panel is open)
```

* `overlayd` forks `fpshud` at startup with `prctl(PR_SET_PDEATHSIG, SIGTERM)`.
  A daemon restart after a settings change therefore always re-spawns the HUD
  with the new configuration.
* `fpshud` claims display **channel 2**, fb0 **page 14** (720×1280 buffer).
  The settings panel keeps its existing preference for **channel 3**, page 15.
  This is implemented by `overlay_layer_open_ex()` in `src/overlay_layer.c`;
  the old two-argument `overlay_layer_open()` keeps the panel behaviour.
* `overlayd` writes `/tmp/overlyder_overlay_open` whenever the panel is up (in
  **both** SIGSTOP and layer mode). `fpshud` watches that flag — and also
  `/tmp/overlay_layer_active` — and **hides** its layer until the panel closes,
  so no HUD ever draws on top of the settings panel.
* `fpshud` writes `/tmp/overlyder_hud.pid` and `/tmp/overlyder_hud.status`
  while alive. Both are removed on exit. They are diagnostic aids, not needed
  by the UI.
* It also writes `/tmp/overlyder_hud_layer` (`<pid> <ch>`). Like the panel's
  `LAYER_FLAG`, this lets the next instance disable a layer left behind by a
  `SIGKILL` (`clear_stale_flag()` in `overlay_layer.c`).
* `fpshud` is added to `overlayd`'s `is_system_daemon()` skip list, so it is
  never in the SIGSTOP/kill target set.

### Rendering

A 1280×720 RGBA canvas is drawn with SDL_ttf (the same font as the panel:
`/mnt/SDCARD/System/resources/DejaVuSans.ttf`), then rotated into the layer's
720×1280 portrait geometry exactly like `layer_flip()` in `overlay_ui.c`. The
display engine composites it over the live game. Redraw is 4 Hz. Simple style
draws only the text line(s); window style draws the rounded translucent box.

## 2. Configuration

Open the in-game panel → **Contador FPS** row → A. `A` toggles/cycles, `B`
goes back to the panel. Every change is auto-saved to
`/mnt/SDCARD/System/etc/overlay.cfg`.

Because the panel is a child of `overlayd`, changing a value there must **not**
restart the daemon (PDEATHSIG would close the panel). Instead the UI touches
`/tmp/overlyder_hud_sync`; `overlayd` re-reads the config and respawns/stops
the HUD after the panel closes, while the running `fpshud` re-reads the config
each cycle so style/position/fields update live. Changes made from the
CrossMix app still use `overlayctl restart`.

| Submenu row | Config key | Values |
| --- | --- | --- |
| Activated | `fps_hud` | `0` off (default), `1` on |
| Position | `fps_hud_pos` | `0` top-left, `1` top-right, `2` bottom-left, `3` bottom-right |
| Style | `fps_hud_style` | `0` **simple** (default): just the selected texts side by side, no window; `1` window: translucent box with one row per metric |
| FPS / Frametime / CPU usage / GPU usage / CPU temp / CPU freq / GPU freq / Battery | `fps_hud_fields` | bit mask: 1, 2, 4, 8, 16, 32, 64, 128. Default `31` (first five) |

In simple mode the tokens are short (`FPS 60  16.7ms  CPU 12%  GPU 5%  TMP
45C`) with a 1 px dark shadow for legibility, wrapped to at most 3 lines if
too many fields are enabled. It is the default so the game is not covered by
a permanent box.

The counter is deliberately **read-only**: no clocks, no frame cap and no
overclock controls live in this submenu. The frame limiter is a separate
**Limitador FPS** row in the panel (see [`FRAME_LIMITER.md`](FRAME_LIMITER.md)).

## 3. Where each number comes from

| Row | Source | Notes |
| --- | --- | --- |
| FPS | `FBIOGET_VSCREENINFO` on `/dev/fb0`, polled every 2 ms for 250 ms; count `yoffset` changes | Emulators/games on this firmware present by panning fb0. A flip is one presented frame. Renderers that never pan fb0 will show `--`. |
| FRAME | exponential moving average of the time between two flips | Window-limited; converges over a few updates. |
| CPU | `cpu` line of `/proc/stat`, `100 * (1 - idle_delta/total_delta)` | All cores aggregated, sampled between redraws. |
| GPU | `GPU Utilisation: N%` in `/sys/kernel/debug/pvr/status` | Real PowerVR counter. The earlier approach (sampling `idle:on/off` in `sunxi_gpu/dump`) always read 0 because that flag tracks the power domain, not the GPU. |
| TEMP | `/sys/class/thermal/thermal_zone0/temp` ÷ 1000 | `cpu_thermal_zone` on this device. Zones 1 and 2 (GPU/DDR) are also read into the status file. |
| CPUFRQ | `scaling_cur_freq` of `policy0` ÷ 1000 | Read only; the HUD never writes it. |
| GPUFRQ | `Frequency:` from the same sunxi_gpu dump | Read only. |
| BATT | `/sys/class/power_supply/axp2202-battery/capacity` | Percentage. |

Measured thermal zones on the test unit: `cpu_thermal_zone`,
`gpu_thermal_zone`, `ddr_thermal_zone`, `axp2202-battery`. The stock OSD uses
the battery PMIC temperature, the HUD uses the CPU zone.

## 4. Validation (device 192.168.100.13, 2026-11-27)

Build: `scripts/build-win.ps1`, Zig 0.14.1 → `aarch64-linux-gnu.2.33`,
no warnings. Binaries copied to `/mnt/SDCARD/Apps/OverLyder/bin/`.

### 4.1 Startup and layer ownership

With `no_sigstop=0`, `fps_hud=1`, `fps_hud_fields=255`:

```
overlayd starting ...
fpshud started (pid 11793)
layer canvas 1280x720 -> layer geometry 720x1280
disp layer ch2/l0 page=14 phys=0xff138000 720x1280 z=20
fpshud started (ch=2 page=14, pos=0, fields=0xff)
```

`/sys/class/disp/disp/attr/sys` showed channel 1 (UI, z16) and channel 2
(z20) enabled; the HUD page contained 67,872 non-zero bytes. The status file
reported plausible values (`cpu=5.6 gpu=0.0 cputemp=45 gputemp=46
ddrtemp=46 cpumhz=1200 gpumhz=700 batt=67`).

### 4.2 FPS accuracy against a known source

A synthetic panner (`tools/fakegame.c` with `usleep(33000)`) was run on the
device. An instrumented copy measured its real rate independently:

```
flips=300 seconds=14.953 rate=20.06 fps
```

While it was running the HUD reported `fps=19.85`, `frame=46.42 ms` and then
`fps=23.80`, `frame=50.28 ms`. The counter tracks the actual presentation rate.

> Note: on this device a 33 ms `usleep` plus the ioctl/draw work results in
> ~20 fps, not 30. The HUD measured what really happened.

An early version divided the flip count by the *whole* iteration time
(sampling + compose + sysfs reads), which under-reported by ~30 %. Fixed by
dividing only by the 250 ms polling window.

### 4.3 SIGSTOP mode (`no_sigstop=0`)

Hotkey injected with `vpad` bound via `OVERLAY_INPUT_DEV`:

```
15:19:48 stopped pid 10749            (MainUI)
15:19:48 opening overlay ..., layer=0
```

The HUD process stayed alive and kept writing its status file while the panel
was open. Closing the panel with B:

```
15:19:51 ui exit code 0
15:19:51 resumed 1 process(es)
```

### 4.4 Layer / no-SIGSTOP mode (`no_sigstop=1`)

Before: channel 2 enabled (HUD, `0xff138000`). With the panel open:

```
-- layer flag --    12185 3
-- layers --        BUF enable ch[1] ... z[16]
                    BUF enable ch[3] ... z[20] addr[ff4bc000]
```

Channel 2 was hidden, the panel used channel 3 / page 15. After closing:

```
-- layer flag --    (none)
-- layers --        ch[1] z16 + ch[2] z20 addr[ff138000]
```

No framebuffer conflict and no SIGSTOP was used for the game.

### 4.5 Submenu in the in-game overlay

The panel now has 15 rows: Volume … Keyboard, **Contador FPS**, Continue …
Power, plus the WiFi/BT strip. `ROW_H` was reduced from 32 to 30 so the strip
and footer still fit. Verified with `vpad` (FIFO, not `tail -f`, so no
duplicated D-pad events):

```
10 D-pad downs → activate sel=10 kind=1 action=6   (ACT_FPSHUD)
A → submenu opens, overlay_ui still alive
A → fps_hud=1, panel stays open (no daemon restart)
down + A → fps_hud_pos=0 → 1
down + A → fps_hud_style=0 → 1
B → back to panel (overlay_ui still alive)
B → panel closes, fpshud starts with the new settings
```

Two issues found and fixed during this test:

1. Restarting the daemon from an overlay row killed the panel through
   `PR_SET_PDEATHSIG`. Replaced by the `HUD_SYNC_FILE` hot-sync described in
   §2.
2. The first `tail -f` based input harness duplicated some D-pad presses;
   using a FIFO gave exactly 10 events for 10 presses.

### 4.6 Position and style rendering

Bounding box of non-transparent pixels in the HUD layer buffer (720×1280):

| Mode | Pixels | Bounding box |
| --- | --- | --- |
| Window, top-right (`pos=1`) | 47,376 | `(24,24)-(211,275)` |
| **Simple**, top-right (`pos=1`) | 4,020 | `(21,16)-(38,534)` |
| Window, bottom-left (`pos=2`) | 61,488 | `(452,1004)-(695,1255)` |

Simple mode uses ~8 % of the window mode's pixels: a thin (~18 px) line of
text instead of a box. The boxes match the rotated canvas coordinates for
each corner.

### 4.6b HUD hidden while the panel is open

With `fps_hud=1`:

* **SIGSTOP mode** (`no_sigstop=0`): before opening, ch2 (HUD) enabled; while
  the panel was open, `/tmp/overlyder_overlay_open` existed and only ch1
  (UI) was enabled; after closing, ch2 was enabled again.
* **Layer mode** (`no_sigstop=1`): while the panel was open, ch3/page 15 was
  the panel and ch2 was hidden; after closing, ch2 returned. No HUD ever drew
  over the panel.

### 4.7 Cleanup

`overlayctl stop` removed the daemon and the HUD, disabled channel 2, and
deleted `/tmp/overlyder_hud.pid`, `/tmp/overlyder_hud.status` and
`/tmp/overlyder_hud_layer`.

A `kill -9` of `fpshud` left channel 2 enabled; restarting the daemon logged
`cleared stale layer on ch2 (pid 13039 gone)` and the new HUD reclaimed the
channel cleanly.

### 4.7 GPU counter, navigation and close fixes

* `/sys/kernel/debug/pvr/status` reports a real `GPU Utilisation` line. Sampling
  it over six seconds gave `2, 4, 4, 3, 3, 3 %` from the kernel and `3, 3, 3,
  4, 4, 4 %` in the HUD status file — it now tracks actual GPU use instead of
  always showing 0.
* Left/Right on the D-pad no longer navigates the overlay list. On an action
  row, 11 Downs + Left/Right + A still reported `activate sel=12 kind=1
  action=1` (Continue) and closed the panel; Left/Right only changes sliders
  (including the 5 fps limiter steps) or selects WiFi vs Bluetooth.
* The close bug was caused by **any** key release resetting the 600 ms trigger
  grace, so a B/Home press after a D-pad or volume interaction was ignored.
  Now only the configured trigger key's release starts that grace, and the
  grace applies only to that key. Tested 9 open/close cycles in both modes
  (SIGSTOP and layer): D-pad→B, volume→Home, and immediate B all closed the
  panel every time.

## 5. Limitations

* FPS is derived from fb0 page flips. A renderer that presents through a
  different path (e.g. a pure DRM/dmabuf path) will show `--`; this is a
  read-only observer, there is no hook into the game.
* GPU usage is the kernel's own `GPU Utilisation` counter from PowerVR debugfs.
  It updates ~4 Hz with the HUD; brief spikes may be missed.
* The HUD needs a free display channel. If `trimui_osdd` or another app owns
  channels 2 and 3, `fpshud` exits and logs `no free hardware layer` to
  `/tmp/overlayd.log`; the panel still works (it falls back to SIGSTOP).
* The HUD writes to a spare fb0 page (14). An emulator using nearly all ~18
  pages could collide; the same caveat as for the panel's page 15 in
  [`NO_SIGSTOP_OVERLAY.md`](NO_SIGSTOP_OVERLAY.md).

## 6. Files changed for this feature

* `src/fpshud.c` (new), `src/overlay_layer.c/.h`,
  `src/overlayd.c`, `src/overlay_ui.c`, `src/util.c`, `src/common.h`
* `app/lang/{keys,es,en,pt}.json` + regenerated `src/lang_keys.h`
* `app/defaults/overlay.cfg`, `install.sh`, `uninstall.sh`,
  `scripts/build-win.ps1`, `scripts/build-docker.sh`
