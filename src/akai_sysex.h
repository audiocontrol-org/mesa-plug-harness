/*
 * akai_sysex.h — Akai S3000XL SysEx protocol
 *
 * Message format: F0 47 <channel> <opcode> 48 <data...> F7
 * Reference: https://lakai.sourceforge.net/docs/s2800_sysex.html
 */
#ifndef AKAI_SYSEX_H
#define AKAI_SYSEX_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

/* Akai manufacturer ID */
constexpr uint8_t AKAI_MFR_ID = 0x47;

/* S3000XL device ID byte */
constexpr uint8_t S3K_DEVICE_ID = 0x48;

/* SysEx framing */
constexpr uint8_t SYSEX_START = 0xF0;
constexpr uint8_t SYSEX_END = 0xF7;

/* SysEx header size: F0 47 ch op 48 */
constexpr int SYSEX_HEADER_SIZE = 5;

enum AkaiOpcode : uint8_t {
    OP_RSTAT  = 0x00,  /* Request S1000 status */
    OP_STAT   = 0x01,  /* S1000 status report */
    OP_RPLIST = 0x02,  /* Request program name list */
    OP_PLIST  = 0x03,  /* Program name list */
    OP_RSLIST = 0x04,  /* Request sample name list */
    OP_SLIST  = 0x05,  /* Sample name list */
    OP_RPDATA = 0x06,  /* Request program header */
    OP_PDATA  = 0x07,  /* Program header data */
    OP_RKDATA = 0x08,  /* Request keygroup header */
    OP_KDATA  = 0x09,  /* Keygroup header data */
    OP_RSDATA = 0x0A,  /* Request sample header */
    OP_SDATA  = 0x0B,  /* Sample header data */
    OP_RSPACK = 0x0C,  /* Request sample data packets */
    OP_ASPACK = 0x0D,  /* Accept sample data packets */
    OP_RDDATA = 0x0E,  /* Request drum settings */
    OP_DDATA  = 0x0F,  /* Drum settings data */
    OP_RMDATA = 0x10,  /* Request misc data */
    OP_MDATA  = 0x11,  /* Misc data */
    OP_DELP   = 0x12,  /* Delete program */
    OP_DELK   = 0x13,  /* Delete keygroup */
    OP_DELS   = 0x14,  /* Delete sample */
    OP_SETEX  = 0x15,  /* Set exclusive channel */
    OP_REPLY  = 0x16,  /* Command reply (0=OK, else error) */
};

/* Build an Akai SysEx message */
std::vector<uint8_t> akai_build_sysex(uint8_t channel, AkaiOpcode op,
                                       const uint8_t *data = nullptr, size_t len = 0);

/* Parse a response: extract opcode and payload pointer.
 * Returns false if message is invalid. */
bool akai_parse_response(const uint8_t *msg, size_t msg_len,
                         AkaiOpcode *op, const uint8_t **payload, size_t *payload_len);

/* Check if response is a REPLY error */
bool akai_is_error(const uint8_t *msg, size_t msg_len);

/* ---- Nibble encoding (Akai uses LE nibble pairs) ---- */
inline void byte_to_nibbles(uint8_t b, uint8_t out[2]) {
    out[0] = b & 0x0F;        /* low nibble first */
    out[1] = (b >> 4) & 0x0F; /* high nibble second */
}

inline uint8_t nibbles_to_byte(uint8_t lo, uint8_t hi) {
    return (hi << 4) | (lo & 0x0F);
}

/* Read a byte from nibble stream, advance offset by 2 */
inline uint8_t read_nibble_byte(const uint8_t *data, int *offset) {
    uint8_t v = nibbles_to_byte(data[*offset], data[*offset + 1]);
    *offset += 2;
    return v;
}

/* Read a 16-bit LE value from nibble stream (4 nibbles) */
inline uint16_t read_nibble_u16(const uint8_t *data, int *offset) {
    uint8_t lo = read_nibble_byte(data, offset);
    uint8_t hi = read_nibble_byte(data, offset);
    return lo | (hi << 8);
}

/* Read a 32-bit LE value from nibble stream (8 nibbles) */
inline uint32_t read_nibble_u32(const uint8_t *data, int *offset) {
    uint16_t lo = read_nibble_u16(data, offset);
    uint16_t hi = read_nibble_u16(data, offset);
    return lo | (hi << 16);
}

/* Write a byte as 2 nibbles into the stream */
inline void write_nibble_byte(uint8_t *data, int *offset, uint8_t val) {
    uint8_t nib[2];
    byte_to_nibbles(val, nib);
    data[*offset] = nib[0];
    data[*offset + 1] = nib[1];
    *offset += 2;
}

/* Write a 12-byte Akai name into nibble stream (24 nibbles) */
void akai_encode_name(const char *ascii, uint8_t *nibble_data, int *offset);

/* ---- Akai character encoding ---- */
char akai_to_ascii(uint8_t c);
uint8_t ascii_to_akai(char c);
void akai_decode_name(const uint8_t *raw, int len, char *out);

/* Parse a name list response (PLIST/SLIST).
 * Returns number of names parsed. Each name is null-terminated, 13 bytes. */
int akai_parse_name_list(const uint8_t *payload, size_t payload_len,
                         char names[][13], int max_names);

#endif /* AKAI_SYSEX_H */
