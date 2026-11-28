# Input device order (`event3`)

Our virtual keyboard is a **fake input device**. At boot it used to be created
before the firmware's gamepad, so it took `/dev/input/event3`. CrossMix
hardcodes that path as the gamepad, which broke **Menu + Power** force-exit.

## The short version

Linux names button sources `/dev/input/event0`, `event1`, ... The number is
just the order of creation: whoever asks first gets the lowest free number.

On the console at boot, two services create input devices:

| Service | Device |
| --- | --- |
| `trimui_inputd` (firmware) | "TRIMUI Player1" — the real gamepad, has Menu |
| `overlayd` (us) | "OverLyder Virtual Keyboard" — fake keyboard, no Menu |

What happened:

```
overlayd starts first  → keyboard claims event3
trimui_inputd starts   → gamepad gets event4
```

CrossMix's `button_state.sh` asks:

```sh
evtest --query /dev/input/event3 1 316   # is Menu held?
```

It was asking our keyboard, which has no Menu key. Answer: always "no".
So `kill_apps.sh` treated Menu + Power as Power alone and shut down instead
of force-closing the game. Cizia64's VirtualKeyboard creates a fake keyboard
too, so it hit the same race.

## The fix

`src/overlayd.c` now **waits for the real gamepad by name** before creating the
fake keyboard:

```
wait for "TRIMUI Player1" → gamepad claims event3 → keyboard gets event4
```

The keyboard still exists and typing is unchanged; we just no longer cut in
line. The old `event3` fallback remains as a timeout safety net.

## Verify

Need a **reboot** after installing: device numbers are assigned at creation and
do not move until the system restarts. After reboot, check the mapping:

```sh
grep -E 'Name=|Handlers=' /proc/bus/input/devices
```

`event3` should say "TRIMUI Player1". Then, with Home held:

```sh
evtest --query /dev/input/event3 1 316; echo $?
```

`10` = pressed, `0` = released. If it returns `10`, force-exit works again.

Note: the firmware's shortcut listener (`thd /dev/input/event3`) used the same
hardcoded path, so those shortcuts were affected by this too.
