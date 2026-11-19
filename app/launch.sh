#!/bin/sh
# Entry point used by CrossMix when the app is opened from the menu.
APP=/mnt/SDCARD/Apps/OverLyder
export LD_LIBRARY_PATH="/mnt/SDCARD/System/lib:/usr/trimui/lib:$LD_LIBRARY_PATH"

"$APP/bin/overlay_ui" --config

# Re-enable the daemon with the new settings (does nothing if disabled).
"$APP/overlayctl" restart
