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
     * Payload starts with sample_number (2 nibbles), then header fields.
     * Each "byte" in the spec = 2 nibbles in the data stream.
     * Field widths from Akai S2800/S3000 SysEx specification. */
    int off = 2; /* skip sample number nibbles */
    memset(hdr, 0, sizeof(*hdr));

#define NEED(n) if (off + (n) > (int)payload_len) return -5
#define SKIP(n) off += (n) * 2  /* skip n bytes = n*2 nibbles */

    NEED(2); read_nibble_byte(payload, &off);          /* SHIDENT (1 byte) */
    NEED(2); hdr->bandwidth = read_nibble_byte(payload, &off);  /* SBANDW (1) */
    NEED(2); hdr->pitch = read_nibble_byte(payload, &off);      /* SPITCH (1) */

    /* SHNAME (12 bytes = 24 nibbles) */
    NEED(24);
    uint8_t name_raw[12];
    for (int i = 0; i < 12; i++)
        name_raw[i] = read_nibble_byte(payload, &off);
    akai_decode_name(name_raw, 12, hdr->name);

    NEED(2); read_nibble_byte(payload, &off);                   /* SSRVLD (1) */
    NEED(2); hdr->loop_count = read_nibble_byte(payload, &off); /* SLOOPS (1) */
    NEED(2); read_nibble_byte(payload, &off);                   /* SALOOP (1) */
    NEED(2); read_nibble_byte(payload, &off);                   /* SHLOOP (1) */
    NEED(2); hdr->play_type = read_nibble_byte(payload, &off);  /* SPTYPE (1) */
    NEED(4); read_nibble_u16(payload, &off);                    /* STUNO (2) */
    NEED(8); read_nibble_u32(payload, &off);                    /* SLOCAT (4) */
    NEED(8); hdr->length = read_nibble_u32(payload, &off);      /* SLNGTH (4) */
    NEED(8); hdr->start = read_nibble_u32(payload, &off);       /* SSTART (4) */
    NEED(8); hdr->end = read_nibble_u32(payload, &off);         /* SMPEND (4) */

    /* 4 loops × (LOOPAT:4 + LLNGTH:6 + LDWELL:2) = 4 × 12 = 48 bytes = 96 nibbles */
    NEED(96); SKIP(48);

    /* 4 × SLXY: 12 bytes each = 48 bytes = 96 nibbles */
    NEED(96); SKIP(48);

    /* SSPARE (1) + SWCOMM (1) + SSPAIR (2) = 4 bytes = 8 nibbles */
    NEED(8); SKIP(4);

    /* SSRATE (2 bytes = 4 nibbles) */
    NEED(4); hdr->sample_rate = read_nibble_u16(payload, &off);

#undef NEED
#undef SKIP
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
