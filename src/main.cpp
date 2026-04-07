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
#include "scsi_midi.h"
#include "akai_sysex.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

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
        "  program-header N     Show program header for program N\n"
        "  keygroup P K         Show keygroup K of program P\n"
        "  download-sample N [file.wav]  Download sample N as WAV\n"
        "  upload-sample N file.wav      Upload WAV to sample N\n"
        "  delete-sample N               Delete sample N\n"
        "  clone-program SRC DST         Clone program SRC to slot DST\n"
        "  delete-program N              Delete program N\n"
        "  status                        Show device overview\n"
        "  raw OPCODE [DATA...]          Send raw Akai SysEx\n",
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

static int cmd_program_header(S3kClient *c, int program_num) {
    S3kProgramHeader hdr;
    int status = s3k_fetch_program_header(c, program_num, &hdr);
    if (status != 0) {
        fprintf(stderr, "Failed to fetch program header %d (error=%d)\n", program_num, status);
        return 1;
    }

    printf("Program %d:\n", program_num);
    printf("  Name:        %s\n", hdr.name);
    printf("  MIDI Prog:   %d\n", hdr.midi_program);
    if (hdr.midi_channel == 255)
        printf("  MIDI Chan:   OMNI\n");
    else
        printf("  MIDI Chan:   %d\n", hdr.midi_channel + 1);
    printf("  Keygroups:   %d\n", hdr.num_keygroups);
    printf("  Polyphony:   %d\n", hdr.polyphony + 1);
    printf("  Play range:  %s - %s\n", note_name(hdr.play_lo), note_name(hdr.play_hi));
    printf("  Priority:    %s\n",
        hdr.priority == 0 ? "low" : hdr.priority == 1 ? "normal" :
        hdr.priority == 2 ? "high" : hdr.priority == 3 ? "hold" : "?");
    printf("  Output:      %d\n", hdr.output);
    printf("  Pan:         %d\n", hdr.pan);
    printf("  Loudness:    %d\n", hdr.loudness);
    return 0;
}

