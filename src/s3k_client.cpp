/*
 * s3k_client.cpp — Akai S3000XL SCSI client implementation
 */
#include "s3k_client.h"
#include "scsi_bridge.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <algorithm>

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
/* Drain any pending MIDI data from the device (e.g., leftover SDS packets) */
static void s3k_drain(S3kClient *c) {
    uint8_t junk[4096];
    for (int i = 0; i < 200; i++) {
        uint8_t pcdb[6] = {0x0D, 0, 0, 0, 0, 0};
        uint8_t pb[3] = {0};
        size_t plen = 3;
        scsi_bridge_exec(c->midi.target_id, 0, pcdb, 6, NULL, 0, pb, &plen, 5);
        uint32_t avail = 0;
        if (plen >= 3) avail = ((uint32_t)pb[0]<<16)|((uint32_t)pb[1]<<8)|pb[2];
        if (avail == 0) break;
        /* Read and discard */
        size_t rlen = (avail < sizeof(junk)) ? avail : sizeof(junk);
        uint8_t rcdb[6] = {0x0E, 0, (uint8_t)((rlen>>16)&0xFF),
                           (uint8_t)((rlen>>8)&0xFF), (uint8_t)(rlen&0xFF), 0};
        scsi_bridge_exec(c->midi.target_id, 0, rcdb, 6, NULL, 0, junk, &rlen, 5);
    }
}

