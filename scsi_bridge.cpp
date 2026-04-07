/*
 * scsi_bridge.cpp — Forward SCSI commands to scsi2pi over TCP.
 * Extracted from SheepShaver's scsi_s2p.cpp (hand-rolled protobuf, no dependencies).
 */
#include "scsi_bridge.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

/* ---- Protobuf encoding (hand-rolled) ---- */

static std::vector<uint8_t> encode_varint(uint64_t value)
{
    std::vector<uint8_t> buf;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) byte |= 0x80;
        buf.push_back(byte);
    } while (value);
    return buf;
}

static void pb_varint(std::vector<uint8_t> &out, int field, uint64_t value)
{
    auto tag = encode_varint((field << 3) | 0);
    out.insert(out.end(), tag.begin(), tag.end());
    auto val = encode_varint(value);
    out.insert(out.end(), val.begin(), val.end());
}

static void pb_bytes(std::vector<uint8_t> &out, int field, const uint8_t *data, size_t len)
{
    auto tag = encode_varint((field << 3) | 2);
    out.insert(out.end(), tag.begin(), tag.end());
    auto length = encode_varint(len);
    out.insert(out.end(), length.begin(), length.end());
    out.insert(out.end(), data, data + len);
}

static uint64_t decode_varint(const uint8_t *data, size_t len, size_t &pos)
{
    uint64_t val = 0;
    int shift = 0;
    while (pos < len) {
        uint8_t b = data[pos++];
        val |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return val;
}

struct PbField {
    int field_num;
    int wire_type;
    uint64_t varint_val;
    const uint8_t *bytes_ptr;
    size_t bytes_len;
};

static std::vector<PbField> pb_parse(const uint8_t *data, size_t len)
{
    std::vector<PbField> fields;
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = decode_varint(data, len, pos);
        PbField f;
        f.field_num = tag >> 3;
        f.wire_type = tag & 7;
        f.varint_val = 0;
        f.bytes_ptr = nullptr;
        f.bytes_len = 0;
        if (f.wire_type == 0) {
            f.varint_val = decode_varint(data, len, pos);
        } else if (f.wire_type == 2) {
            f.bytes_len = decode_varint(data, len, pos);
            f.bytes_ptr = data + pos;
            pos += f.bytes_len;
        } else if (f.wire_type == 5) {
            pos += 4;
        } else if (f.wire_type == 1) {
            pos += 8;
        } else {
            break;
        }
        fields.push_back(f);
    }
    return fields;
}

/* ---- TCP client ---- */

static const char *bridge_host = "s3k.local";
static int bridge_port = 6868;

void scsi_bridge_init(const char *host, int port)
{
    if (host) bridge_host = host;
    if (port > 0) bridge_port = port;
    fprintf(stderr, "scsi_bridge: target %s:%d\n", bridge_host, bridge_port);
}

static bool s2p_command(const std::vector<uint8_t> &payload, std::vector<uint8_t> &result)
{
    result.clear();

    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", bridge_port);
    if (getaddrinfo(bridge_host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "scsi_bridge: cannot resolve %s:%d\n", bridge_host, bridge_port);
        return false;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return false; }

    struct timeval tv = { 30, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "scsi_bridge: connect to %s:%d failed: %s\n",
            bridge_host, bridge_port, strerror(errno));
        freeaddrinfo(res);
        close(sock);
        return false;
    }
    freeaddrinfo(res);

    /* Send: "RASCSI" magic + 4-byte LE length + payload */
    const char *magic = "RASCSI";
    if (write(sock, magic, 6) != 6) { close(sock); return false; }

    uint32_t len = payload.size();
    uint8_t len_buf[4] = {
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF), (uint8_t)((len >> 24) & 0xFF)
    };
    if (write(sock, len_buf, 4) != 4) { close(sock); return false; }

    size_t written = 0;
    while (written < payload.size()) {
        ssize_t n = write(sock, payload.data() + written, payload.size() - written);
        if (n <= 0) { close(sock); return false; }
        written += n;
    }

    /* Read response: 4-byte LE length + payload */
    uint8_t resp_len_buf[4];
    size_t got = 0;
    while (got < 4) {
        ssize_t n = read(sock, resp_len_buf + got, 4 - got);
        if (n <= 0) { close(sock); return false; }
        got += n;
    }
    uint32_t resp_len = resp_len_buf[0] | (resp_len_buf[1] << 8) |
                        (resp_len_buf[2] << 16) | (resp_len_buf[3] << 24);

    result.resize(resp_len);
    got = 0;
    while (got < resp_len) {
        ssize_t n = read(sock, result.data() + got, resp_len - got);
        if (n <= 0) { close(sock); return false; }
        got += n;
    }

    close(sock);
    return true;
}

/* ---- Public API ---- */

int scsi_bridge_exec(int target_id, int target_lun,
                     const uint8_t *cdb, int cdb_len,
                     const uint8_t *data_out, size_t data_out_len,
                     uint8_t *data_in, size_t *data_in_len,
                     int timeout_sec)
{
    /* Build PbScsiRequest */
    std::vector<uint8_t> scsi_req;
    pb_varint(scsi_req, 1, target_id);
    pb_varint(scsi_req, 2, target_lun);
    pb_bytes(scsi_req, 3, cdb, cdb_len);
    if (data_out && data_out_len > 0)
        pb_bytes(scsi_req, 4, data_out, data_out_len);
    if (data_in && data_in_len && *data_in_len > 0)
        pb_varint(scsi_req, 5, *data_in_len);
    if (timeout_sec > 0)
        pb_varint(scsi_req, 6, timeout_sec);

    /* Build PbCommand (SCSI_EXEC = 210) */
    std::vector<uint8_t> cmd;
    pb_varint(cmd, 1, 210);
    pb_bytes(cmd, 21, scsi_req.data(), scsi_req.size());

    /* Send and receive */
    std::vector<uint8_t> result;
    if (!s2p_command(cmd, result)) {
        fprintf(stderr, "scsi_bridge: transport error\n");
        return -1;
    }

    /* Parse PbResult → PbScsiResponse */
    int scsi_status = 0;  /* Default to success (GOOD) */
    bool got_result_status = false;
    auto fields = pb_parse(result.data(), result.size());
    for (auto &f : fields) {
        if (f.field_num == 1 && f.wire_type == 0) {
            got_result_status = true;
            if (!f.varint_val) {
                /* s2p reported failure — check for error message */
                for (auto &g : fields)
                    if (g.field_num == 2 && g.wire_type == 2)
                        fprintf(stderr, "scsi_bridge: error: %.*s\n", (int)g.bytes_len, g.bytes_ptr);
                return -2;
            }
        }
        if (f.field_num == 102 && f.wire_type == 2) {
            auto inner = pb_parse(f.bytes_ptr, f.bytes_len);
            for (auto &g : inner) {
                if (g.field_num == 1 && g.wire_type == 0)
                    scsi_status = g.varint_val;
                if (g.field_num == 3 && g.wire_type == 2 && data_in && data_in_len) {
                    size_t copy_len = g.bytes_len < *data_in_len ? g.bytes_len : *data_in_len;
                    memcpy(data_in, g.bytes_ptr, copy_len);
                    *data_in_len = copy_len;
                }
            }
        }
    }

    if (!got_result_status)
        return -3;  /* Couldn't parse response at all */

    return scsi_status;
}
