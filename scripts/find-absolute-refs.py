#!/usr/bin/env python3
"""Find all absolute JSR/JMP instructions in the plug binary that need relocation."""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "../sheepshaver-data/mesa_scsi_plug.bin"
with open(path, "rb") as f:
    data = f.read()

print("Searching for absolute JSR (4EB9) and JMP (4EF9) instructions...\n")

# JSR (abs.l) = 4EB9 followed by 4-byte address
# JMP (abs.l) = 4EF9 followed by 4-byte address
# JSR (abs.w) = 4EB8 followed by 2-byte address (sign-extended)
# Also check for MOVE.L #abs (2F3C/213C etc.) that might store function pointers

count = 0
for i in range(0, len(data) - 5, 2):
    word = (data[i] << 8) | data[i+1]
    if word == 0x4EB9:  # JSR (abs.l)
        addr = struct.unpack('>I', data[i+2:i+6])[0]
        if 0x0400 < addr < len(data):  # Looks like a file-relative address
            print(f"  0x{i:04x}: JSR $0x{addr:08x}  (file offset, needs +PLUG_CODE_BASE)")
            count += 1
        elif addr > 0x10000:
            print(f"  0x{i:04x}: JSR $0x{addr:08x}  (already relocated?)")
    elif word == 0x4EF9:  # JMP (abs.l)
        addr = struct.unpack('>I', data[i+2:i+6])[0]
        if 0x0400 < addr < len(data):
            print(f"  0x{i:04x}: JMP $0x{addr:08x}  (file offset, needs +PLUG_CODE_BASE)")
            count += 1

print(f"\nTotal unrelocated absolute references: {count}")
