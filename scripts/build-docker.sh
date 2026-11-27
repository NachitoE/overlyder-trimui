#!/bin/sh
# Optional Docker-based cross build (requires Docker + the header/lib bundle).
# The Windows build (scripts/build-win.ps1) is the one used to produce the
# binaries shipped in dist/; this script is provided as a reproducible
# alternative for Linux/macOS hosts.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

ZIG_IMAGE=${ZIG_IMAGE:-ghcr.io/ziglang/zig:0.14.1}
TARGET=${TARGET:-aarch64-linux-gnu.2.33}
# headers/libs expected in third_party/ (see README):
#   third_party/SDL/include      third_party/SDL_ttf
#   third_party/libs/libSDL2.so  third_party/libs/libSDL2_ttf.so
: "${SDL_INC:=$ROOT/third_party/SDL/include}"
: "${TTF_INC:=$ROOT/third_party/SDL_ttf}"
: "${LIBS:=$ROOT/third_party/libs}"

mkdir -p dist/bin
run() {
    docker run --rm -v "$ROOT:/src" -w /src "$ZIG_IMAGE" \
        zig cc -target "$TARGET" -O2 -Wall -Wextra -Wno-unused-parameter \
        -Isrc -I"$SDL_INC" -I"$TTF_INC" "$@"
}

echo "==> overlayd"
run src/util.c src/overlay_layer.c src/textinject.c src/overlayd.c -o dist/bin/overlayd
echo "==> overlay_ui"
run src/util.c src/overlay_layer.c src/textinject.c src/overlay_ui.c -L "$LIBS" -lSDL2 -lSDL2_ttf -lm -o dist/bin/overlay_ui
echo "==> vpad (test tool)"
run tools/vpad.c -o dist/bin/vpad
echo "done: dist/bin"
