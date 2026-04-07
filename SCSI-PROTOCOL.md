# MESA II SCSI Plug - SCSI Protocol Reference

Extracted from `mesa_scsi_plug.bin` (SCSI Plug v2.1.2, AKAI & Living Memory 1995).

Binary analysis of the 68k code resource. A4-relative data base = file offset `$2BD2`.
Absolute-to-file address offset = `$061E` (file_offset = abs_addr + $61E).

## Class Hierarchy

```
CMESAPlugIn        - Base MESA Plug-In class
  CSCSIPlug        - SCSI MIDI plug-in (subclass)
CSCSIUtils         - SCSI Manager 4.3 utility class (composition, not inheritance)
CSCSIDialog        - Progress/abort dialog for SCSI operations
CDialog            - Generic Mac dialog wrapper
```

`CSCSIPlug` contains a `CSCSIUtils` instance at offset `$093A` within itself.

## Function Map

| File Offset | Abs Addr | C++ Symbol | Short Name |
|-------------|----------|------------|------------|
| `$0E72` | `$0854` | `SendData__9CSCSIPlugFP7IP_Data` | CSCSIPlug::SendData(IP_Data*) |
| `$12C0` | `$0CA2` | `SetSCSIMIDIMode__9CSCSIPlugFsUcUc` | CSCSIPlug::SetSCSIMIDIMode(short, uchar, uchar) |
| `$1372` | `$0D54` | `SMDataByteEnquiry__9CSCSIPlugFsUc` | CSCSIPlug::SMDataByteEnquiry(short, uchar) |
| `$141A` | `$0DFC` | `SMDispatchReply__9CSCSIPlugFsPUcUcPl` | CSCSIPlug::SMDispatchReply(short, uchar*, uchar, long*) |
| `$168C` | `$106E` | `SMSendData__9CSCSIPlugFsUcPUcPUclPl` | CSCSIPlug::SMSendData(short, uchar, uchar*, uchar*, long, long*) |
| `$1C3E` | `$1620` | `SCSICommand__10CSCSIUtilsFsP3CdbPUcUlls` | CSCSIUtils::SCSICommand(short, Cdb*, uchar*, ulong, long, short) |
| `$1DCA` | `$17AC` | `Inquiry__10CSCSIUtilsFccPUcUl` | CSCSIUtils::Inquiry(char, char, uchar*, ulong) |
| `$1E42` | `$1824` | `TestUnitReady__10CSCSIUtilsFs` | CSCSIUtils::TestUnitReady(short) |
| `$1E9C` | `$187E` | `TestUnitReady__10CSCSIUtilsFs` | (duplicate or wrapper) |
| `$1F38` | `$191A` | `WaitUntilReady__10CSCSIUtilsFs` | CSCSIUtils::WaitUntilReady(short) |
| `$1F8E` | `$1970` | `IdentifyBusses__10CSCSIUtilsFv` | CSCSIUtils::IdentifyBusses() |

Note: THINK C name strings follow each function body (after `UNLK A6; RTS`). The length
byte has its high bit set as a marker.

## SCSI ID Encoding

The SCSI ID is packed into a single `short` (16-bit word):

```
High byte = bus number
Low byte  = target ID
```

Example: SCSI ID `$0003` = bus 0, target 3.

## CDB Summary Table

| Opcode | CDB (hex) | Command | Direction | Data Size | Function |
|--------|-----------|---------|-----------|-----------|----------|
| `$00` | `00 00 00 00 00 00` | TEST UNIT READY | None | 0 | TestUnitReady |
| `$09` | `09 00 MM TT 00 00` | Set MIDI Mode | None | 0 | SetSCSIMIDIMode |
| `$0C` | `0C 00 HH MM LL FF` | Send MIDI Data | Write | 24-bit len | SMSendData |
| `$0D` | `0D 00 00 00 00 FF` | Data Byte Enquiry | Read | 3 bytes | SMDataByteEnquiry |
| `$0E` | `0E 00 HH MM LL FF` | Receive MIDI Data | Read | 24-bit len | SMDispatchReply |
| `$12` | `12 00 00 00 AL 00` | INQUIRY | Read | AL bytes | Inquiry |

## CDB Templates in Data Section

Stored at A4-relative offsets, copied to stack before each command:

| A4 Offset | File Offset | Bytes | Used By |
|-----------|-------------|-------|---------|
| `$0090` | `$2C62` | `00 00 00 00 00 00` | TestUnitReady |
| `$0096` | `$2C68` | `12 00 00 00 00 00` | Inquiry |
| `$0174` | `$2D46` | `0D 00 00 00 00 00` | SMDataByteEnquiry |
| `$017A` | `$2D4C` | `09 00 00 00 00 00` | SetSCSIMIDIMode |

`SMSendData` and `SMDispatchReply` build their CDBs inline (no template).

## Detailed CDB Specifications

### 1. TEST UNIT READY ($00)

```
Byte 0: $00  Operation code
Byte 1: $00  Reserved
Byte 2: $00  Reserved
Byte 3: $00  Reserved
Byte 4: $00  Reserved
Byte 5: $00  Control
```