static int s3k_exchange(S3kClient *c, AkaiOpcode op,
                        const uint8_t *data, size_t data_len,
                        uint8_t *rx_buf, size_t *rx_len)
{
    /* Drain any leftover data (e.g., SDS packets from a previous transfer) */
    s3k_drain(c);

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

int s3k_fetch_program_header(S3kClient *c, int program_num, S3kProgramHeader *hdr) {
    uint8_t req_data[2];
    byte_to_nibbles(program_num & 0xFF, req_data);

    uint8_t rx[S3K_MAX_RESPONSE];
    size_t rx_len = sizeof(rx);
    int status = s3k_exchange(c, OP_RPDATA, req_data, 2, rx, &rx_len);
    if (status != 0) return -1;
    if (rx_len < 6) return -2;

    AkaiOpcode op;
    const uint8_t *payload;
    size_t payload_len;
    if (!akai_parse_response(rx, rx_len, &op, &payload, &payload_len))
        return -3;
    if (op == OP_REPLY) return -4;
    if (op != OP_PDATA) return -5;

    /* Payload: program_number (2 nibbles) + nibblized header data.
     * Field layout from S2800 SysEx spec (byte offsets, each = 2 nibbles): */
    /* Skip: program_number_lo (1 nibble), program_number_hi (1 nibble),
     * then 1 extra preamble byte (2 nibbles) per audiocontrol's offset=1 pattern */
    int off = 4;
    memset(hdr, 0, sizeof(*hdr));

#define NEED(n) if (off + (n) > (int)payload_len) return 0 /* partial parse OK */

    NEED(4); read_nibble_u16(payload, &off);                        /* KGRP1 (2) — skip */

    /* PRNAME (12 bytes = 24 nibbles) */
    NEED(24);
    uint8_t name_raw[12];
    for (int i = 0; i < 12; i++)
        name_raw[i] = read_nibble_byte(payload, &off);
    akai_decode_name(name_raw, 12, hdr->name);

    NEED(2); hdr->midi_program = read_nibble_byte(payload, &off);   /* PRGNUM (1) */
    NEED(2); hdr->midi_channel = read_nibble_byte(payload, &off);   /* PMCHAN (1) */
    NEED(2); hdr->polyphony = read_nibble_byte(payload, &off);      /* POLYPH (1) */
    NEED(2); hdr->priority = read_nibble_byte(payload, &off);       /* PRIORT (1) */
    NEED(2); hdr->play_lo = read_nibble_byte(payload, &off);        /* PLAYLO (1) */
    NEED(2); hdr->play_hi = read_nibble_byte(payload, &off);        /* PLAYHI (1) */
    NEED(2); read_nibble_byte(payload, &off);                        /* OSHIFT (1) — skip */
    NEED(2); hdr->output = read_nibble_byte(payload, &off);          /* OUTPUT (1) */
    NEED(2); read_nibble_byte(payload, &off);                        /* STEREO (1) — skip */
    NEED(2); hdr->pan = (int8_t)read_nibble_byte(payload, &off);    /* PANPOS (1, signed) */
    NEED(2); hdr->loudness = read_nibble_byte(payload, &off);        /* PRLOUD (1) */

    /* Skip to GROUPS: fields 25-36 are LFO, velocity, etc. = 13 bytes */
    int skip_to_groups = 13;
    if (off + skip_to_groups * 2 <= (int)payload_len) {
        off += skip_to_groups * 2;
        NEED(2); hdr->num_keygroups = read_nibble_byte(payload, &off); /* GROUPS (1) */
    }

#undef NEED
    return 0;
}

int s3k_fetch_keygroup_header(S3kClient *c, int program_num, int keygroup_num,
                               S3kKeygroupHeader *hdr) {
    /* Request: RKDATA with program number (2 nibbles) + keygroup number (1 raw byte) */
    uint8_t req_data[3];
    byte_to_nibbles(program_num & 0xFF, req_data);
    req_data[2] = keygroup_num;

    uint8_t rx[S3K_MAX_RESPONSE];
    size_t rx_len = sizeof(rx);
    int status = s3k_exchange(c, OP_RKDATA, req_data, 3, rx, &rx_len);
    if (status != 0) return -1;
    if (rx_len < 6) return -2;

    AkaiOpcode op;
    const uint8_t *payload;
    size_t payload_len;
    if (!akai_parse_response(rx, rx_len, &op, &payload, &payload_len))
        return -3;
    if (op == OP_REPLY) return -4;
    if (op != OP_KDATA) return -5;

    /* Payload: program_number (2 nibbles) + keygroup_number (1 raw byte) + nibblized data.
     * The KNUMBER is NOT nibblized — it's a single raw byte. */
    int off = 3; /* 2 nibbles for program + 1 raw byte for keygroup */
    memset(hdr, 0, sizeof(*hdr));

#define NEED(n) if (off + (n) > (int)payload_len) return 0

    NEED(2); read_nibble_byte(payload, &off);                        /* KGIDENT (1) — skip */
    NEED(4); read_nibble_u16(payload, &off);                         /* NXTKG (2) — skip */
    NEED(2); hdr->lo_note = read_nibble_byte(payload, &off);        /* LONOTE (1) */
    NEED(2); hdr->hi_note = read_nibble_byte(payload, &off);        /* HINOTE (1) */

    /* Skip to FILFRQ: OKTUNO(2) + KXFLT(1) + KXFLA(1) + ATTAK(1) + DECAY(1) +
     *   SUSTN(1) + RELSE(1) + AVELO(1) + ARELO(1) = 10 bytes */
    if (off + 20 <= (int)payload_len) off += 20;

    NEED(2); hdr->filter_freq = read_nibble_byte(payload, &off);    /* FILFRQ (1) */

    /* Skip to velocity zone 1 sample name: many envelope/modulation fields.
     * The exact offset depends on the S3000XL version. We'll read from the
     * known position relative to the start. Zone 1 starts at byte offset ~58. */
    /* For robustness, scan forward to find sample names from LONOTE position.
     * Zone sample names are at fixed offsets in the keygroup:
     * Zone 1 SNAME at byte 59, Zone 2 at 78, Zone 3 at 97, Zone 4 at 116.
     * Each zone block is 19 bytes. */
    int zone_base = 3 + 59 * 2; /* nibble offset for zone 1 sample name */
    if (zone_base + 24 <= (int)payload_len) {
        int zoff = zone_base;
        uint8_t n[12];
        for (int i = 0; i < 12; i++) n[i] = read_nibble_byte(payload, &zoff);
        akai_decode_name(n, 12, hdr->zone1_sample);

        /* Zone 1 velocity range: LOVEL at zone+12, HIVEL at zone+13 */
        int vel_off = zone_base + 12 * 2;
        if (vel_off + 4 <= (int)payload_len) {
            hdr->zone1_lo_vel = read_nibble_byte(payload, &vel_off);
            hdr->zone1_hi_vel = read_nibble_byte(payload, &vel_off);
        }
    }
    /* Zone 2-4 sample names (19-byte stride) */
    for (int z = 1; z < 4; z++) {
        int zoff = zone_base + z * 19 * 2;
        char (*dest)[S3K_NAME_LEN] = (z == 1) ? &hdr->zone2_sample :
                                      (z == 2) ? &hdr->zone3_sample :
                                                  &hdr->zone4_sample;
        if (zoff + 24 <= (int)payload_len) {
            uint8_t n[12];
            for (int i = 0; i < 12; i++) n[i] = read_nibble_byte(payload, &zoff);
            akai_decode_name(n, 12, *dest);
        }
    }

#undef NEED
    return 0;
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

/* ---- SDS Sample Download ---- */

/* Decode one 16-bit sample from 3 SDS-encoded bytes (MSB first, 7 bits per byte) */
static int16_t sds_decode_sample_16(const uint8_t *p) {
    uint16_t raw = ((uint16_t)p[0] << 9) | ((uint16_t)p[1] << 2) | (p[2] >> 5);
    return (int16_t)raw;
}

int s3k_download_sample(S3kClient *c, int sample_num,
                        int16_t **samples_out, S3kSampleHeader *hdr_out)
{
    /* Fetch header first to know sample length and rate */
    S3kSampleHeader hdr;
    int status = s3k_fetch_sample_header(c, sample_num, &hdr);
    if (status != 0) return -1;
    if (hdr_out) *hdr_out = hdr;

    uint32_t total_samples = hdr.length;
    if (total_samples == 0) return -2;

    fprintf(stderr, "Downloading sample %d (%s): %u samples @ %u Hz\n",
        sample_num, hdr.name, total_samples, hdr.sample_rate);

    /* Build RSPACK request:
     * F0 47 ch 0C 48 <sn_lo:7> <sn_hi:7> <off:4×7> <cnt:4×7> <type> <spare> F7 */
    uint8_t rspack[18];
    rspack[0] = SYSEX_START;
    rspack[1] = AKAI_MFR_ID;
    rspack[2] = c->channel;
    rspack[3] = OP_RSPACK;
    rspack[4] = S3K_DEVICE_ID;
    rspack[5] = sample_num & 0x7F;
    rspack[6] = (sample_num >> 7) & 0x7F;
    /* Offset = 0 (4 bytes, 7-bit encoding) */
    rspack[7] = rspack[8] = rspack[9] = rspack[10] = 0;
    /* Count (4 bytes, 7-bit LE encoding) */
    uint32_t cnt = total_samples;
    rspack[11] = cnt & 0x7F;
    rspack[12] = (cnt >> 7) & 0x7F;
    rspack[13] = (cnt >> 14) & 0x7F;
    rspack[14] = (cnt >> 21) & 0x7F;
    rspack[15] = 0x01; /* type */
    rspack[16] = 0x00; /* spare */
    rspack[17] = SYSEX_END;

    /* Send RSPACK */
    status = scsi_midi_send(&c->midi, rspack, sizeof(rspack));
    /* Send may CHECK CONDITION — continue anyway */

    /* Allocate output buffer */
    int16_t *samples = (int16_t *)calloc(total_samples, sizeof(int16_t));
    if (!samples) return -3;

    /* Accumulation buffer for SCSI reads — SDS packets may span multiple reads */
    std::vector<uint8_t> accum;
    accum.reserve(8192);
    uint32_t samples_received = 0;

    auto process_sds_message = [&](const uint8_t *msg, size_t len) {
        fprintf(stderr, "  MSG [%zu bytes]: %02x %02x %02x %02x %02x\n",
            len, msg[0], len>1?msg[1]:0, len>2?msg[2]:0, len>3?msg[3]:0, len>4?msg[4]:0);
        /* SDS Dump Header: F0 7E ch 01 ... */
        if (len >= 21 && msg[1] == 0x7E && msg[3] == 0x01) {
            fprintf(stderr, "  SDS Dump Header received\n");
            uint8_t ack[] = { 0xF0, 0x7E, c->channel, 0x7F, 0x00, 0xF7 };
            scsi_midi_send(&c->midi, ack, sizeof(ack));
            return;
        }
        /* SDS Data Packet: F0 7E ch 02 pp <120 data> cc F7 */
        if (len >= 7 && msg[1] == 0x7E && msg[3] == 0x02) {
            int pkt_num = msg[4];
            const uint8_t *data = msg + 5;
            int data_bytes = (int)len - 7;
            if (data_bytes > 120) data_bytes = 120;
            int pkt_samples = data_bytes / 3;
            for (int i = 0; i < pkt_samples && samples_received < total_samples; i++)
                samples[samples_received++] = sds_decode_sample_16(data + i * 3);
            /* ACK — must succeed for closed-loop SDS */
            uint8_t ack[] = { 0xF0, 0x7E, c->channel, 0x7F,
                              (uint8_t)(pkt_num & 0x7F), 0xF7 };
            int ack_status = scsi_midi_send(&c->midi, ack, sizeof(ack));
            fprintf(stderr, "  Pkt %d: %u/%u samples (ack=%d)\n",
                pkt_num, samples_received, total_samples, ack_status);
            return;
        }
        /* Akai SysEx */
        if (len >= 6 && msg[1] == 0x47) {
            fprintf(stderr, "  Akai SysEx opcode=0x%02x (%zu bytes)\n", msg[3], len);
            return;
        }
    };

    for (int round = 0; round < 5000 && samples_received < total_samples; round++) {
        /* Raw poll (CDB 0x0D) + read (CDB 0x0E) — don't use scsi_midi_receive
         * which stops at F7 and may discard remaining buffered packets */
        uint32_t avail = 0;
        for (int poll = 0; poll < 20; poll++) {
            uint8_t pcdb[6] = {0x0D, 0, 0, 0, 0, 0};
            uint8_t pb[3] = {0};
            size_t plen = 3;
            scsi_bridge_exec(c->midi.target_id, 0, pcdb, 6, NULL, 0, pb, &plen, 10);
            if (plen >= 3) avail = ((uint32_t)pb[0]<<16)|((uint32_t)pb[1]<<8)|pb[2];
            if (avail > 0) break;
            usleep(100000);
        }
        if (avail == 0) {
            if (samples_received >= total_samples) break;
            fprintf(stderr, "\n  Receive timeout after %u/%u samples\n",
                samples_received, total_samples);
            break;
        }
        /* Read ALL available bytes at once */
        uint8_t rx[8192];
        size_t rx_len = (avail < sizeof(rx)) ? avail : sizeof(rx);
        uint8_t rcdb[6] = {0x0E, 0, (uint8_t)((rx_len>>16)&0xFF),
                           (uint8_t)((rx_len>>8)&0xFF), (uint8_t)(rx_len&0xFF), 0};
        scsi_bridge_exec(c->midi.target_id, 0, rcdb, 6, NULL, 0, rx, &rx_len, 10);
        if (rx_len == 0) continue;

        /* Append to accumulation buffer */
        accum.insert(accum.end(), rx, rx + rx_len);

        /* Extract complete SysEx messages (F0...F7) from the buffer */
        while (true) {
            /* Find F0 */
            auto start = std::find(accum.begin(), accum.end(), 0xF0);
            if (start == accum.end()) { accum.clear(); break; }
            /* Discard bytes before F0 */
            if (start != accum.begin())
                accum.erase(accum.begin(), start);
            /* Find F7 after F0 */
            auto end = std::find(accum.begin() + 1, accum.end(), 0xF7);
            if (end == accum.end()) break; /* Incomplete message, wait for more data */
            size_t msg_len = (end - accum.begin()) + 1;
            process_sds_message(accum.data(), msg_len);
            accum.erase(accum.begin(), accum.begin() + msg_len);
        }
    }
    fprintf(stderr, "\n");

    *samples_out = samples;
    return (int)samples_received;
}

/* ---- WAV writer ---- */

int s3k_write_wav(const char *path, const int16_t *samples, int num_samples,
                  int sample_rate, int channels)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot create %s\n", path); return -1; }

    int data_size = num_samples * channels * 2; /* 16-bit */
    int file_size = 44 + data_size;

    /* WAV header */
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    *(uint32_t *)(hdr + 4) = file_size - 8;
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    *(uint32_t *)(hdr + 16) = 16; /* chunk size */
    *(uint16_t *)(hdr + 20) = 1;  /* PCM */
    *(uint16_t *)(hdr + 22) = channels;
    *(uint32_t *)(hdr + 24) = sample_rate;
    *(uint32_t *)(hdr + 28) = sample_rate * channels * 2; /* byte rate */
    *(uint16_t *)(hdr + 32) = channels * 2; /* block align */
    *(uint16_t *)(hdr + 34) = 16; /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    *(uint32_t *)(hdr + 40) = data_size;

    fwrite(hdr, 1, 44, f);
    fwrite(samples, 2, num_samples * channels, f);
    fclose(f);

    fprintf(stderr, "Wrote %s (%d samples, %d Hz, %d bytes)\n",
        path, num_samples, sample_rate, file_size);
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
