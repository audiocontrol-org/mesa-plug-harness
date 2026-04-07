/*
 * scsi_midi.h — MIDI-over-SCSI transport for Akai S3000XL
 *
 * Wraps the scsi2pi bridge with the MIDI-over-SCSI CDB protocol:
 *   0x09 = Set MIDI Mode (enable/disable)
 *   0x0C = Send MIDI Data
 *   0x0D = Data Byte Enquiry (poll for available bytes)
 *   0x0E = Receive MIDI Data
 */
#ifndef SCSI_MIDI_H
#define SCSI_MIDI_H

#include <stdint.h>
#include <stddef.h>

struct ScsiMidi {
    int target_id;
    bool midi_enabled;
};

/* Initialize (does not enable MIDI mode) */
int scsi_midi_init(ScsiMidi *sm, int target_id);

/* Enable MIDI-over-SCSI mode on the device */
int scsi_midi_enable(ScsiMidi *sm);

/* Disable MIDI-over-SCSI mode */
int scsi_midi_disable(ScsiMidi *sm);

/* Send MIDI data to the device (CDB 0x0C).
 * Calls TEST UNIT READY first, per MESA protocol. */
int scsi_midi_send(ScsiMidi *sm, const uint8_t *data, size_t len);

/* Receive MIDI data from the device.
 * Polls with CDB 0x0D, then reads with CDB 0x0E.
 * *len is set to actual bytes received.
 * Returns 0 on success, negative on error, positive if no data after timeout. */
int scsi_midi_receive(ScsiMidi *sm, uint8_t *buf, size_t *len, int poll_retries);

/* Send SysEx and receive response (combined send + receive).
 * Returns 0 on success. rx_len updated with actual received length. */
int scsi_midi_exchange(ScsiMidi *sm,
                       const uint8_t *tx, size_t tx_len,
                       uint8_t *rx, size_t *rx_len);

#endif /* SCSI_MIDI_H */
