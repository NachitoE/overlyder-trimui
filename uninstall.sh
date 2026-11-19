#!/bin/sh
# Uninstall OverLyder from CrossMix (run on the device as root).
set -e

if [ "$(id -u)" != "0" ]; then
    echo "This script must run as root."
    exit 1
fi

APP=/mnt/SDCARD/Apps/OverLyder
ETC=/mnt/SDCARD/System/etc
STARTS=/mnt/SDCARD/System/starts
HOOK=$STARTS/overLyder.sh
LEGACY_APP=/mnt/SDCARD/Apps/OverlaySwitch
LEGACY_HOOK=$STARTS/overlayswitch.sh
STAMP=$(date +%Y%m%d-%H%M%S)

if [ -x "$APP/overlayctl" ]; then
    "$APP/overlayctl" stop || true
fi

# keep a copy of the configuration before removing it
if [ -f "$ETC/overlay.cfg" ]; then
    cp -a "$ETC/overlay.cfg" "$ETC/overlay.cfg.uninstalled.$STAMP"
    echo "Config backup: $ETC/overlay.cfg.uninstalled.$STAMP"
fi

rm -rf "$APP" "$LEGACY_APP"
rm -f "$HOOK" "$LEGACY_HOOK"
rm -f "$ETC/overlay.cfg" "$ETC/overlay.disabled"
rm -f /tmp/overlayd.pid /tmp/overlay_stopped.pids

sync
echo "OverLyder uninstalled."