static int cmd_keygroup_header(S3kClient *c, int program_num, int kg_num) {
    S3kKeygroupHeader hdr;
    int status = s3k_fetch_keygroup_header(c, program_num, kg_num, &hdr);
    if (status != 0) {
        fprintf(stderr, "Failed to fetch keygroup %d:%d (error=%d)\n", program_num, kg_num, status);
        return 1;
    }

    printf("Keygroup %d:%d\n", program_num, kg_num);
    printf("  Key range:   %s - %s\n", note_name(hdr.lo_note), note_name(hdr.hi_note));
    printf("  Filter freq: %d\n", hdr.filter_freq);
    printf("  Zone 1:      %s (vel %d-%d)\n", hdr.zone1_sample[0] ? hdr.zone1_sample : "(empty)",
        hdr.zone1_lo_vel, hdr.zone1_hi_vel);
    if (hdr.zone2_sample[0]) printf("  Zone 2:      %s\n", hdr.zone2_sample);
    if (hdr.zone3_sample[0]) printf("  Zone 3:      %s\n", hdr.zone3_sample);
    if (hdr.zone4_sample[0]) printf("  Zone 4:      %s\n", hdr.zone4_sample);
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
    else if (strcmp(cmd, "program-header") == 0 && i < argc)
        result = cmd_program_header(&client, atoi(argv[i]));
    else if (strcmp(cmd, "keygroup") == 0 && i + 1 < argc) {
        int p = atoi(argv[i++]);
        result = cmd_keygroup_header(&client, p, atoi(argv[i]));
    }
    else if (strcmp(cmd, "status") == 0) {
        /* Overview: list programs with their keygroups and samples */
        char pnames[S3K_MAX_NAMES][S3K_NAME_LEN];
        char snames[S3K_MAX_NAMES][S3K_NAME_LEN];
        int pc = s3k_list_programs(&client, pnames, S3K_MAX_NAMES);
        int sc = s3k_list_samples(&client, snames, S3K_MAX_NAMES);
        printf("Programs (%d):\n", pc > 0 ? pc : 0);
        for (int j = 0; j < pc; j++) {
            S3kProgramHeader ph;
            if (s3k_fetch_program_header(&client, j, &ph) == 0)
                printf("  %d: %-12s  ch=%s kg=%d poly=%d range=%s-%s\n",
                    j, ph.name,
                    ph.midi_channel == 255 ? "OMNI" : "?",
                    ph.num_keygroups, ph.polyphony + 1,
                    note_name(ph.play_lo), note_name(ph.play_hi));
            else
                printf("  %d: %s\n", j, pnames[j]);
        }
        printf("\nSamples (%d):\n", sc > 0 ? sc : 0);
        for (int j = 0; j < sc; j++) {
            S3kSampleHeader sh;
            if (s3k_fetch_sample_header(&client, j, &sh) == 0)
                printf("  %d: %-12s  %uHz %u samples %s\n",
                    j, sh.name, sh.sample_rate, sh.length,
                    sh.play_type == 2 ? "oneshot" : "loop");
            else
                printf("  %d: %s\n", j, snames[j]);
        }
    }
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
    else if (strcmp(cmd, "upload-sample") == 0 && i + 1 < argc) {
        int sn = atoi(argv[i++]);
        const char *wavpath = argv[i];
        /* Read WAV file */
        FILE *wf = fopen(wavpath, "rb");
        if (!wf) { fprintf(stderr, "Cannot open %s\n", wavpath); result = 1; }
        else {
            uint8_t whdr[44];
            if (fread(whdr, 1, 44, wf) != 44 || memcmp(whdr, "RIFF", 4) != 0) {
                fprintf(stderr, "Not a valid WAV file\n"); result = 1;
            } else {
                int wav_rate = *(uint32_t *)(whdr + 24);
                int wav_bits = *(uint16_t *)(whdr + 34);
                int data_size = *(uint32_t *)(whdr + 40);
                int wav_samples = data_size / (wav_bits / 8);
                if (wav_bits != 16) {
                    fprintf(stderr, "Only 16-bit WAV supported (got %d-bit)\n", wav_bits);
                    result = 1;
                } else {
                    int16_t *pcm = (int16_t *)malloc(data_size);
                    fread(pcm, 2, wav_samples, wf);
                    printf("Uploading %s → sample %d (%d samples, %d Hz)\n",
                        wavpath, sn, wav_samples, wav_rate);
                    result = s3k_upload_sample(&client, sn, pcm, wav_samples, wav_rate);
                    free(pcm);
                    if (result == 0) printf("Upload complete.\n");
                }
            }
            fclose(wf);
        }
    }
    else if (strcmp(cmd, "delete-sample") == 0 && i < argc) {
        int sn = atoi(argv[i]);
        printf("Deleting sample %d...\n", sn);
        result = s3k_delete_sample(&client, sn);
        printf("%s\n", result == 0 ? "OK" : "Failed");
    }
    else if (strcmp(cmd, "clone-program") == 0 && i + 1 < argc) {
        int src = atoi(argv[i++]);
        int dst = atoi(argv[i]);
        printf("Cloning program %d → %d\n", src, dst);
        /* Fetch raw SysEx for source program */
        uint8_t raw[S3K_MAX_RESPONSE];
        size_t raw_len = sizeof(raw);
        int status2 = s3k_fetch_raw(&client, OP_RPDATA, src, raw, &raw_len);
        if (status2 != 0 || raw_len < 8) {
            fprintf(stderr, "Failed to fetch program %d (status=%d)\n", src, status2);
            result = 1;
        } else {
            /* Patch program number nibbles in the payload.
             * Payload starts at byte 5 (after F0 47 ch op 48).
             * First 2 nibbles of payload = program number (lo, hi). */
            uint8_t dst_nib[2];
            byte_to_nibbles(dst & 0xFF, dst_nib);
            raw[5] = dst_nib[0];
            raw[6] = dst_nib[1];
            /* Send as PDATA (write) — strip SysEx envelope */
            result = s3k_write_program_header(&client, dst, raw, raw_len);
            printf("  PDATA write: %s\n", result == 0 ? "OK" : "FAILED");

            if (result == 0) {
                /* Also clone keygroup 0 from source to dest */
                printf("  Cloning keygroup 0...\n");
                uint8_t kg_raw[S3K_MAX_RESPONSE];
                size_t kg_len = sizeof(kg_raw);
                /* Fetch keygroup: RKDATA needs program nibbles + kg number */
                usleep(300000);
                /* Use raw command path — build and send RKDATA request manually */
                uint8_t kg_req[3];
                byte_to_nibbles(src & 0xFF, kg_req);
                kg_req[2] = 0; /* keygroup 0 */
                auto kg_sysex = akai_build_sysex(client.channel, OP_RKDATA, kg_req, 3);
                scsi_midi_send(&client.midi, kg_sysex.data(), kg_sysex.size());
                usleep(200000);
                int kst = scsi_midi_receive(&client.midi, kg_raw, &kg_len, 30);
                printf("  RKDATA fetch: status=%d len=%zu first: %02x %02x %02x %02x\n",
                    kst, kg_len, kg_raw[0], kg_len>1?kg_raw[1]:0,
                    kg_len>2?kg_raw[2]:0, kg_len>3?kg_raw[3]:0);
                if (kst == 0 && kg_len > 7) {
                    /* Patch program number in the keygroup response */
                    uint8_t dst_nib2[2];
                    byte_to_nibbles(dst & 0xFF, dst_nib2);
                    kg_raw[5] = dst_nib2[0];
                    kg_raw[6] = dst_nib2[1];
                    /* Send as KDATA */
                    int kwst = s3k_write_keygroup_header(&client, dst, 0, kg_raw, kg_len);
                    printf("  KDATA write: %s (status=%d)\n", kwst == 0 ? "OK" : "FAILED", kwst);
                } else {
                    printf("  Failed to fetch source keygroup (status=%d)\n", kst);
                }

                usleep(500000);
                printf("  Verifying:\n");
                cmd_list(&client, "programs");
            } else {
                fprintf(stderr, "Write failed (status=%d)\n", result);
            }
        }
    }
    else if (strcmp(cmd, "delete-program") == 0 && i < argc) {
        int pn = atoi(argv[i]);
        printf("Deleting program %d...\n", pn);
        result = s3k_delete_program(&client, pn);
        printf("%s\n", result == 0 ? "OK" : "Failed");
    }
    else if (strcmp(cmd, "raw") == 0) result = cmd_raw(&client, argc - i, argv + i);
    else { fprintf(stderr, "Unknown command: %s\n", cmd); usage(argv[0]); result = 1; }

    s3k_close(&client);
    return result;
}
