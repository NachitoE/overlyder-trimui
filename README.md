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

- Volume, brightness, contrast, exposure, saturation, colour temperature and the LED brightness
- Mute, quit the game, reboot and shut down
- No patches or setup inside the game
- Trigger, hold time and language (Spanish, English, Portuguese) set from the app

## Install

Grab the zip from the Releases page, unzip it and copy what's inside into `Apps/` on your SD card. Then open **OverLyder** in CrossMix and enable the overlay.

If you're on the device over SSH, run `sh install.sh` as root from the repo root. `uninstall.sh` takes it off again. Both scripts back up your config first.

## Use

| | |
| --- | --- |
| Open the panel | hold <kbd>Home</kbd> for about 700 ms |
| Move around | D-pad or stick |
| Change a value | left/right |
| Close it | <kbd>B</kbd> or <kbd>Home</kbd> |

You can tweak the trigger and timing from the app.

## License

MIT. See [LICENSE](LICENSE). The bundled Material Icons font is Apache 2.0, and its license lives in `app/resources/LICENSE-MaterialIcons.txt`.
