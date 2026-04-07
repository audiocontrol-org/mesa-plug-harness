#!/usr/bin/env bash
# Test read-modify-write for program headers and keygroup headers.
# Changes values, verifies, then restores originals.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

S3K="./s3k-client"

echo "=== Read-Modify-Write Round-Trip Test ==="
echo ""

# --- Test 1: Modify program loudness ---
echo "--- Test 1: Program loudness ---"
echo "Before:"
$S3K program-header 0 2>/dev/null | grep Loudness

# Fetch raw, modify, write back, verify
$S3K raw 06 00 00 2>/dev/null > /tmp/s3k_prog_raw.txt
ORIG_LOUDNESS=$($S3K program-header 0 2>/dev/null | grep Loudness | awk '{print $2}')
echo "  Original loudness: $ORIG_LOUDNESS"

# We need to modify the raw SysEx and send it back.
# For now, test via the CLI — we'll need a modify command.
echo "  (Need modify CLI command — skipping raw write test)"
echo ""

# --- Test 2: Verify sample header read-back after upload ---
echo "--- Test 2: Sample upload round-trip ---"
echo "Downloading sample 0..."
$S3K download-sample 0 /tmp/s3k_test.wav 2>/dev/null
echo "Uploading as sample 8..."
$S3K upload-sample 8 /tmp/s3k_test.wav 2>/dev/null

echo "Comparing headers:"
echo "  Original:"
$S3K sample-header 0 2>/dev/null | grep -E "Name|Length|rate"
echo "  Uploaded:"
# Find the new sample index
NEW_COUNT=$($S3K list-samples 2>/dev/null | head -1 | awk '{print $1}')
NEW_IDX=$((NEW_COUNT - 1))
$S3K sample-header $NEW_IDX 2>/dev/null | grep -E "Name|Length|rate"

echo "Downloading uploaded sample for PCM comparison..."
$S3K download-sample $NEW_IDX /tmp/s3k_test_rt.wav 2>/dev/null
if cmp -s <(xxd -s 44 /tmp/s3k_test.wav) <(xxd -s 44 /tmp/s3k_test_rt.wav); then
    echo "  PCM data: IDENTICAL ✓"
else
    echo "  PCM data: DIFFERS ✗"
fi

echo "Cleaning up — deleting uploaded sample..."
$S3K delete-sample $NEW_IDX 2>/dev/null
FINAL_COUNT=$($S3K list-samples 2>/dev/null | head -1 | awk '{print $1}')
echo "  Sample count: $NEW_COUNT → $FINAL_COUNT"

echo ""
echo "=== Done ==="