- **Direction:** None (flags = `$0000`)
- **Timeout:** 1000 ticks
- **Data:** None
- **Purpose:** Checks if the SCSI device is ready. Called as a pre-check before
  every MIDI command (SetSCSIMIDIMode, SMSendData) and as a recovery mechanism
  in SMDispatchReply when no data is available.

### 2. INQUIRY ($12)

```
Byte 0: $12  Operation code
Byte 1: $00  EVPD=0, CMDDT=0
Byte 2: $00  Page code
Byte 3: $00  Reserved
Byte 4: AL   Allocation length (low byte of bufLen)
Byte 5: $00  Control
```

- **Direction:** Read (flags = `$0001`)
- **Timeout:** 1000 ticks
- **Data:** Reads `AL` bytes of INQUIRY data from the device
- **Purpose:** Identifies the SCSI device. The Plug checks for device type `$03`
  (Processor) to find AKAI samplers with MIDI-over-SCSI support.

### 3. Set MIDI Mode ($09)

```
Byte 0: $09  Operation code (vendor-specific)
Byte 1: $00  Reserved
Byte 2: MM   MIDI mode (0=off, 1=on)
Byte 3: TT   MIDI thru (0=off, 1=on)
Byte 4: $00  Reserved
Byte 5: $00  Control
```

- **Direction:** None (flags = `$0000`)
- **Timeout:** 1000 ticks
- **Data:** None
- **Pre-check:** Calls TestUnitReady before sending
- **Purpose:** Enables or disables MIDI-over-SCSI mode on the sampler. Must be
  called with MM=1 before sending any MIDI data, and with MM=0 after the
  transaction completes. The thru flag controls whether MIDI data received over
  SCSI is echoed to the device's physical MIDI Out.

### 4. Data Byte Enquiry ($0D)

```
Byte 0: $0D  Operation code (vendor-specific)
Byte 1: $00  Reserved
Byte 2: $00  Reserved
Byte 3: $00  Reserved
Byte 4: $00  Reserved
Byte 5: FF   Flag ($80 or $00)
```

- **Direction:** Read (flags = `$0001`)
- **Timeout:** 1000 ticks
- **Data:** Reads exactly 3 bytes
- **Returns:** 24-bit big-endian byte count: `(byte0 << 16) | (byte1 << 8) | byte2`
- **Flag byte:** `$80` when polling with expectation of data, `$00` when re-querying
  after a timeout/empty poll
- **Purpose:** Queries the device for how many MIDI bytes are buffered and ready to
  be read. The 3-byte response is a big-endian 24-bit count. Called before each
  read operation to determine the transfer size.

### 5. Send MIDI Data ($0C)

```
Byte 0: $0C  Operation code (vendor-specific)
Byte 1: $00  Reserved
Byte 2: HH   Data length, high byte
Byte 3: MM   Data length, mid byte
Byte 4: LL   Data length, low byte
Byte 5: FF   Flag ($80 or $00)
```

- **Direction:** Write (flags = `$0002`)
- **Timeout:** 1000 ticks
- **Data:** Writes `HH:MM:LL` bytes of MIDI data to the device
- **Length encoding:** 24-bit big-endian in CDB bytes 2-4
- **Flag byte:** `$80` when a reply is expected, `$00` for fire-and-forget sends
- **Pre-check:** Calls TestUnitReady before sending
- **Post-send:** If a receive buffer and result pointer are provided, automatically
  calls SMDispatchReply to read back the device's response.

### 6. Receive MIDI Data ($0E)

```
Byte 0: $0E  Operation code (vendor-specific)
Byte 1: $00  Reserved
Byte 2: HH   Data length, high byte
Byte 3: MM   Data length, mid byte
Byte 4: LL   Data length, low byte
Byte 5: FF   Flag ($80 or $00)
```

- **Direction:** Read (flags = `$0001`)
- **Timeout:** 1000 ticks
- **Data:** Reads `HH:MM:LL` bytes of MIDI data from the device
- **Length encoding:** 24-bit big-endian in CDB bytes 2-4, determined dynamically
  from a preceding Data Byte Enquiry ($0D) call
- **Flag byte:** Propagated from the caller's flag parameter
- **Termination:** Checks the last received byte for `$F7` (MIDI End of SysEx).
  The read loop terminates when `$F7` is seen, the expected byte count is reached,
  or the user aborts via the progress dialog.

### IdentifyBusses (SCSI Manager 4.3 Bus Inquiry)

Not a SCSI device command. Uses SCSI Manager 4.3 `SCSIBusInquiry` (function code
`$03`) to enumerate available SCSI buses on the Mac.

```
Parameter Block ($AC = 172 bytes):
  PB+$06: scsiPBLength = $00AC (172)
  PB+$08: scsiFunctionCode = $03 (SCSIBusInquiry)
  PB+$0D: scsiDevice.bus = loop counter (0..5)
  PB+$0E: scsiDevice.targetID = 0
  PB+$0F: scsiDevice.LUN = 0
```

