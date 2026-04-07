#!/usr/bin/env bash
# Quick hex dump of a range in the plug binary.
# Usage: ./scripts/hexdump-range.sh <start_offset> [length] [file]
set -euo pipefail
START="${1:?Usage: hexdump-range.sh <start_hex_offset> [length] [file]}"
LEN="${2:-128}"
FILE="${3:-../sheepshaver-data/mesa_scsi_plug.bin}"
xxd -s "$START" -l "$LEN" "$FILE"
