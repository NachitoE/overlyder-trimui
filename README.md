<p align="center">
  <img src="app/icon.png" alt="OverLyder" width="260">
</p>

<h1 align="center">OverLyder</h1>

<p align="center">
  Quick settings for the TrimUI Smart Pro on CrossMix.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-2F80ED.svg" alt="License: MIT">
  <img src="https://img.shields.io/badge/TrimUI%20Smart%20Pro-CrossMix-2F80ED.svg" alt="Runs on TrimUI Smart Pro with CrossMix">
</p>

<p align="center">
  <img src="repo/overlyder.gif" alt="OverLyder in action">
</p>

Hold the Home button (or a combo you choose) and a Switch-style overlay slides over whatever you're playing. The game doesn't need to support anything. OverLyder freezes it and draws the panel over the screen. Close it and you land right back where you were.

## Features

- Volume, brightness, contrast, exposure, saturation, colour temperature, LED brightness and LED color (static RGB, restored at boot)
- Mute, quit the game, reboot and shut down
- Virtual keyboard: opens from the overlay, sends real key presses to the app underneath and returns to the panel with B/Home
- No patches or setup inside the game
- Trigger, hold time, language (Spanish, English, Portuguese, from JSON files) and the SIGSTOP/layer mode set from the app
- Optional persistent, read-only FPS counter: FPS, frametime, CPU/GPU usage, temperatures and frequencies in any corner (simple text or window), configured from the overlay
- Optional global frame limiter (5–60 fps in steps of 5, or no cap) for emulators and ports, hidden behind an experimental toggle in the app; no game files or clocks are changed
- Built-in updater in the app: shows the installed version and checks GitHub releases for a newer one

## Install

Grab the zip from the Releases page, unzip it and copy what's inside into `Apps/` on your SD card. Then open **OverLyder** in CrossMix and enable the overlay.

If you're on the device over SSH, run `sh install.sh` as root from the repo root. `uninstall.sh` takes it off again. Both scripts back up your config first.

## Use

| | |
| --- | --- |
| Open the panel | hold <kbd>Home</kbd> for about 700 ms |
| Move around | Up/Down on the D-pad or stick |
| Change a value / pick WiFi or BT | left/right (they never move the row selection) |
| Close it | <kbd>B</kbd> or <kbd>Home</kbd> |

### FPS counter

Hold the hotkey to open the panel, choose **Contador FPS / FPS counter** and
press <kbd>A</kbd>. The submenu (inside the overlay) turns it on, picks the
corner, toggles **Simple** (just the texts side by side, no window — default)
or **Window**, and selects which read-only rows to show (FPS, frametime,
CPU/GPU usage, temperature, frequencies, battery). The HUD stays on screen
while you play, drawn on a second hardware display layer; it never changes
game settings. See [`docs/FPS_HUD.md`](docs/FPS_HUD.md).

### Frame limiter

First enable **FPS limiter (experimental)** in the OverLyder app; only then the
**Limitador FPS** row appears in the panel. It is a slider: Left/Right move in
5 fps steps from 5 to 60, and the far right is **No cap** (default). It paces
frame presentation for emulators and ports with a small LD_PRELOAD library
installed inside the app. **While the switch is off there is no
`/etc/ld.so.preload` entry at all**, so nothing is injected into other
processes and their FPS are untouched. It is separate from the read-only
counter and never overclocks or edits game files. See
[`docs/FRAME_LIMITER.md`](docs/FRAME_LIMITER.md).

### Virtual keyboard

Pick **Keyboard** in the panel and press <kbd>A</kbd>, or press the dedicated shortcut (currently **L2 + R2**) to open only the keyboard without showing the panel first. In that keyboard-only session, **B** (or Home / Start / OK) simply closes the keyboard and returns to the game — the panel is never shown. When the keyboard is opened from the panel, B still returns to the panel. The shortcut is remappable in the app's settings screen: choose **Keyboard shortcut** and press the button combination (B finishes; B alone disables it). **L2/R2 (LT/RT)** are supported as shortcut keys too (analog triggers pressed past halfway). It is stored as `kbd_trigger=` in `overlay.cfg`; `l2`/`lt` and `r2`/`rt` names are also accepted in that file.

There are two keyboard modes, selected in the app's settings screen (**Keyboard mode**: Compact / Full) or with `keyboard_mode=` in `overlay.cfg`:

- **Compact** (default): a small floating keyboard (720×248) that can be moved anywhere on screen with the **right stick** (clamped to the screen bounds). The game stays fully visible and characters go straight into the app — there is no input box.
- **Full**: the original full-screen keyboard with a text field and a dimmed background.

| | |
| --- | --- |
| Move between keys | D-pad or stick |
| Type / activate a key | <kbd>A</kbd> |
| Back to the overlay | <kbd>B</kbd> or <kbd>Home</kbd> |
| Shift / symbols | <kbd>Y</kbd> / <kbd>X</kbd> |
| Backspace / confirm | <kbd>Select</kbd> / <kbd>Start</kbd> |
| Move compact keyboard | right stick (left stick still moves the key cursor) |
| Open keyboard directly | hold <kbd>Select</kbd> + <kbd>L1</kbd> (remappable) |

Keys are injected through a virtual keyboard device created at daemon startup, so games can enumerate it like a physical keyboard. Going back always returns to the same overlay screen with the Keyboard row focused. In compact mode the typed text is still saved to `/tmp/overlyder.keyboard.txt` as a side channel.

The panel and keyboard are drawn on their own alpha-blended hardware display
layer, so the game keeps running and nothing fights over `/dev/fb0` (no
SIGSTOP flicker). If no free layer is available, OverLyder falls back to the
old freeze-the-game behaviour. Even in that SIGSTOP mode, opening the keyboard
resumes the game and switches the overlay to a hardware layer for as long as
the keyboard/overlay is up, so typing is live. The last typed text is also
saved to `/tmp/overlyder.keyboard.txt`.

### SIGSTOP or no SIGSTOP

Both modes ship in the same binaries. You can switch with the
**Pause game when overlay opens (SIGSTOP)** row in the app's settings screen
(**Pausar juego al abrir overlay (SIGSTOP)** in Spanish): **Sí** freezes the game while the panel is up,
**No** keeps it running on a hardware layer. (Internally this is `no_sigstop`
in `/mnt/SDCARD/System/etc/overlay.cfg`):

- `no_sigstop=1` (default) — draw on a hardware display layer; the game is
  never stopped. Automatically falls back if no layer can be claimed.
- `no_sigstop=0` — classic behaviour: freeze the game and MainUI with SIGSTOP
  while the panel is up.

`OVERLAY_NO_LAYER=1` forces the fallback for a single run, and
`OVERLAY_DEBUG=1` logs input and layer decisions to `/tmp/overlayd.log`.

The reverse-engineering, device measurements and pitfalls behind the layer
approach are documented in [docs/NO_SIGSTOP_OVERLAY.md](docs/NO_SIGSTOP_OVERLAY.md).

You can tweak the trigger and timing from the app.

## Languages

All UI strings are flat JSON files in `app/lang/` (`es.json`, `en.json`,
`pt.json`), installed to `Apps/OverLyder/lang/`. See
[docs/LANGUAGES.md](docs/LANGUAGES.md) for the format, loading order, fallback
and how to add a language.

## Build from source (Windows host)

`scripts/build-win.ps1` cross-compiles with Zig 0.14.1 for
`aarch64-linux-gnu.2.33` and produces `dist/OverLyder` plus `dist/bin`:

```powershell
.\scripts\build-win.ps1
```

It looks for SDL2 / SDL_ttf headers under `%TEMP%\sdl` (auto-detects
`SDL2-* / include` and `SDL2_ttf-* / include`) and for the aarch64 shared
libraries in `%TEMP%\libs` as `libSDL2.so` / `libSDL2_ttf.so`. Get those two
`.so` files from `/usr/trimui/lib/` on the device (`libSDL2-2.0.so.0` and
`libSDL2_ttf-2.0.so.0`) and rename/copy them there. Every path can be
overridden with `-Zig`, `-SdlInc`, `-TtfInc`, `-SdlLib`, `-Target` and `-Out`.

On Linux/macOS, `scripts/build-docker.sh` does the same with Docker but needs
a `third_party/` bundle (`third_party/SDL/include`, `third_party/SDL_ttf`,
`third_party/libs`).

## Updates

The app presents an updater, which pulls directly from the releases in this github page.

## License

MIT. See [LICENSE](LICENSE). The bundled Material Icons font is Apache 2.0, and its license lives in `app/resources/LICENSE-MaterialIcons.txt`.
