#!/usr/bin/env python3
"""Map all functions in the MESA SCSI Plug binary by finding LINK/UNLK/RTS patterns."""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "../sheepshaver-data/mesa_scsi_plug.bin"
with open(path, "rb") as f:
    data = f.read()

# Find all LINK A6 (4E56) and match with next UNLK+RTS (4E5E 4E75) + name string
for i in range(0x0600, len(data) - 3, 2):
    if data[i:i+2] != b'\x4E\x56':
        continue
    disp = struct.unpack('>h', data[i+2:i+4])[0]
    # Find next UNLK+RTS
    for j in range(i + 4, min(i + 0x1000, len(data) - 4), 2):
        if data[j:j+4] != b'\x4E\x5E\x4E\x75':
            continue
        rts_off = j + 2
        name_off = j + 4
        name_len = data[name_off] & 0x7F
        name = ""
        if 2 < name_len < 80 and name_off + 1 + name_len <= len(data):
            try:
                n = data[name_off+1:name_off+1+name_len].decode("ascii", errors="replace")
                if sum(c.isalnum() or c in "_:" for c in n) > len(n) * 0.7:
                    name = n
            except:
                pass
        if not name:
            name = f"(unnamed, RTS at 0x{rts_off:04x})"
        size = rts_off - i + 2
        print(f"  0x{i:04x}  frame={-disp:4d}  size={size:5d}  {name}")
        break
