# Virtual keyboard + no-SIGSTOP overlay — research and draft

> The definitive write-up of the **no-SIGSTOP layer technique** (device
> measurements, pitfalls, mode switching) is
> [NO_SIGSTOP_OVERLAY.md](NO_SIGSTOP_OVERLAY.md). This file keeps the keyboard
> implementation notes and the research trail.

Status: **implemented and validated on device** (CrossMix 4.9.191, 2026-11-26).
The hardware-layer backend, keyboard and key injection all work; only the
subjective visual check (panel/keyboard look correct on the panel) is left to
the user. Findings below marked `[device]` were measured on the real unit.

Code added/changed: `src/overlay_layer.c/.h` (disp2 layer + `DISP_MEM_REQUEST`),
`src/textinject.c/.h`, keyboard view inside `src/overlay_ui.c`, conditional
SIGSTOP in `src/overlayd.c`, `no_sigstop=1` in the default config, and both
build scripts.

## 1. Why the overlay flickers without SIGSTOP (root cause)

`src/overlayd.c` sends `SIGSTOP` to the game process tree and `MainUI`, then
`overlay_ui` captures `/dev/fb0`, writes its canvas to **page 0** and calls
`FBIOPAN_DISPLAY` with `yoffset = 0` (`fb_flip()` in `src/overlay_ui.c`).

On Allwinner-based TrimUI units `/dev/fb0` is a *multi-buffered, panned*
framebuffer:

- `ytctl` in `NVNTKNN/brick-tube` documents fb0 as `virtual 1024x16384` on the
  Brick (≈21 buffers) and says the visible buffer sits at a **live y-offset**
  read with `FBIOGET_VSCREENINFO`; drawing to buffer 0 lands off-screen.
- The game/emulator keeps panning (RetroArch, etc.). `overlay_ui` keeps forcing
  the display back to its own page. Two processes panning/writing the same
  framebuffer is the "fight": the screen alternates between game and overlay.
- `loksundar/carplay-mipi-dsi-monitor` notes that on sunxi drivers two
  concurrent panners can even wedge the G2D IRQ until reboot.

So: **the flicker is a framebuffer ownership problem, not an input problem.**
SIGSTOP is a blunt way to win ownership. The VirtualKeyboard repo provides no
magic here — it is a plain SDL2 fullscreen app (`SDL_CreateWindow` +
`SDL_GetWindowSurface`, no `/dev/disp`, no layering, no signals). It simply takes
the whole framebuffer, which is why it cannot coexist with a running game.

## 2. Clue: the stock OS already draws overlays over a *running* game

`trimui_osdd` (stock/CrossMix OSD daemon, binary in CrossMix-OS at
`System/usr/trimui/osd/trimui_osdd`) does exactly what we need. Symbols and
strings in that binary:

- opens `/dev/disp`, `/dev/fb0`, `/dev/g2d`
- `trimui_init_layer`, `trimui_set_layer`, `trimui_set_hlayer`,
  `trimui_show_hlayer`, `trimui_osd_init_layers`, `disp2_ui_transparent`
- logs `====> video layer:%d %d` and `====> video zorder:%d, alpha_mode:%d, alpha:%d`
- `rotate_90_to_mem`, `fb_smem`, `g_vfb_video`, `g_vfb_msg`, `g_vfb_osd`

Disassembly of `trimui_osdd` (ARM64) shows:

- ioctl `0x48` = `DISP_LAYER_GET_CONFIG`, ioctl `0x47` = `DISP_LAYER_SET_CONFIG`
  (Allwinner display2 / BSP 4.9 structs — same ones used by `brick-tube`'s
  `rectfix.c`). Args are `unsigned long a[4] = { screen, &cfg, count, 0 }`.
- `trimui_osd_init_layers` calls `trimui_set_hlayer(0, 8, 720, 1280, 8, 0)` and
  `trimui_set_hlayer(1, 12, 720, 1280, 12, 2)`. Since the helpers split the
  second argument as `channel = n / 4`, `layer_id = n % 4`, these are
  **channel 2 / layer 0** and **channel 3 / layer 0** — i.e. hardware layers
  beyond the normal channel 0 (video) and channel 1 (UI/fb0).
- `trimui_set_layer` builds a `disp_layer_config` with `zorder`,
  `alpha_mode`, `alpha_value`, `screen_win`, `fb.addr[0]`, `fb.size[]`, and
  `enable = 1`, then calls ioctl `0x47`.
