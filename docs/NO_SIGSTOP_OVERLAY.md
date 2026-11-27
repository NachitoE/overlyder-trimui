# Drawing the OverLyder overlay without `SIGSTOP`

This document explains why the original overlay needed `SIGSTOP`, how the
no-SIGSTOP mode works on the TrimUI Smart Pro (Allwinner A133, kernel 4.9.191),
what was measured on the real device, and how to choose between the two modes.
It is written so the technique can be reused in other TrimUI/CrossMix projects.

## 1. The problem: two writers fighting over `/dev/fb0`

The classic implementation (still available as a fallback) does this:

1. `overlayd` finds the game process tree and `MainUI` and sends `SIGSTOP`.
2. `overlay_ui` captures `/dev/fb0`, draws its panel into a private canvas,
   copies it to **page 0**, and calls `FBIOPAN_DISPLAY` with `yoffset = 0`.
3. On close it restores the captured image and sends `SIGCONT`.

`SIGSTOP` is only there to stop the other process from drawing. Removing it
without changing anything else makes the screen flicker: the game keeps
panning/flipping `/dev/fb0` and OverLyder keeps forcing the display back to
its own page, so the two images alternate on screen. This is a framebuffer
*ownership* problem, not an input problem.

Relevant facts about fb0 on this device (measured):

| Property | Value |
| --- | --- |
| visible resolution | 1280 x 720 |
| `yres_virtual` | 13107 lines (about 18 full pages of 720 lines) |
| `line_length` (stride) | 5120 bytes (1280 px * 4) |
| bits per pixel | 32 (ARGB8888, format code 0) |
| `smem_start` | `0xfc000000`, `smem_len` 64 MiB |
| game panning | `yoffset` alternates 0 / 720 / … |

`NVNTKNN/brick-tube`'s `ytctl` independently documents the same design: fb0 is
heavily multi-buffered and the visible buffer is at a *live* `yoffset` that
must be read with `FBIOGET_VSCREENINFO`; drawing to page 0 lands off-screen.
The `carplay-mipi-dsi-monitor` notes that on sunxi drivers two concurrent
panners can even wedge the G2D IRQ until reboot.

## 2. The solution: a hardware display layer

Allwinner's display2 driver (`/dev/disp`) can composite several independent
layers. The stock firmware already uses this: `trimui_osdd` (the stock/CrossMix
OSD daemon) draws toasts and sliders over a running game without freezing it.
Reverse-engineering that binary gave the ioctls and struct layout:

- `0x48` = `DISP_LAYER_GET_CONFIG`
- `0x47` = `DISP_LAYER_SET_CONFIG`
- arguments: `unsigned long args[4] = { screen, &disp_layer_config, count, 0 }`
- `disp_layer_config` (184 bytes on this kernel) contains `mode`, `zorder`,
  `alpha_mode`, `alpha_value`, `screen_win`, `fb.addr[0]`, `fb.size[0]`,
  `fb.crop`, `enable`, `channel`, `layer_id`.
- The exact C structs are in `src/overlay_layer.c`; they match the public
  `sunxi_display2.h` from Allwinner BSP 4.9 and `brick-tube/rectfix.c`.

Measured layer map on this unit:

| Channel | Layer | State | Notes |
| --- | --- | --- | --- |
| 0 | 0 | normally off | video/scaler layer |
| 1 | 0 | **enabled**, zorder 16 | UI layer = what games/MainUI draw into fb0 |
| 2, 3 | 0 | free (used by `trimui_osdd` when enabled) | real hardware layers |
| 4–7 | 0 | GET/SET return 0 but **ignore writes** | phantom; unusable |

`src/overlay_layer.c` therefore claims channel 3, then 2, and **verifies** the
write by reading the config back after enabling it. If neither is free (e.g.
`trimui_osdd` is running), OverLyder falls back to the SIGSTOP path.

The persistent FPS HUD (`fpshud`, see [`FPS_HUD.md`](FPS_HUD.md)) uses the
*other* channel: it prefers 2 and page 14, while the panel keeps 3 / page 15.
`overlay_layer_open_ex()` implements that preference. While the panel is up it
publishes `LAYER_FLAG`, and the HUD hides its layer until the panel closes, so
only one extra layer is enabled at a time.

