/*
 * s3k-client — Command-line client for Akai S3000XL over SCSI
 *
 * Communicates with an S3000XL sampler via scsi2pi over TCP.
 *
 * Usage:
 *   s3k-client [options] <command>
 *
 * Options:
 *   --host HOST    scsi2pi host (default: 10.0.0.57)
 *   --port PORT    scsi2pi port (default: 6868)
 *   --target ID    SCSI target ID (default: 6)
 *   --channel CH   SysEx channel (default: 0)
 *
 * Commands:
 *   inquiry              Show SCSI INQUIRY data
 *   list-samples         List resident sample names
 *   list-programs        List resident program names
 *   sample-header N      Show sample header for sample N
 *   raw OPCODE [DATA...] Send raw Akai SysEx (hex opcode + data bytes)
 */
#include "s3k_client.h"
#include "scsi_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <command>\n\n"
        "Options:\n"
        "  --host HOST    scsi2pi host (default: 10.0.0.57)\n"
        "  --port PORT    scsi2pi port (default: 6868)\n"
        "  --target ID    SCSI target ID (default: 6)\n"
        "  --channel CH   SysEx channel (default: 0)\n\n"
        "Commands:\n"
        "  inquiry              Show SCSI INQUIRY data\n"
        "  list-samples         List resident sample names\n"
        "  list-programs        List resident program names\n"
        "  sample-header N      Show sample header for sample N\n"
        "  download-sample N [file.wav]  Download sample N as WAV\n"
        "  raw OPCODE [DATA...] Send raw Akai SysEx\n",
        prog);
}

static int cmd_inquiry(const char *host, int port, int target) {
    scsi_bridge_init(host, port);

    uint8_t cdb[6] = { 0x12, 0, 0, 0, 96, 0 };
    uint8_t buf[96] = {0};
    size_t buf_len = 96;
    int status = scsi_bridge_exec(target, 0, cdb, 6, nullptr, 0, buf, &buf_len, 10);
    if (status < 0) {
        fprintf(stderr, "INQUIRY failed (status=%d)\n", status);
        return 1;
    }

    printf("SCSI Target %d:\n", target);
    printf("  Device type:  0x%02x (%s)\n", buf[0] & 0x1F,
        (buf[0] & 0x1F) == 0x03 ? "Processor" : "Other");
    printf("  Vendor:       %.8s\n", buf + 8);
    printf("  Product:      %.16s\n", buf + 16);
    printf("  Revision:     %.4s\n", buf + 32);
    return 0;
}

static int cmd_list(S3kClient *c, const char *kind) {
    char names[S3K_MAX_NAMES][S3K_NAME_LEN];
    int count;

    if (strcmp(kind, "samples") == 0) {
        count = s3k_list_samples(c, names, S3K_MAX_NAMES);
    } else {
        count = s3k_list_programs(c, names, S3K_MAX_NAMES);
    }

    if (count < 0) {
        fprintf(stderr, "Failed to list %s (error=%d)\n", kind, count);
        return 1;
    }

    printf("%d %s:\n", count, kind);
    for (int i = 0; i < count; i++)
        printf("  %3d: %s\n", i, names[i]);
    return 0;
}

