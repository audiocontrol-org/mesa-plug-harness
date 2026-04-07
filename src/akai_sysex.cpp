/*
 * akai_sysex.cpp — Akai S3000XL SysEx protocol implementation
 */
#include "akai_sysex.h"
#include <cstring>

std::vector<uint8_t> akai_build_sysex(uint8_t channel, AkaiOpcode op,
                                       const uint8_t *data, size_t len)
{
    std::vector<uint8_t> msg;
    msg.push_back(SYSEX_START);
    msg.push_back(AKAI_MFR_ID);
    msg.push_back(channel);
    msg.push_back(op);
    msg.push_back(S3K_DEVICE_ID);
    if (data && len > 0)
        msg.insert(msg.end(), data, data + len);
    msg.push_back(SYSEX_END);
    return msg;
}

bool akai_parse_response(const uint8_t *msg, size_t msg_len,
                         AkaiOpcode *op, const uint8_t **payload, size_t *payload_len)
{
    if (msg_len < 6) return false;
    if (msg[0] != SYSEX_START) return false;
    if (msg[1] != AKAI_MFR_ID) return false;

    /* Find the F7 end marker (may not be at the very end of the buffer) */
    size_t end_pos = msg_len;
    for (size_t i = 5; i < msg_len; i++) {
        if (msg[i] == SYSEX_END) { end_pos = i; break; }
    }
    if (end_pos >= msg_len) return false; /* No F7 found */

    *op = static_cast<AkaiOpcode>(msg[3]);
    *payload = msg + SYSEX_HEADER_SIZE;
    *payload_len = end_pos - SYSEX_HEADER_SIZE;
    return true;
}

bool akai_is_error(const uint8_t *msg, size_t msg_len)
{
    if (msg_len < 7) return false;
    if (msg[3] != OP_REPLY) return false;
    return msg[5] != 0; /* data byte 0 = OK, non-zero = error */
}

/* Akai character table:
 * 0-9: '0'-'9', 10: ' ', 11-36: 'A'-'Z', 37-62: 'a'-'z', 63: '#', 64: '+', 65: '-', 66: '.' */
char akai_to_ascii(uint8_t c)
{
    if (c <= 9) return '0' + c;
    if (c == 10) return ' ';
    if (c >= 11 && c <= 36) return 'A' + (c - 11);
    if (c >= 37 && c <= 62) return 'a' + (c - 37);
    if (c == 63) return '#';
    if (c == 64) return '+';
    if (c == 65) return '-';
    if (c == 66) return '.';
    return '?';
}

uint8_t ascii_to_akai(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ' ') return 10;
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 37 + (c - 'a');
    if (c == '#') return 63;
    if (c == '+') return 64;
    if (c == '-') return 65;
    if (c == '.') return 66;
    return 10; /* default to space */
}

void akai_decode_name(const uint8_t *raw, int len, char *out)
{
    for (int i = 0; i < len && i < 12; i++)
        out[i] = akai_to_ascii(raw[i]);
    out[len < 12 ? len : 12] = '\0';
    /* Trim trailing spaces */
    for (int i = 11; i >= 0 && out[i] == ' '; i--)
        out[i] = '\0';
}

void akai_encode_name(const char *ascii, uint8_t *nibble_data, int *offset) {
    for (int i = 0; i < 12; i++) {
        char c = (ascii && ascii[i]) ? ascii[i] : ' ';
        if (c == '\0') c = ' '; /* pad with spaces */
        write_nibble_byte(nibble_data, offset, ascii_to_akai(c));
    }
}

int akai_parse_name_list(const uint8_t *payload, size_t payload_len,
                         char names[][13], int max_names)
{
    if (payload_len < 2) return 0;

    int count = payload[0] | (payload[1] << 8); /* 16-bit LE */
    if (count > max_names) count = max_names;

    int offset = 2;
    int parsed = 0;
    for (int i = 0; i < count && offset + 12 <= (int)payload_len; i++) {
        akai_decode_name(payload + offset, 12, names[i]);
        offset += 12;
        parsed++;
    }
    return parsed;
}
