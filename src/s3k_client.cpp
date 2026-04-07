/*
 * s3k_client.cpp — Akai S3000XL SCSI client implementation
 */
#include "s3k_client.h"
#include "scsi_bridge.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

int s3k_init(S3kClient *c, const char *host, int port, int target, int channel) {
    c->channel = channel;

    scsi_bridge_init(host, port);

    int status = scsi_midi_init(&c->midi, target);
    if (status != 0) {
        fprintf(stderr, "s3k: device at target %d not ready (status=%d)\n", target, status);
        return status;
    }

    status = scsi_midi_enable(&c->midi);
    if (status != 0) {
        fprintf(stderr, "s3k: failed to enable MIDI mode (status=%d)\n", status);
        return status;
    }

    return 0;
}

void s3k_close(S3kClient *c) {
    scsi_midi_disable(&c->midi);
}

/* Internal: send an Akai SysEx and receive the response */
static int s3k_exchange(S3kClient *c, AkaiOpcode op,
                        const uint8_t *data, size_t data_len,
                        uint8_t *rx_buf, size_t *rx_len)
{
    auto msg = akai_build_sysex(c->channel, op, data, data_len);

    /* Send SysEx — ignore CHECK CONDITION, the device often returns
     * status 2 but still accepts the data */
    int send_status = scsi_midi_send(&c->midi, msg.data(), msg.size());

    usleep(200000);

    /* Receive response */
    int recv_status = scsi_midi_receive(&c->midi, rx_buf, rx_len, 30);
    if (recv_status == 0) return 0;

    /* If receive timed out and send failed, report the send error */
    if (recv_status > 0 && send_status != 0)
        fprintf(stderr, "s3k: send returned %d, no response from device\n", send_status);
    return recv_status;
}

int s3k_list_samples(S3kClient *c, char names[][S3K_NAME_LEN], int max) {
    uint8_t rx[S3K_MAX_RESPONSE];
    size_t rx_len = sizeof(rx);

    int status = s3k_exchange(c, OP_RSLIST, nullptr, 0, rx, &rx_len);
    if (status != 0) return -1;
    if (rx_len < 6) return -2;

    AkaiOpcode op;
    const uint8_t *payload;
    size_t payload_len;
    if (!akai_parse_response(rx, rx_len, &op, &payload, &payload_len))
        return -3;
    if (op != OP_SLIST) return -4;

    return akai_parse_name_list(payload, payload_len, names, max);
}

int s3k_list_programs(S3kClient *c, char names[][S3K_NAME_LEN], int max) {
    uint8_t rx[S3K_MAX_RESPONSE];
    size_t rx_len = sizeof(rx);

    int status = s3k_exchange(c, OP_RPLIST, nullptr, 0, rx, &rx_len);
    if (status != 0) return -1;
    if (rx_len < 6) return -2;

    AkaiOpcode op;
    const uint8_t *payload;
    size_t payload_len;
    if (!akai_parse_response(rx, rx_len, &op, &payload, &payload_len))
        return -3;
    if (op != OP_PLIST) return -4;

    return akai_parse_name_list(payload, payload_len, names, max);
}

