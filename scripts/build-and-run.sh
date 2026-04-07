#!/usr/bin/env bash
# Build the plug harness and run it with a 10-second timeout.
# Usage: ./scripts/build-and-run.sh [plug_binary]
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

PLUG_BIN="${1:-../sheepshaver-data/mesa_scsi_plug.bin}"

echo "=== Building ==="
make -j4 2>&1 | grep -E "^(gcc -o|error|warning:.*main\.c)" || true
echo ""

echo "=== Running (10s timeout) ==="
perl -e 'alarm 30; exec @ARGV' ./mesa-plug-harness "$PLUG_BIN" 2>&1
EXIT=$?
echo ""
echo "Exit code: $EXIT"
if [ $EXIT -eq 142 ]; then
    echo "(killed by alarm — likely infinite loop)"
fi