- `brick-tube/rectfix.c` confirms the exact v1/v2 struct layouts and that
  the vendor player's video layer is channel 0 while the UI fb is channel 1.

Conclusion: the A133 display pipeline can composite extra alpha-blended
layers on top of whatever the game is drawing into fb0. **That is the clean
way to bypass SIGSTOP**: our overlay/keyboard becomes its own layer instead of
fighting the game for page 0.

## 3. Proposed design

### 3.1 UI / UX

Two in-process views inside `overlay_ui`:

| View | Enter | Exit |
| --- | --- | --- |
| Panel (current drawer) | hotkey | B/Home → back to game |
| Keyboard (new) | select the **Keyboard** row in the panel | B/Menu → back to **panel**; SELECT/OK → commit text and back to panel |

Opening the keyboard simply stops drawing the panel ("temporarily hides the
overlay"); it does not close the daemon or resume/quit the game. The same
input device stays grabbed (`EVIOCGRAB`) while either view is up.

Keyboard control mapping (adapted from `cizia64/virtualkeyboard`, but with
B remapped to Back because B is "go back" in OverLyder):

- D-pad / stick: move key cursor
- A: type the selected key
- Y: backspace
- X: space
- L / R: move caret
- L2 / R2: switch keyset (lower/symbol ↔ upper/symbol) or jump to row edges
- START: Enter / commit and return to the panel (matches the CrossMix fork of
  VirtualKeyboard, which moved confirm from SELECT to START)
- SELECT: backspace shortcut
- B / Home: Back to panel (cancel, no commit)

Layout: 4×13 keys, two UTF-8 keysets, text field with blinking caret — same
model as the reference keyboard. Rendering uses the existing
`cached_surface`/`draw_text`/`fill_round_a` helpers, so no new SDL dependency.

The typed string is kept in `g_keyboard_text`. Delivery to the game is a
separate, explicit action (see question to user): inject as key events after
the overlay closes, using evdev writes / a small uinput keyboard. `sendkey`
(cizia64) proves direct `write()` to `/dev/input/eventX` works on this device;
`trimui_inputd` creates the "TRIMUI Player1" virtual device games actually
read.

### 3.2 Rendering backends

Abstract `fb_flip()`/`fb_restore()` behind a target:

1. **`FB` backend (today, fallback):** capture fb0, draw opaque panel, pan to
   our page. Requires SIGSTOP. Used when `no_sigstop=0` or when the disp-layer
   probe fails.
2. **`LAYER` backend (goal):** open `/dev/disp`, claim a free hardware layer,
   draw ARGB8888 into its buffer with `alpha_mode = 2` / configurable
   `alpha_value`, zorder above the game layer. No fb capture, no panning, no
   SIGSTOP, game keeps running behind a translucent dim. `[verify on device]`

New `src/overlay_layer.c/.h` holds the disp2 structs + ioctl wrappers
(`disp_layer_get_config`, `disp_layer_set_config`). Only ~150 lines, modelled
on `brick-tube/rectfix.c`.

### 3.3 Process / signal changes

`overlayd.c`:

- `stop_targets()` / `resume_targets()` are conditional on `no_sigstop`
  (default 1, with automatic fallback to the old SIGSTOP path if no layer
  can be claimed).
- When `no_sigstop=1`, don't stop anything; pass `OVERLAY_NO_SIGSTOP=1` to the
  UI. `kill_game()` still needs to SIGCONT/kill the game tree explicitly.
- Crash recovery (`STOPPED_FILE`) stays for the FB backend.

`overlay_ui.c`:

- `g_view` state (`PANEL`/`KEYBOARD`), keyboard model + input cases, translations
  for ES/EN/PT for the new row and footer.
- Keep the trigger-grace logic so B/Home from the keyboard doesn't immediately
  reopen the overlay.

### 3.4 Files to touch

- `src/common.h`, `src/overlayd.c`, `src/overlay_ui.c`, `src/util.c`
- new `src/overlay_layer.c`, `src/overlay_layer.h` (or `src/keyboard.c/.h` if we
  split the model out)
- `scripts/build-win.ps1`, `scripts/build-docker.sh` (add sources)
- `app/defaults/overlay.cfg` + `cfg_load()` (`no_sigstop`, optional `keyboard`)
- `README.md` (controls table)
- no new binary/resource is required if the keyboard is in-process; the
  reference VirtualKeyboard binary is *not* shipped.

## 4. Alternatives considered (and why not first choice)

1. **Draw to the currently displayed fb buffer** (read `yoffset` before every
   paint, as `ytctl` does). Works only if nobody else pans; with a running
   emulator it still flickers.
2. **Pin the display to a spare fb page.** The emulator's own
   `FBIOPAN_DISPLAY` immediately overrides it.
3. **LD_PRELOAD shim in the game** that ignores `FBIOPAN_DISPLAY` while the
   overlay is open. Robust-ish but requires patching CrossMix launcher env,
   fails for static binaries, and is fragile across OS updates. Kept as a
   fallback if hardware layers turn out to be unavailable.
4. **Keep SIGSTOP and inject all text on OK.** Fully functional but the game
   cannot show live typing feedback, and it does not meet the "no sigstop"
   goal. Useful as an interim milestone (Phase 1 below).

## 5. Delivery plan

**Phase 1 (can do without SSH):** in-process keyboard view + panel row +
translations, working under the existing SIGSTOP backend; text buffered and
injected on overlay close. Low risk, immediately usable.

**Phase 2 (needs SSH):** probe `/dev/disp`, find a free layer + buffer, implement
the LAYER backend, flip `no_sigstop=1`, verify: no flicker in RetroArch and
MainUI, live typing visible, clean restore on B/Home, no stale layer after
crash.

## 6. Device validation results (2026-11-26)

- `[device]` fb0: `1280x720` visible, `yres_virtual=13107` (~18 pages),
  `line_length=5120`, `smem_start=0xfc000000`.
- `[device]` UI layer: channel 1 / layer 0, enabled, zorder 16, alpha_mode 1,
  window 720x1280 (rotated), physical address inside fb0 memory.
- `[device]` Channels 4–7 return 0 from both GET and SET but **ignore writes**.
  Only channels 2 and 3 are real. OverLyder therefore claims channel 3 (or 2)
  and verifies the config by reading it back after enabling.
- `[device]` `DISP_MEM_REQUEST`/`DISP_MEM_GETADR` returns an address the display
  engine cannot safely use — enabling a layer with it reset the device. The
  backend now uses a **spare fb0 page** (page 15 of 18) instead; validated with
  a synthetic game continuously panning pages 0/1: the layer stays enabled at
  zorder 20 and our page retains the overlay pixels while the game keeps
  running. No SIGSTOP, no framebuffer fight.
- `[device]` `/dev/disp` mmap only maps MMIO at offset 0; `/dev/mem` is needed
  for arbitrary physical buffers (no longer needed by the final backend).
- `[device]` Keyboard: the virtual keyboard device is created by overlayd at
  startup (so games can enumerate it before they launch) and inherited by the
  UI through `OVERLAY_KBD_FD`. evtest confirms real `KEY_1`/`KEY_ENTER`
  events. Typing "112" produced
  `/tmp/overlyder.keyboard.txt` with `112`. B returns to the panel and a
  second B closes the overlay, disabling the layer and removing the flag file.
- `[device]` Layer geometry must match the live UI layer: **720x1280**
  (portrait, hardware-rotated to the panel), not the fb var's 1280x720. Using
  1280x720 produced a visibly glitched image. The canvas is rotated 90 degrees
  into the layer page (page 15), written at the UI layer's 2880-byte stride.
  `alpha_mode=2`, `alpha_value=255`, zorder 20 (above the UI layer at 16).
  User confirmed the panel renders clearly with this geometry.

## 7. Remaining checks (need a human looking at the screen)

- Visual: with a real game running, confirm the panel and keyboard appear in
  the right place, are not stretched/rotated, and the game shows through the
  dim without flicker. (Automated checks already confirmed the layer stays at
  zorder 20 over the live game's panning fb0 pages.)
- Confirm the injected `KEY_*` events land in a real game's text-entry field.
  The temporary uinput keyboard is verified to emit the events, but each app
  enumerates input devices differently. `OVERLAY_TEXT_TO_GAMEPAD=1` makes
  OverLyder also replay them onto the device the game already reads (A/B test).
- If `trimui_osdd` is enabled, it occupies channels 2/3; with no other real
  channels available OverLyder falls back to the SIGSTOP backend.
- `no_sigstop=0` in `/mnt/SDCARD/System/etc/overlay.cfg` restores the old
  freeze-the-game backend instantly, useful as a control comparison.