int s3k_fetch_sample_header(S3kClient *c, int sample_num, S3kSampleHeader *hdr) {
    /* Request: RSDATA with sample number as nibble pair */
    uint8_t req_data[2];
    byte_to_nibbles(sample_num & 0xFF, req_data);

    uint8_t rx[S3K_MAX_RESPONSE];
    size_t rx_len = sizeof(rx);

    int status = s3k_exchange(c, OP_RSDATA, req_data, 2, rx, &rx_len);
    fprintf(stderr, "s3k: RSDATA: status=%d rx_len=%zu\n", status, rx_len);
    if (rx_len > 0) {
        fprintf(stderr, "  raw:");
        for (size_t di = 0; di < rx_len && di < 32; di++) fprintf(stderr, " %02x", rx[di]);
        if (rx_len > 32) fprintf(stderr, " ...(total %zu)", rx_len);
        fprintf(stderr, "\n");
        /* Find F7 */
        for (size_t fi = 0; fi < rx_len; fi++) {
            if (rx[fi] == 0xF7) { fprintf(stderr, "  F7 found at offset %zu\n", fi); break; }
            if (fi == rx_len - 1) fprintf(stderr, "  NO F7 in buffer!\n");
        }
    }
    if (status != 0) return -1;
    if (rx_len < 6) return -2;

    AkaiOpcode op;
    const uint8_t *payload;
    size_t payload_len;
    if (!akai_parse_response(rx, rx_len, &op, &payload, &payload_len))
        return -2;
    if (op == OP_REPLY) return -3; /* Error response */
    if (op != OP_SDATA) return -4;

    /* Parse nibblized sample header.
     * Payload starts with sample_number (2 nibbles), then header data. */
    int off = 2; /* skip sample number nibbles */

    /* Debug: print raw response */
    fprintf(stderr, "s3k: RSDATA response: %zu bytes, op=0x%02x\n", payload_len, (int)op);
    fprintf(stderr, "  raw:");
    for (size_t di = 0; di < payload_len && di < 40; di++)
        fprintf(stderr, " %02x", payload[di]);
    if (payload_len > 40) fprintf(stderr, " ...");
    fprintf(stderr, "\n");

    memset(hdr, 0, sizeof(*hdr));

    /* SHIDENT (1 byte = 2 nibbles) */
    if (off + 2 > (int)payload_len) return -5;
    uint8_t ident = read_nibble_byte(payload, &off);
    (void)ident; /* expected: 3 */

    /* SBANDW (1 byte) */
    hdr->bandwidth = read_nibble_byte(payload, &off);

    /* SPITCH (1 byte) */
    hdr->pitch = read_nibble_byte(payload, &off);

    /* SHNAME (12 bytes = 24 nibbles) */
    uint8_t name_raw[12];
    for (int i = 0; i < 12; i++)
        name_raw[i] = read_nibble_byte(payload, &off);
    akai_decode_name(name_raw, 12, hdr->name);

    /* Skip: SSRVLD (1), SLOOPS (1) = 4 nibbles */
    if (off + 4 <= (int)payload_len) {
        read_nibble_byte(payload, &off); /* SSRVLD */
        hdr->loop_count = read_nibble_byte(payload, &off);
    }

    /* SPTYPE (1 byte) */
    if (off + 2 <= (int)payload_len)
        hdr->play_type = read_nibble_byte(payload, &off);

    /* STUNO (2 bytes) — skip */
    if (off + 4 <= (int)payload_len) read_nibble_u16(payload, &off);

    /* SLOCAT (4 bytes) — skip */
    if (off + 8 <= (int)payload_len) read_nibble_u32(payload, &off);

    /* SLNGTH (4 bytes) */
    if (off + 8 <= (int)payload_len)
        hdr->length = read_nibble_u32(payload, &off);

    /* SSTART (4 bytes) */
    if (off + 8 <= (int)payload_len)
        hdr->start = read_nibble_u32(payload, &off);

    /* SMPEND (4 bytes) */
    if (off + 8 <= (int)payload_len)
        hdr->end = read_nibble_u32(payload, &off);

    /* Skip loop data: 4 loops × (LOOPAT:4 + LLNGTH:6 + LDWELL:2) = 48 bytes = 96 nibbles */
    /* Skip SLXY fields: 4 × 12 bytes = 48 bytes = 96 nibbles */
    /* Then: SSRATE (2 bytes = 4 nibbles) */
    int remaining_skip = 96 + 96; /* loops + SLXY */
    if (off + remaining_skip + 4 <= (int)payload_len) {
        off += remaining_skip;
        hdr->sample_rate = read_nibble_u16(payload, &off);
    }

    return 0;
}

int s3k_command(S3kClient *c, AkaiOpcode op, const uint8_t *data, size_t data_len,
                uint8_t *response, size_t *response_len)
{
    int status = s3k_exchange(c, op, data, data_len, response, response_len);
    if (status != 0) return status;

    /* Strip SysEx envelope, return just the payload */
    AkaiOpcode resp_op;
    const uint8_t *payload;
    size_t payload_len;
    if (!akai_parse_response(response, *response_len, &resp_op, &payload, &payload_len))
        return -1;

    memmove(response, payload, payload_len);
    *response_len = payload_len;
    return 0;
}
