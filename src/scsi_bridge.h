/*
 * scsi_bridge.h — Forward SCSI commands to scsi2pi over TCP
 */
#ifndef SCSI_BRIDGE_H
#define SCSI_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the bridge. Call once before scsi_bridge_exec. */
void scsi_bridge_init(const char *host, int port);

/* Execute a SCSI command via scsi2pi.
 * Returns 0 on success, negative on transport error, positive = SCSI status.
 * data_in buffer is filled with response data; data_in_len is updated. */
int scsi_bridge_exec(int target_id, int target_lun,
                     const uint8_t *cdb, int cdb_len,
                     const uint8_t *data_out, size_t data_out_len,
                     uint8_t *data_in, size_t *data_in_len,
                     int timeout_sec);

#ifdef __cplusplus
}
#endif

#endif /* SCSI_BRIDGE_H */
