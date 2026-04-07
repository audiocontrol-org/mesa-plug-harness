#!/usr/bin/env python3
"""Find all A-line trap calls ($Axxx) in the MESA SCSI Plug binary and show context."""
import struct
import sys

TRAPS = {
    0xA055: "StripAddress", 0xA089: "SCSIDispatch", 0xA198: "_Unimplemented",
    0xA346: "GetOSTrapAddress", 0xA746: "GetToolTrapAddress", 0xA89F: "SCSIAtomic",
    0xA820: "Get1NamedResource", 0xA9A0: "GetResource", 0xA9A1: "GetNamedResource",
    0xA002: "_Read", 0xA003: "_Write", 0xA004: "_Control", 0xA005: "_Status",
    0xA01E: "NewPtr", 0xA122: "NewHandle", 0xA023: "DisposeHandle",
    0xA029: "HLock", 0xA02A: "HUnlock", 0xA871: "NewDialog", 0xA975: "ModalDialog",
    0xA994: "CurResFile", 0xA998: "UseResFile", 0xA9A1: "GetNamedResource",
}

path = sys.argv[1] if len(sys.argv) > 1 else "../sheepshaver-data/mesa_scsi_plug.bin"
with open(path, "rb") as f:
    data = f.read()

print(f"Scanning {len(data)} bytes for A-line traps...\n")

for i in range(0, len(data) - 1, 2):
    word = (data[i] << 8) | data[i+1]
    if (word & 0xF000) == 0xA000:
        name = TRAPS.get(word, "?")
        # Show 4 bytes before and 4 bytes after for context
        before = data[max(0,i-4):i]
        after = data[i+2:min(len(data),i+6)]
        ctx_before = " ".join(f"{b:02x}" for b in before)
        ctx_after = " ".join(f"{b:02x}" for b in after)
        print(f"  0x{i:04x}: ${word:04X} ({name:20s})  ctx: {ctx_before} | {ctx_after}")
