#!/usr/bin/env bash
# Package the generic Linux runtime into a PortMaster new-structure archive.

set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
VERSION="${1:-}"
OUTPUT_DIR="${2:-$ROOT_DIR/dist}"
PORT_NAME="${PORT_NAME:-gtasa}"
PORT_METADATA_DIR="${PORTMASTER_METADATA_DIR:-$ROOT_DIR/portmaster/$PORT_NAME}"
BINARY="${GTASA_BINARY:-$ROOT_DIR/build-aarch64/gtasa_linux}"
SDL3_RUNTIME="${GTASA_SDL3_RUNTIME:-$ROOT_DIR/libSDL3.so.0}"
CONSOLE_UI="${GTASA_CONSOLE_UI:-1}"
LAUNCHER="${PORTMASTER_LAUNCHER:-$ROOT_DIR/Grand Theft Auto San Andreas.sh}"

usage() {
    printf 'usage: %s VERSION [OUTPUT_DIR]\n' "$(basename "$0")" >&2
    printf '       PORTMASTER_METADATA_DIR=/path/to/gtasa %s VERSION\n' "$(basename "$0")" >&2
    printf '       GTASA_BINARY=/path/to/gtasa_linux GTASA_SDL3_RUNTIME=/path/to/libSDL3.so.0 %s VERSION\n' "$(basename "$0")" >&2
    printf '       GTASA_CONSOLE_UI=0 %s VERSION  # omit Adjustable.cfg\n' "$(basename "$0")" >&2
}