The overlay is drawn into its own layer at **zorder 20** (above the UI layer)
with `alpha_mode = 2` (global × pixel alpha) and `alpha_value = 255`. The
canvas itself carries per-pixel alpha: a translucent black dim over the game
and opaque panel/keyboard shapes. The game keeps running and panning fb0; the
display engine composites our layer on top. Nothing fights for page 0.

### 2.1 Where the layer buffer lives — important pitfall

The obvious allocation path, `DISP_MEM_REQUEST (0x2c0)` +
`DISP_MEM_GETADR (0x2c2)`, returns a value that looks like a physical address
above 2 GiB. Enabling a layer pointing at it **reset the device** — the display
engine cannot safely read that memory. `ioctl()` also returns `int`, so such
addresses arrive sign-extended and need masking to `uint32_t` before use.
`/dev/disp` mmap on this firmware only maps MMIO at offset 0; `/dev/mem` can
map arbitrary physical addresses but is not needed by the final design.

The stable solution is to use a **spare page of fb0's own virtual framebuffer**
as the layer buffer:

- pick page 15 of ~18 (far away from the pages the game pans: 0, 1, …),
- physical address = `smem_start + page * yres * line_length`
  (`0xfc000000 + 15*720*5120 = 0xff4bc000`),
- size used = `720 * 1280 * 4 = 3,686,400` bytes = exactly 720 fb lines, i.e.
  one whole fb page, so it never overlaps another page,
- write the layer pixels through a normal `/dev/fb0` mmap at that page offset.

Verified with a synthetic game (`tools/fakegame.c`) continuously panning pages
0/1: the game kept running, the layer stayed enabled at zorder 20, and page 15
kept the overlay pixels. No flicker, no SIGSTOP.

### 2.2 Geometry: match the UI layer, not the framebuffer var

This was the cause of a visibly glitched image. The fb var reports 1280x720,
but the **UI layer is configured as 720x1280** (portrait) and the display
controller rotates it onto the landscape panel. A layer configured as 1280x720
is composited incorrectly.

`overlay_layer_open()` reads channel 1 / layer 0 with `GET_CONFIG` and copies
its `fb.size[0]` into our layer. On this device that is 720x1280. `layer_flip()`
in `overlay_ui.c` rotates the 1280x720 canvas 90 degrees into that buffer.
With this change the user confirmed the panel renders clearly and in the right
orientation.

## 3. Choosing between SIGSTOP and no-SIGSTOP

Both modes are in the same binaries; selection is via
`/mnt/SDCARD/System/etc/overlay.cfg`, or from the **Pause game when overlay
opens (SIGSTOP)** row in the CrossMix app settings: **Sí** = freeze the game (classic), **No** = keep it
running on a hardware layer. The app writes the file and restarts the daemon
on exit. Note the config key is named `no_sigstop`, so it has the opposite
polarity to the visible option: `no_sigstop=1` shows as "SIGSTOP: No".

```
# 1 = hardware layer, game keeps running (default)
# 0 = original behaviour: freeze game and MainUI with SIGSTOP
no_sigstop=1
```

Behaviour:

- `no_sigstop=1` (default): `overlayd` probes `/dev/disp` for a free layer
  before opening. On success it does **not** send `SIGSTOP`; the overlay UI
  draws to the layer. On failure it logs `hardware layer unavailable,
  falling back to SIGSTOP` and uses the classic path automatically.
- `no_sigstop=0`: always the classic path. Useful for A/B comparison and on
  firmwares where no hardware layer is available.
- `OVERLAY_NO_LAYER=1` in the environment forces the fallback for one run.
- `OVERLAY_DEBUG=1` enables per-event logging in `/tmp/overlayd.log`.

`STOPPED_FILE` (`/tmp/overlay_stopped.pids`) is only written in SIGSTOP mode
and is used by the UI's "Exit game" action and crash recovery. In layer mode a
separate `TARGETS_FILE` (`/tmp/overlay_targets.pids`) publishes the game PIDs
so "Exit game" still works without ever freezing them.