- **Trap:** `_SCSIDispatch` ($A089) with D0=1 (SCSIAction)
- **Loop:** Iterates bus numbers 0 through 5 (max 6 buses)
- **Termination:** Stops when result = `$E143` (no more buses)
- **Stores:** Bus count at `this+$08`, per-bus info (172 bytes each) in the
  bus info array at `this+12 + bus*172`

## SCSICommand Parameter Block Construction

`CSCSIUtils::SCSICommand` builds a SCSI Manager 4.3 `SCSIExecIO` parameter block:

```
SCSIExecIO PB (at bus info array entry, A2-relative):
  A2+$06: scsiVersion        = from bus info lookup
  A2+$08: scsiPBLength       = 1 (valid flag)
  A2+$0D: scsiDevice.bus     = scsiID >> 8
  A2+$0E: scsiDevice.target  = scsiID & $FF
  A2+$10: scsiCompletion     = 0 (synchronous)
  A2+$14: scsiFlags          = direction flags (see below)
  A2+$28: scsiDataPtr        = data buffer pointer
  A2+$2C: scsiDataLength     = buffer length
  A2+$30: scsiSensePtr       = global sense buffer (A4+$01EC)
  A2+$34: scsiSenseLength    = $76 (118 bytes)
  A2+$35: scsiCDBLength      = $06 (6-byte CDB always)
  A2+$44: scsiCDB[0..5]      = 6 CDB bytes copied from caller
  A2+$54: scsiTimeout        = caller-specified timeout
  A2+$64: scsiSelectTimeout  = 0
  A2+$66: scsiMsgType        = 0 (simple)
  A2+$67: scsiIOFlags        = 1 (scsiDontDisconnect)
```

**Direction flags** (written to `scsiFlags` at A2+$14):

| Caller flags | scsiFlags value | Meaning |
|--------------|-----------------|---------|
| 0 | `$C0040000` | No data transfer |
| 1 | `$40040000` | Read (device to host) |
| 2 | `$80040000` | Write (host to device) |

Execution: `_SCSIDispatch` ($A089) trap with D0=1 (SCSIAction).

**Error handling:**
- Check Condition (status bit `$04`): calls `WaitUntilReady` to recover
- Busy (status bit `$02`): returns `$FFFE`
- Other errors: returns `$C945`

## Protocol Flow: MIDI Transaction

The high-level `SendData` function (0x0E72) orchestrates a complete MIDI-over-SCSI
transaction. The call sequence observed from the binary:

```
1. SetSCSIMIDIMode(scsiID, enable=1, thru=?)    -- Enable MIDI mode
2. SMDataByteEnquiry(scsiID, flag)               -- Check for pending data
3. SMDispatchReply(scsiID, buf, flag, &count)     -- Read any pending data
4. SMSendData(scsiID, flag, txData, rxBuf, len, &result)  -- Send MIDI message
   a. Internally: TestUnitReady(scsiID)           -- Pre-check
   b. Internally: CDB $0C (send data, WRITE)      -- Transmit bytes
   c. Internally: SMDispatchReply(...)             -- Read reply
      i.  SMDataByteEnquiry(scsiID, 1)            -- Poll for reply bytes
      ii. CDB $0E (receive data, READ)             -- Fetch reply bytes
      iii. Check for $F7 (End of SysEx)            -- Termination
5. (Repeat steps 4 for additional messages)
6. SetSCSIMIDIMode(scsiID, enable=0, thru=0)    -- Disable MIDI mode
```

The Plug uses a progress dialog with `ModalDialog` ($A975) calls during receive
loops to allow the user to cancel long transfers. Abort flags are checked at
`this+$E46` and `this+$E47` within the CSCSIPlug object.

## Key Constants

| Value | Meaning |
|-------|---------|
| `$03E8` | Default timeout: 1000 ticks (~16.7 seconds) |
| `$F7` | MIDI End of SysEx - terminates receive loop |
| `$E143` | SCSI Manager "no more buses" error |
| `$C945` | Plug error constant (unknown/generic SCSI error) |
| `$C946` | Plug error constant (dialog/abort related) |
| `$C948` | Plug error constant (device not ready / timeout) |
| `$FFFE` | Device busy return code |
| `$093A` | Offset of CSCSIUtils instance within CSCSIPlug |
| `$0E46` | Abort flag 1 offset within CSCSIPlug |
| `$0E47` | Abort flag 2 offset within CSCSIPlug |

## Opcode Reference (MIDI-over-SCSI)

These are vendor-specific SCSI commands used by AKAI samplers (S3000XL and similar)
for MIDI communication over the SCSI bus:

| Opcode | Name | Direction | CDB Bytes 2-4 | Byte 5 |
|--------|------|-----------|---------------|--------|
| `$09` | Set Port Mode | None | Byte 2: MIDI on/off, Byte 3: Thru on/off | Unused |
| `$0C` | Send Data | Write | 24-bit length (big-endian) | Flag ($80/$00) |
| `$0D` | Data Byte Enquiry | Read (3 bytes) | Unused (zeros) | Flag ($80/$00) |
| `$0E` | Receive Data | Read | 24-bit length (big-endian) | Flag ($80/$00) |

All are 6-byte Group 0 CDBs targeting SCSI Processor devices (Inquiry type `$03`).