static const char *note_name(int midi_note) {
    static char buf[8];
    static const char *notes[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (midi_note < 21 || midi_note > 127) { snprintf(buf, sizeof(buf), "%d", midi_note); return buf; }
    snprintf(buf, sizeof(buf), "%s%d", notes[midi_note % 12], (midi_note / 12) - 1);
    return buf;
}

static int cmd_sample_header(S3kClient *c, int sample_num) {
    S3kSampleHeader hdr;
    int status = s3k_fetch_sample_header(c, sample_num, &hdr);
    if (status != 0) {
        fprintf(stderr, "Failed to fetch sample header %d (error=%d)\n", sample_num, status);
        return 1;
    }

    printf("Sample %d:\n", sample_num);
    printf("  Name:        %s\n", hdr.name);
    printf("  Bandwidth:   %s\n", hdr.bandwidth ? "20kHz" : "10kHz");
    printf("  Pitch:       %s (MIDI %d)\n", note_name(hdr.pitch), hdr.pitch);
    printf("  Length:      %u samples\n", hdr.length);
    printf("  Start:       %u\n", hdr.start);
    printf("  End:         %u\n", hdr.end);
    printf("  Sample rate: %u Hz\n", hdr.sample_rate);
    printf("  Loops:       %d\n", hdr.loop_count);
    printf("  Play type:   %d (%s)\n", hdr.play_type,
        hdr.play_type == 0 ? "loop" :
        hdr.play_type == 1 ? "loop-until-release" :
        hdr.play_type == 2 ? "no loop" :
        hdr.play_type == 3 ? "play to end" : "?");
    return 0;
}

static int cmd_raw(S3kClient *c, int argc, char *argv[]) {
    if (argc < 1) { fprintf(stderr, "raw: need opcode\n"); return 1; }

    int opcode = strtol(argv[0], nullptr, 16);
    uint8_t data[256];
    int data_len = 0;
    for (int i = 1; i < argc && data_len < 256; i++)
        data[data_len++] = strtol(argv[i], nullptr, 16);

    uint8_t response[S3K_MAX_RESPONSE];
    size_t response_len = sizeof(response);
    int status = s3k_command(c, static_cast<AkaiOpcode>(opcode),
                             data, data_len, response, &response_len);
    if (status != 0) {
        fprintf(stderr, "Command failed (status=%d)\n", status);
        return 1;
    }

    printf("Response (%zu bytes):", response_len);
    for (size_t i = 0; i < response_len; i++)
        printf(" %02x", response[i]);
    printf("\n");
    return 0;
}

int main(int argc, char *argv[]) {
    const char *host = "10.0.0.57";
    int port = 6868;
    int target = 6;
    int channel = 0;

    /* Parse options */
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) { host = argv[++i]; }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) { port = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) { target = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) { channel = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
        i++;
    }

    if (i >= argc) { usage(argv[0]); return 1; }
    const char *cmd = argv[i++];

    /* Inquiry doesn't need MIDI mode */
    if (strcmp(cmd, "inquiry") == 0)
        return cmd_inquiry(host, port, target);

    /* All other commands need the full client */
    S3kClient client;
    int status = s3k_init(&client, host, port, target, channel);
    if (status != 0) {
        fprintf(stderr, "Failed to connect to S3000XL at %s:%d target %d (status=%d)\n",
            host, port, target, status);
        return 1;
    }

    int result = 0;
    if (strcmp(cmd, "list-samples") == 0) result = cmd_list(&client, "samples");
    else if (strcmp(cmd, "list-programs") == 0) result = cmd_list(&client, "programs");
    else if (strcmp(cmd, "sample-header") == 0 && i < argc)
        result = cmd_sample_header(&client, atoi(argv[i]));
    else if (strcmp(cmd, "download-sample") == 0 && i < argc) {
        int sn = atoi(argv[i++]);
        const char *outpath = (i < argc) ? argv[i] : nullptr;
        int16_t *samples = nullptr;
        S3kSampleHeader hdr;
        int count = s3k_download_sample(&client, sn, &samples, &hdr);
        if (count <= 0) {
            fprintf(stderr, "Download failed (error=%d)\n", count);
            result = 1;
        } else {
            printf("Downloaded %d samples from \"%s\" (%u Hz)\n", count, hdr.name, hdr.sample_rate);
            char default_path[64];
            if (!outpath) {
                snprintf(default_path, sizeof(default_path), "sample_%d.wav", sn);
                outpath = default_path;
            }
            s3k_write_wav(outpath, samples, count, hdr.sample_rate, 1);
            free(samples);
        }
    }
    else if (strcmp(cmd, "raw") == 0) result = cmd_raw(&client, argc - i, argv + i);
    else { fprintf(stderr, "Unknown command: %s\n", cmd); usage(argv[0]); result = 1; }

    s3k_close(&client);
    return result;
}
