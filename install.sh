#!/bin/sh
# Install OverLyder into CrossMix (run on the device as root, e.g. via SSH).
set -e

if [ "$(id -u)" != "0" ]; then
    echo "This script must run as root."
    exit 1
fi

SRC=$(cd "$(dirname "$0")" && pwd)
APP=/mnt/SDCARD/Apps/OverLyder
ETC=/mnt/SDCARD/System/etc
STARTS=/mnt/SDCARD/System/starts
CFG=$ETC/overlay.cfg
DISABLE=$ETC/overlay.disabled
HOOK=$STARTS/overLyder.sh
STAMP=$(date +%Y%m%d-%H%M%S)

for f in "$SRC/dist/bin/overlayd" "$SRC/dist/bin/overlay_ui" "$SRC/dist/bin/fpshud" \
         "$SRC/dist/lib/liboverlyder_pace.so" \
         "$SRC/app/config.json" "$SRC/app/manifest.json" "$SRC/app/icon.png" "$SRC/app/launch.sh" \
         "$SRC/app/overlayctl" "$SRC/app/overLyder.start" "$SRC/app/updater.sh" \
         "$SRC/app/resources/MaterialIcons-Regular.ttf" \
         "$SRC/app/resources/LICENSE-MaterialIcons.txt" \
         "$SRC/app/lang/es.json" "$SRC/app/lang/en.json" "$SRC/app/lang/pt.json"; do
    if [ ! -f "$f" ]; then
        echo "Missing file: $f"
        exit 1
    fi
done

mkdir -p "$APP/bin" "$APP/lib" "$APP/defaults" "$APP/resources" "$APP/lang" "$ETC" "$STARTS"

echo "Backing up existing configuration..."
for f in "$CFG" "$DISABLE" "$HOOK"; do
    if [ -e "$f" ]; then
        cp -a "$f" "$f.bak.$STAMP"
        echo "  $f.bak.$STAMP"
    fi
done

echo "Installing app..."
cp -f "$SRC/app/config.json" "$APP/config.json"
cp -f "$SRC/app/manifest.json" "$APP/manifest.json"
cp -f "$SRC/app/icon.png" "$APP/icon.png"
cp -f "$SRC/app/launch.sh" "$APP/launch.sh"
cp -f "$SRC/app/overlayctl" "$APP/overlayctl"
cp -f "$SRC/app/overLyder.start" "$APP/overLyder.start"
cp -f "$SRC/app/updater.sh" "$APP/updater.sh"
cp -f "$SRC/app/defaults/overlay.cfg" "$APP/defaults/overlay.cfg"
cp -f "$SRC/dist/bin/overlayd" "$APP/bin/overlayd"
cp -f "$SRC/dist/bin/overlay_ui" "$APP/bin/overlay_ui"
cp -f "$SRC/dist/bin/fpshud" "$APP/bin/fpshud"
cp -f "$SRC/dist/lib/liboverlyder_pace.so" "$APP/lib/liboverlyder_pace.so"
cp -f "$SRC/app/resources/MaterialIcons-Regular.ttf" "$APP/resources/"
cp -f "$SRC/app/resources/LICENSE-MaterialIcons.txt" "$APP/resources/"
cp -f "$SRC/app/lang/"*.json "$APP/lang/"
chmod 755 "$APP/launch.sh" "$APP/overlayctl" "$APP/updater.sh" "$APP/bin/overlayd" "$APP/bin/overlay_ui" "$APP/bin/fpshud" "$APP/lib/liboverlyder_pace.so"

if [ ! -f "$CFG" ]; then
    cp -f "$APP/defaults/overlay.cfg" "$CFG"
fi

echo "Installing autostart hook..."
cp -f "$SRC/app/overLyder.start" "$HOOK"
chmod 755 "$HOOK"

# One pointer line in /etc/ld.so.preload loads the limiter into every game
# process. The library itself lives inside the app; other entries are kept.
echo "Registering frame limiter shim..."
LIBLINE="$APP/lib/liboverlyder_pace.so"
PRELOAD=/etc/ld.so.preload
if [ -f "$PRELOAD" ]; then
    if ! grep -qxF "$LIBLINE" "$PRELOAD"; then
        cp -a "$PRELOAD" "$PRELOAD.bak.$STAMP"
        { cat "$PRELOAD"; printf '%s\n' "$LIBLINE"; } > "$PRELOAD.new"
        cat "$PRELOAD.new" > "$PRELOAD"
        rm -f "$PRELOAD.new"
        sync
    fi
else
    printf '%s\n' "$LIBLINE" > "$PRELOAD"
    chmod 644 "$PRELOAD"
    sync
fi

# Upgrade from the old name: remove the previous app and startup hook so two
# daemons cannot be launched at boot. The shared config in $ETC is preserved.
LEGACY_APP=/mnt/SDCARD/Apps/OverlaySwitch
LEGACY_HOOK=$STARTS/overlayswitch.sh
if [ -d "$LEGACY_APP" ] || [ -e "$LEGACY_HOOK" ]; then
    echo "Removing legacy OverlaySwitch install..."
    if [ -x "$LEGACY_APP/overlayctl" ]; then
        "$LEGACY_APP/overlayctl" stop || true
    fi
    rm -rf "$LEGACY_APP"
    rm -f "$LEGACY_HOOK"
fi

sync

if [ "$1" = "--no-start" ]; then
    # Leave it inactive: the user enables it from the OverLyder app.
    printf 'disabled\n' > "$DISABLE"
    echo
    echo "OverLyder installed but NOT activated."
    echo "Open the 'OverLyder' app and set Estado = Activado to enable it."
else
    "$APP/overlayctl" restart
    echo
    echo "OverLyder installed and running."
    echo "  - Open the 'OverLyder' app to enable/disable and set the hotkey."
    echo "  - Log: /tmp/overlayd.log"
fi
