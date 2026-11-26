#!/usr/bin/env bash
# WUP ThinClient — build the GamePad homebrew into build/apps/.
#
#   scripts/build.sh
#
# Runs libwup's wupbuild.py with the app dir as its ONLY argument (so
# pad/app.cfg is honoured), then copies the produced .wup into this
# repository's build/apps/ directory.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# shellcheck disable=SC1091
[ -f build/env.sh ] || { echo "build: run 'make setup' first" >&2; exit 1; }
# shellcheck disable=SC1091
. build/env.sh

APP_DIR="$ROOT/pad"
OUT_DIR="$ROOT/build/apps"
mkdir -p "$OUT_DIR"

SDK_OUT="$LIBWUP_DIR/build/apps"

( cd "$LIBWUP_DIR" && python3 tools/wupbuild.py "$APP_DIR" )

# wupbuild names the artifact after app.cfg 'name' (lowercased, spaces -> _).
SRC_WUP=""
for f in "$SDK_OUT"/*.wup; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in
        wup_thinclient.wup) SRC_WUP="$f" ;;
    esac
done
if [ -z "$SRC_WUP" ] || [ ! -s "$SRC_WUP" ]; then
    # fall back: newest .wup in the SDK output dir
    SRC_WUP="$(ls -t "$SDK_OUT"/*.wup 2>/dev/null | head -1 || true)"
fi
if [ -z "${SRC_WUP:-}" ] || [ ! -s "$SRC_WUP" ]; then
    echo "build: wupbuild finished but no .wup artifact found in $SDK_OUT" >&2
    exit 1
fi

cp "$SRC_WUP" "$OUT_DIR/wup_thinclient.wup"
echo "build: $OUT_DIR/wup_thinclient.wup ($(stat -c %s "$OUT_DIR/wup_thinclient.wup") bytes)"
