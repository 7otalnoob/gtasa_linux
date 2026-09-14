#!/usr/bin/env bash
# Build and stage one PortMaster AArch64 artifact inside the builder container.

set -euo pipefail

GAME="${GAME:-gtasa}"
LAUNCHER_SCRIPT="${LAUNCHER_SCRIPT:-Grand Theft Auto San Andreas.sh}"
BINARY_NAME="${BINARY_NAME:-${GAME}_linux}"
GAME_TITLE="${GAME_TITLE:-$GAME}"
CONFIG_NAME="${CONFIG_NAME:-${GAME}.cfg}"
APPSTATE_NAME="${APPSTATE_NAME:-${GAME}-appstate.txt}"

BUILD_ROOT="${PORTMASTER_BUILD_ROOT:-/workspace/.portmaster-build}"
OUT_ROOT="${PORTMASTER_OUT_ROOT:-/workspace/out}"
PACKAGE_ROOT="${PORTMASTER_PACKAGE_ROOT:-/workspace/package}"
SDL_COMMIT="${SDL_COMMIT:-6057d79baf8321bf190479a699655f06cc2a962f}"
SPIRV_CROSS_COMMIT="${SPIRV_CROSS_COMMIT:-be71ee8c12cd7dc5ca8fa9581f708c2e8561fe2a}"

prepare() {
    if command -v dpkg-query >/dev/null 2>&1 && \
       ! dpkg-query -W -f='${Status}' libmpg123-dev 2>/dev/null | grep -q 'install ok installed'; then
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libmpg123-dev
    fi
    mkdir -p "$BUILD_ROOT"
    if [ ! -d "$BUILD_ROOT/SDL/.git" ]; then
        git clone --depth 1 --branch sdl2-backend https://github.com/bmdhacks/SDL.git "$BUILD_ROOT/SDL"
    fi
    test "$(git -C "$BUILD_ROOT/SDL" rev-parse HEAD)" = "$SDL_COMMIT"
    if [ ! -d "$BUILD_ROOT/SPIRV-Cross/.git" ]; then
        git clone --filter=blob:none https://github.com/KhronosGroup/SPIRV-Cross.git "$BUILD_ROOT/SPIRV-Cross"
    fi
    git -C "$BUILD_ROOT/SPIRV-Cross" checkout --detach "$SPIRV_CROSS_COMMIT"
    test "$(git -C "$BUILD_ROOT/SPIRV-Cross" rev-parse HEAD)" = "$SPIRV_CROSS_COMMIT"
}

build_shim() {
    prepare
    if ! git -C "$BUILD_ROOT/SDL" apply --reverse --check \
        /workspace/patches/sdl2-explicit-controller-config.patch >/dev/null 2>&1; then
        git -C "$BUILD_ROOT/SDL" apply \
            /workspace/patches/sdl2-explicit-controller-config.patch
    fi
    cmake -S "$BUILD_ROOT/SDL" -B "$BUILD_ROOT/SDL-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$BUILD_ROOT/sysroot" \
        -DSDL_SDL2_BACKEND=ON -DSDL_SPIRV_CROSS_DIR="$BUILD_ROOT/SPIRV-Cross" \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_KMSDRM=ON \
        -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=OFF -DSDL_ALSA_SHARED=OFF \
        -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_JACK=OFF \
        -DSDL_OFFSCREEN=OFF -DSDL_DUMMYVIDEO=OFF -DSDL_DUMMYAUDIO=OFF \
        -DSDL_DISKAUDIO=OFF -DSDL_VULKAN=OFF
    cmake --build "$BUILD_ROOT/SDL-build" --parallel "${JOBS:-2}"
    cmake --install "$BUILD_ROOT/SDL-build"
    test -f "$BUILD_ROOT/sysroot/lib/libSDL3.so.0"
    if command -v readelf >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1; then
        local max_glibc
        max_glibc="$(readelf --version-info "$BUILD_ROOT/sysroot/lib/libSDL3.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 | sed 's/^GLIBC_//')"
        if [ -n "$max_glibc" ] && dpkg --compare-versions "$max_glibc" gt 2.30; then
            echo "libSDL3.so.0 requires GLIBC_$max_glibc; maximum supported is GLIBC_2.30" >&2
            exit 1
        fi
        echo "libSDL3.so.0 maximum GLIBC requirement: ${max_glibc:-unknown}"
    fi
}

build_game() {
    build_shim
    export PKG_CONFIG_PATH="$BUILD_ROOT/sysroot/lib/pkgconfig"
    local build_dir="build-${GAME}-portmaster"
    cmake -S /workspace -B "/workspace/$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
        -DGTASA_DEBUG_LOG="${GTASA_DEBUG_LOG:-OFF}" -DGTASA_QUIT_CHORD=ON -DGTASA_SDL2_SHIM=ON \
        -DGTASA_PRODUCT_NAME="$GAME_TITLE" -DGTASA_CONFIG_NAME="$CONFIG_NAME" \
        -DGTASA_APPSTATE_NAME="$APPSTATE_NAME" -DGAME_BINARY_NAME="$BINARY_NAME"
    cmake --build "/workspace/$build_dir" --parallel "${JOBS:-2}"
    ctest --test-dir "/workspace/$build_dir" --output-on-failure
    local out="$OUT_ROOT/$GAME"
    rm -rf "$out"
    mkdir -p "$out/libs.aarch64"
    cp "/workspace/$build_dir/$BINARY_NAME" "$out/$BINARY_NAME"
    cp "$BUILD_ROOT/sysroot/lib/libSDL3.so.0" "$out/libs.aarch64/libSDL3.so.0"
    file "$out/$BINARY_NAME"
    if command -v readelf >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1; then
        local max_glibc
        max_glibc="$(readelf --version-info "$out/$BINARY_NAME" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 | sed 's/^GLIBC_//')"
        if [ -n "$max_glibc" ] && dpkg --compare-versions "$max_glibc" gt 2.30; then
            echo "$BINARY_NAME requires GLIBC_$max_glibc; maximum supported is GLIBC_2.30" >&2
            exit 1
        fi
        echo "$BINARY_NAME maximum GLIBC requirement: ${max_glibc:-unknown}"
    fi
}

package_game() {
    rm -rf "$PACKAGE_ROOT"
    mkdir -p "$PACKAGE_ROOT/$GAME/libs.aarch64"
    cp "/workspace/$LAUNCHER_SCRIPT" "$PACKAGE_ROOT/"
    cp "$OUT_ROOT/$GAME/$BINARY_NAME" "$PACKAGE_ROOT/$GAME/"
    cp "$OUT_ROOT/$GAME/libs.aarch64/libSDL3.so.0" "$PACKAGE_ROOT/$GAME/libs.aarch64/"
    cp /workspace/libc++_shared.so "$PACKAGE_ROOT/$GAME/"
    for optional in assetfile.txt Adjustable.cfg; do
        if [ -f "/workspace/$optional" ]; then
            cp "/workspace/$optional" "$PACKAGE_ROOT/$GAME/"
        fi
    done
}

case "${1:-build}" in
    prepare) prepare ;;
    shim) build_shim ;;
    build) build_game; package_game ;;
    *) printf 'usage: %s [prepare|shim|build]\n' "$0" >&2; exit 2 ;;
esac
