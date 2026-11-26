#!/usr/bin/env bash
# WUP ThinClient — deploy the built .wup to the GamePad NetFS share.
#
#   scripts/deploy.sh
#
# Target: ${WUP_SHARE_DIR:-$HOME/wup-share}/wup_thinclient.wup
# Verifies the artifact exists, copies atomically, and checks SHA256.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/apps/wup_thinclient.wup"
SHARE="${WUP_SHARE_DIR:-$HOME/wup-share}"
DST="$SHARE/wup_thinclient.wup"

if [ ! -s "$SRC" ]; then
    echo "deploy: $SRC missing or empty — run 'make' first" >&2
    exit 1
fi
if [ ! -d "$SHARE" ]; then
    echo "deploy: share directory $SHARE does not exist" >&2
    echo "deploy: set WUP_SHARE_DIR to the GamePad NetFS share" >&2
    exit 1
fi

TMP="$SHARE/.wup_thinclient.wup.tmp.$$"
cp "$SRC" "$TMP"
mv -f "$TMP" "$DST"

S1="$(sha256sum "$SRC" | cut -d' ' -f1)"
S2="$(sha256sum "$DST" | cut -d' ' -f1)"
if [ "$S1" != "$S2" ]; then
    echo "deploy: checksum mismatch after copy" >&2
    exit 1
fi

echo "deploy: $DST"
echo "deploy: size $(stat -c %s "$DST") bytes"
echo "deploy: sha256 $S2"
