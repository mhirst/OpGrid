#!/usr/bin/env bash
# Copy the built module to a Move running Schwung, over SSH.
#   MOVE_HOST=ableton@move.local ./scripts/install.sh
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules/overtake

[ -f build/opgrid/dsp.so ] || { echo "Run ./scripts/build.sh first"; exit 1; }

ssh "$HOST" "mkdir -p $DEST"
scp -r build/opgrid "$HOST:$DEST/"
echo "Installed to $HOST:$DEST/opgrid"
echo "On the Move: open Schwung's overtake menu and pick OpGrid (rescan modules or restart Move if it is not listed)."
