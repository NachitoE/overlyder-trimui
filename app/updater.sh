#!/bin/sh
# OverLyder self-updater (used by the "Actualizar" button of the CrossMix app).
#
#   updater.sh check                   -> "OK <latest_tag>"  or  "ERR <code>"
#   updater.sh install <tag>           -> "OK"               or  "ERR <code>"
#
# Exactly one line is printed so the C caller can parse it with sscanf(). The
# version, owner and repo come from manifest.json (read by overlay_ui.c); this
# script only reports what GitHub publishes and performs the install when asked.

APP="${OVERLYDER_APP:-/mnt/SDCARD/Apps/OverLyder}"
MF="${OVERLYDER_MANIFEST:-$APP/manifest.json}"
TMP="${OVERLYDER_TMP:-/tmp/overlyder-update}"

# Everything about the app (version, owner, repo, updater script) lives in
# manifest.json. Environment variables still win, for testing from a PC.
json_val() {
    [ -f "$MF" ] || return 1
    sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$MF" | head -n 1
}
OWNER="${GITHUB_OWNER:-$(json_val owner)}"
OWNER="${OWNER:-NachitoE}"
REPO="${GITHUB_REPO:-$(json_val repo)}"
REPO="${REPO:-overlyder-trimui}"
API="https://api.github.com/repos/$OWNER/$REPO/releases/latest"

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

# "v0.0.4" and "0.0.4" name the same release.
ver_plain() {
    printf '%s' "$1" | sed 's/^[vV]//'
}

cmd_install() {
    tag="$1"
    allow_same="${2:-0}"
    [ -n "$tag" ] || { echo "ERR notag"; return; }

    # Without the option enabled, a release equal to the installed one is not
    # an update: refuse it here too, not just in the UI.
    if [ "$allow_same" != "1" ]; then
        cur_ver=$(json_val version)
        if [ -n "$cur_ver" ] && [ "$(ver_plain "$cur_ver")" = "$(ver_plain "$tag")" ]; then
            echo "ERR same"
            return
        fi
    fi

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

    # Zips built with Windows Compress-Archive store entries as
    # "OverLyder\\bin\\overlayd" (backslashes are part of the filename), so
    # unzip drops them in the root instead of creating OverLyder/. Normalize
    # that layout before looking for the app folder.
    if [ ! -d "$src" ]; then
        mkdir -p "$src" || { echo "ERR layout"; rm -rf "$TMP"; return; }
        for f in "$TMP"/*; do
            [ -f "$f" ] || continue
            [ "$f" = "$TMP/OverLyder.zip" ] && continue
            rel=$(basename "$f" | sed 's|\\|/|g; s|^OverLyder/||')
            [ -n "$rel" ] || continue
            mkdir -p "$src/$(dirname "$rel")" 2>/dev/null
            mv "$f" "$src/$rel" 2>/dev/null
        done
    fi

    [ -d "$src" ] || { echo "ERR layout"; rm -rf "$TMP"; return; }
    [ -f "$src/manifest.json" ] || { echo "ERR layout"; rm -rf "$TMP"; return; }

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

    # A release zip can ship a stale manifest (the 0.0.3 asset had 0.0.2
    # inside). Trust the tag we were asked to install, not the zip's copy.
    mf="$APP/manifest.json"
    if [ -f "$mf" ]; then
        sed -i "s|\"version\"[[:space:]]*:[[:space:]]*\"[^\"]*\"|\"version\": \"$tag\"|" "$mf" 2>/dev/null
    fi
    sync

    if [ -x "$APP/overlayctl" ]; then
        "$APP/overlayctl" start >/dev/null 2>&1
    fi
    rm -rf "$TMP"
    echo "OK"
}

case "$1" in
    check)   cmd_check "$2" ;;
    install) cmd_install "$2" "$3" ;;
    *)       echo "ERR usage" ;;
esac
exit 0
