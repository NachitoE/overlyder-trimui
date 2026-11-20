#!/bin/sh
# OverLyder self-updater (used by the "Actualizar" button of the CrossMix app).
#
#   updater.sh check                   -> "OK <latest_tag>"  or  "ERR <code>"
#   updater.sh install <tag>           -> "OK"               or  "ERR <code>"
#
# Exactly one line is printed so the C caller can parse it with sscanf(). The
# version, owner and repo come from manifest.json (read by overlay_ui.c); this
# script only reports what GitHub publishes and performs the install when asked.

OWNER="${GITHUB_OWNER:-NachitoE}"
REPO="${GITHUB_REPO:-overlyder-trimui}"
API="https://api.github.com/repos/$OWNER/$REPO/releases/latest"
APP="${OVERLYDER_APP:-/mnt/SDCARD/Apps/OverLyder}"
TMP="${OVERLYDER_TMP:-/tmp/overlyder-update}"

# -k / --no-check-certificate: CrossMix images often ship without a current CA
# bundle, which otherwise breaks HTTPS requests to api.github.com.
fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -k -fsSL --connect-timeout 6 --max-time 20 "$1" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -q --no-check-certificate -T 20 -O - "$1" 2>/dev/null
    else
        return 127
    fi
}

fetch_file() {
    if command -v curl >/dev/null 2>&1; then
        curl -k -fL --connect-timeout 6 --max-time 120 -o "$2" "$1" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -q --no-check-certificate -T 120 -O "$2" "$1" 2>/dev/null
    else
        return 127
    fi
}

cmd_check() {
    json=$(fetch "$API") || { echo "ERR nonet"; return; }
    [ -n "$json" ] || { echo "ERR nonet"; return; }
    tag=$(printf '%s\n' "$json" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
    [ -n "$tag" ] || { echo "ERR parse"; return; }
    echo "OK $tag"
}

cmd_install() {
    tag="$1"
    [ -n "$tag" ] || { echo "ERR notag"; return; }

    url="https://github.com/$OWNER/$REPO/releases/download/$tag/OverLyder.zip"
    rm -rf "$TMP" 2>/dev/null
    mkdir -p "$TMP" || { echo "ERR tmp"; return; }

    if ! fetch_file "$url" "$TMP/OverLyder.zip" || [ ! -s "$TMP/OverLyder.zip" ]; then
        echo "ERR download"; rm -rf "$TMP"; return
    fi
    if command -v unzip >/dev/null 2>&1; then
        unzip -oq "$TMP/OverLyder.zip" -d "$TMP" 2>/dev/null || { echo "ERR unzip"; rm -rf "$TMP"; return; }
    else
        echo "ERR nounzip"; rm -rf "$TMP"; return
    fi

    src="$TMP/OverLyder"
    [ -d "$src" ] || { echo "ERR layout"; rm -rf "$TMP"; return; }

    # The daemon must not keep old inodes alive while we replace its binaries.
    if [ -x "$APP/overlayctl" ]; then
        "$APP/overlayctl" stop >/dev/null 2>&1
    fi

    # Remove each destination before copying: overwriting a running executable
    # in place can fail with "text file busy" on busybox cp.
    files=$(cd "$src" && find . -type f | sed 's|^\./||')
    for f in $files; do
        mkdir -p "$APP/$(dirname "$f")" 2>/dev/null
        rm -f "$APP/$f" 2>/dev/null
        cp -f "$src/$f" "$APP/$f" 2>/dev/null || { echo "ERR copy"; rm -rf "$TMP"; return; }
    done
    chmod 755 "$APP/launch.sh" "$APP/overlayctl" "$APP/bin/overlayd" "$APP/bin/overlay_ui" 2>/dev/null
    sync

    if [ -x "$APP/overlayctl" ]; then
        "$APP/overlayctl" start >/dev/null 2>&1
    fi
    rm -rf "$TMP"
    echo "OK"
}

case "$1" in
    check)   cmd_check "$2" ;;
    install) cmd_install "$2" ;;
    *)       echo "ERR usage" ;;
esac
exit 0
