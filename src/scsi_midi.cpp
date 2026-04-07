/*
 * scsi_midi.cpp — MIDI-over-SCSI transport implementation
 */
#include "scsi_midi.h"
#include "scsi_bridge.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

static int test_unit_ready(int target) {
    uint8_t cdb[6] = {0};
    return scsi_bridge_exec(target, 0, cdb, 6, nullptr, 0, nullptr, nullptr, 10);
}

int scsi_midi_init(ScsiMidi *sm, int target_id) {
    sm->target_id = target_id;
    sm->midi_enabled = false;
    return test_unit_ready(target_id);
}

int scsi_midi_enable(ScsiMidi *sm) {
    uint8_t cdb[6] = { 0x09, 0x00, 0x01, 0x00, 0x00, 0x00 };
    int status = scsi_bridge_exec(sm->target_id, 0, cdb, 6,
                                   nullptr, 0, nullptr, nullptr, 10);
    if (status == 0) sm->midi_enabled = true;
    return status;
}

int scsi_midi_disable(ScsiMidi *sm) {
    uint8_t cdb[6] = { 0x09, 0x00, 0x00, 0x00, 0x00, 0x00 };
    int status = scsi_bridge_exec(sm->target_id, 0, cdb, 6,
                                   nullptr, 0, nullptr, nullptr, 10);
    sm->midi_enabled = false;
    return status;
}

int scsi_midi_send(ScsiMidi *sm, const uint8_t *data, size_t len) {
    /* CDB 0x0C: Send MIDI Data */
    uint8_t cdb[6] = {
        0x0C, 0x00,
        (uint8_t)((len >> 16) & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)(len & 0xFF),
        0x00  /* flag=0x00 (NOT 0x80 — S3000XL rejects 0x80) */
    };
    return scsi_bridge_exec(sm->target_id, 0, cdb, 6,
                            data, len, nullptr, nullptr, 10);
}

int scsi_midi_receive(ScsiMidi *sm, uint8_t *buf, size_t *len, int poll_retries) {
    if (poll_retries <= 0) poll_retries = 30;
    size_t max_len = *len;
    size_t total = 0;

    /* Loop: poll (0x0D) → read (0x0E) until F7 (End of SysEx) is seen */
    for (int round = 0; round < 50 && total < max_len; round++) {
        /* Poll for available bytes */
        uint32_t avail = 0;
        for (int i = 0; i < poll_retries; i++) {
            uint8_t poll_cdb[6] = { 0x0D, 0, 0, 0, 0, 0x00 };
            uint8_t poll_buf[3] = {0};
            size_t poll_len = 3;
            int status = scsi_bridge_exec(sm->target_id, 0, poll_cdb, 6,
                                           nullptr, 0, poll_buf, &poll_len, 10);
            if (status < 0) { *len = total; return status; }
            if (poll_len >= 3)
                avail = ((uint32_t)poll_buf[0] << 16) |
                        ((uint32_t)poll_buf[1] << 8) | poll_buf[2];
            if (avail > 0) break;
            usleep(100000);
        }

        if (avail == 0) break; /* No more data */

        /* Read chunk */
        size_t chunk = avail;
        if (total + chunk > max_len) chunk = max_len - total;
        uint8_t read_cdb[6] = {
            0x0E, 0x00,
            (uint8_t)((chunk >> 16) & 0xFF),
            (uint8_t)((chunk >> 8) & 0xFF),
            (uint8_t)(chunk & 0xFF),
            0x00
        };
        size_t got = chunk;
        int status = scsi_bridge_exec(sm->target_id, 0, read_cdb, 6,
                                       nullptr, 0, buf + total, &got, 10);
        if (status < 0) { *len = total; return status; }
        total += got;

        /* Check for F7 (End of SysEx) in received data */
        for (size_t i = total - got; i < total; i++) {
            if (buf[i] == 0xF7) {
                *len = i + 1; /* Include the F7 */
                return 0;
            }
        }

        /* Reduce poll retries for subsequent rounds (data should come faster) */
        poll_retries = 5;
    }

    *len = total;
    return (total > 0) ? 0 : 1;
}

int scsi_midi_exchange(ScsiMidi *sm,
                       const uint8_t *tx, size_t tx_len,
                       uint8_t *rx, size_t *rx_len)
{
    int status = scsi_midi_send(sm, tx, tx_len);
    if (status != 0) {
        fprintf(stderr, "scsi_midi: send failed (status=%d)\n", status);
        /* Send may return CHECK CONDITION but data was still accepted.
         * Continue to receive anyway. */
    }

    usleep(200000); /* 200ms for device to process */

    status = scsi_midi_receive(sm, rx, rx_len, 30);
    if (status != 0)
        fprintf(stderr, "scsi_midi: receive failed (status=%d, rx_len=%zu)\n", status, *rx_len);
    return status;
}