if [[ -z "$VERSION" || ! "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]]; then
    usage
    exit 2
fi

if [[ ! "$PORT_NAME" =~ ^[a-z0-9][a-z0-9._]*$ ]]; then
    printf 'package: invalid PortMaster port name: %s\n' "$PORT_NAME" >&2
    exit 2
fi
if [[ "$CONSOLE_UI" != 0 && "$CONSOLE_UI" != 1 ]]; then
    printf 'package: GTASA_CONSOLE_UI must be 0 or 1\n' >&2
    exit 2
fi

metadata_required=(
    "$PORT_METADATA_DIR/port.json"
    "$PORT_METADATA_DIR/README.md"
    "$PORT_METADATA_DIR/gameinfo.xml"
)
metadata_screenshot=""
for candidate in "$PORT_METADATA_DIR/screenshot.jpg" "$PORT_METADATA_DIR/screenshot.png"; do
    if [[ -f "$candidate" ]]; then
        metadata_screenshot="$candidate"
        break
    fi
done
if [[ -z "$metadata_screenshot" ]]; then
    printf 'package: missing metadata screenshot.jpg or screenshot.png in %s\n' "$PORT_METADATA_DIR" >&2
    exit 1
fi
for required in "${metadata_required[@]}" "$BINARY" "$LAUNCHER" "$SDL3_RUNTIME" \
    "$ROOT_DIR/libc++_shared.so" "$ROOT_DIR/assetfile.txt"; do
    if [[ ! -f "$required" ]]; then
        printf 'package: missing required file: %s\n' "$required" >&2
        exit 1
    fi
done

if [[ ! -x "$BINARY" ]]; then
    printf 'package: binary is not executable: %s\n' "$BINARY" >&2
    exit 1
fi
if ! file -b "$BINARY" | grep -qi 'aarch64'; then
    printf 'package: binary is not AArch64: %s\n' "$BINARY" >&2
    file -b "$BINARY" >&2
    exit 1
fi

read -r manifest_count < "$ROOT_DIR/assetfile.txt"
manifest_lines="$(wc -l < "$ROOT_DIR/assetfile.txt")"
if [[ "$manifest_count" != 120 || "$manifest_lines" != 121 ]]; then
    printf 'package: assetfile.txt must contain 120 entries (header plus 120 lines)\n' >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(CDPATH= cd -- "$OUTPUT_DIR" && pwd -P)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/gtasa-package.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

PACKAGE_DIR="$STAGE/$PORT_NAME"
GAME_DIR="$PACKAGE_DIR/$PORT_NAME"
mkdir -p "$GAME_DIR/licenses" "$GAME_DIR/libs.aarch64"

install -m 0644 "$PORT_METADATA_DIR/port.json" "$PACKAGE_DIR/port.json"
install -m 0644 "$PORT_METADATA_DIR/README.md" "$PACKAGE_DIR/README.md"
install -m 0644 "$metadata_screenshot" "$PACKAGE_DIR/$(basename "$metadata_screenshot")"
install -m 0644 "$PORT_METADATA_DIR/gameinfo.xml" "$PACKAGE_DIR/gameinfo.xml"
if [[ -f "$PORT_METADATA_DIR/cover.jpg" ]]; then
    install -m 0644 "$PORT_METADATA_DIR/cover.jpg" "$PACKAGE_DIR/cover.jpg"
elif [[ -f "$PORT_METADATA_DIR/cover.png" ]]; then
    install -m 0644 "$PORT_METADATA_DIR/cover.png" "$PACKAGE_DIR/cover.png"
fi
install -m 0755 "$LAUNCHER" "$PACKAGE_DIR/$(basename "$LAUNCHER")"

if [[ -d "$PORT_METADATA_DIR/licenses" ]]; then
    cp -a "$PORT_METADATA_DIR/licenses/." "$GAME_DIR/licenses/"
elif [[ -f "$ROOT_DIR/LICENSE" ]]; then
    install -m 0644 "$ROOT_DIR/LICENSE" "$GAME_DIR/licenses/LICENSE"
fi
install -m 0755 "$BINARY" "$GAME_DIR/gtasa_linux"
install -m 0644 "$SDL3_RUNTIME" "$GAME_DIR/libs.aarch64/libSDL3.so.0"
install -m 0644 "$ROOT_DIR/libc++_shared.so" "$GAME_DIR/libc++_shared.so"
install -m 0644 "$ROOT_DIR/assetfile.txt" "$GAME_DIR/assetfile.txt"
if [[ "$CONSOLE_UI" == 1 ]]; then
    install -m 0644 "$ROOT_DIR/Adjustable.cfg" "$GAME_DIR/Adjustable.cfg"
fi

ARCHIVE="$OUTPUT_DIR/$PORT_NAME-$VERSION.zip"
(
    cd "$STAGE"
    TZ=UTC zip -X -q -r "$ARCHIVE" "$PORT_NAME"
)

expected=(
    "$PORT_NAME/"
    "$PORT_NAME/port.json"
    "$PORT_NAME/README.md"
    "$PORT_NAME/gameinfo.xml"
    "$PORT_NAME/$(basename "$metadata_screenshot")"
    "$PORT_NAME/$(basename "$LAUNCHER")"
    "$PORT_NAME/$PORT_NAME/"
    "$PORT_NAME/$PORT_NAME/licenses/"
    "$PORT_NAME/$PORT_NAME/assetfile.txt"
    "$PORT_NAME/$PORT_NAME/gtasa_linux"
    "$PORT_NAME/$PORT_NAME/libs.aarch64/"
    "$PORT_NAME/$PORT_NAME/libs.aarch64/libSDL3.so.0"
    "$PORT_NAME/$PORT_NAME/libc++_shared.so"
)
for license in "$GAME_DIR/licenses"/*; do
    [[ -f "$license" ]] || continue
    expected+=("$PORT_NAME/$PORT_NAME/licenses/$(basename "$license")")
done
if [[ "$CONSOLE_UI" == 1 ]]; then
    expected+=("$PORT_NAME/$PORT_NAME/Adjustable.cfg")
fi
mapfile -t actual < <(unzip -Z1 "$ARCHIVE" | LC_ALL=C sort)
mapfile -t expected_sorted < <(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)
if [[ "${actual[*]}" != "${expected_sorted[*]}" ]]; then
    printf 'package: archive contents failed verification\n' >&2
    printf '%s\n' "${actual[@]}" >&2
    exit 1
fi

sha256sum "$ARCHIVE" > "$ARCHIVE.sha256"
printf 'created: %s\n' "$ARCHIVE"
printf 'sha256: %s\n' "$(cut -d ' ' -f 1 "$ARCHIVE.sha256")"