`LAYER_FLAG` (`/tmp/overlay_layer_active`) records `<pid> <channel>` so the
next instance can disable a layer left behind by a `SIGKILL`. Normal close and
the `SIGTERM` handler both disable the layer and remove the flag.

## 4. Keyboard integration (why the layer matters)

The keyboard is an in-process view inside `overlay_ui.c`:

- a new **Keyboard** row in the panel opens it;
- while it is open the panel is simply not drawn ("temporarily hidden"), and
  B/Home returns to the panel rather than closing the overlay;
- D-pad/stick moves the cursor, A types, Y = Shift, X = symbols, SELECT =
  backspace, START = Enter/commit;
- keycodes use the real QWERTY layout (not alphabetical — `KEY_A=30`,
  `KEY_B=48`, …);
- typed text is also saved to `/tmp/overlyder.keyboard.txt`.

Text delivery is through a uinput keyboard named **`OverLyder Virtual
Keyboard`**. It is created by `overlayd` at daemon startup (not when the
keyboard opens) and inherited by the UI via `OVERLAY_KBD_FD`, so games that
enumerate input devices at launch can see it. `evtest` on the device confirmed
real `KEY_1` / `KEY_ENTER` events and correct text output. If a particular app
does not pick up the new keyboard, `OVERLAY_TEXT_TO_GAMEPAD=1` additionally
replays the same events onto the device the game already reads.

This is where no-SIGSTOP matters: with the game frozen, typed characters could
only be injected after the overlay closed and live feedback in the game's text
field would be impossible. With the hardware layer the game keeps running and
receives the keys while they are typed.

## 5. Diagnostic tools

`tools/` contains the read-only/probe programs used on the device (not installed
with the app):

| Tool | Purpose |
| --- | --- |
| `dispprobe.c` | dump all layer configs and `DISP_MEM` slots |
| `colorlayer.c` | enable a COLOR-mode layer on a channel (safe test) |
| `layeroff.c` | disable a layer by channel |
| `fblayertest.c` | memory layer backed by a spare fb0 page |
| `fakegame.c` | continuously pan fb0 pages 0/1 |
| `pagedump.c` | print sample pixels of any fb0 virtual page |
| `fbinfotest.c` | dump fb fix/var info |
| `mmaptest.c` | compare `/dev/disp` and `/dev/mem` mmap behaviour |
| `layertest.c`, `capturetest.c` | earlier experiments (DISP_MEM / capture) |

Cross-compile with the Zig toolchain used by `scripts/build-*.ps1` / `.sh`,
copy to `/tmp` on the device and run as root.

## 6. Validation performed (2026-11-26)

- Layer probe: channels 2/3 real, 4–7 phantom; verified by GET-after-SET.
- No-SIGSTOP open logged `using hardware layer, game keeps running`, layer
  channel 3 / zorder 20 / 720x1280 at `0xff4bc000`.
- Synthetic game panning fb0 pages 0/1 while overlay open: game kept running,
  layer stayed enabled, overlay page retained its pixels.
- Keyboard opens from the panel, uinput device present, real key events seen by
  `evtest`, text file correct.
- B from keyboard returns to panel; B from panel closes overlay; layer
  disabled and flag removed on exit.
- Visual check by the user: panel renders clearly after matching the UI layer
  geometry.

## 7. Caveats / future work

- Only channels 2 and 3 exist on this firmware. If `trimui_osdd` is enabled it
  uses them, so OverLyder falls back to SIGSTOP. A future version could
  coordinate with `trimui_osdd` or use the `DISP_LAYER_SET_CONFIG2` (0x49)
  dmabuf-fd path instead of physical addresses.
- `DISP_CAPTURE_*` did not work with the structs used here, so visual
  verification of compositing required a human looking at the panel.
- Page selection uses page 15 of ~18; an emulator that uses nearly every fb
  page could still collide. The page is chosen at open time away from the
  current `yoffset`.
- The fallback remains important: it keeps the app usable on any firmware
  where disp layers are unavailable or occupied.
