/*
 * s3k_client.h — Akai S3000XL SCSI client
 *
 * Standalone C++ client for communicating with an Akai S3000XL sampler
 * over SCSI via scsi2pi. Implements the full Akai SysEx protocol over
 * the MIDI-over-SCSI transport.
 */
#ifndef S3K_CLIENT_H
#define S3K_CLIENT_H

#include "scsi_midi.h"
#include "akai_sysex.h"
#include <stdint.h>

constexpr int S3K_MAX_NAMES = 256;
constexpr int S3K_NAME_LEN = 13;  /* 12 chars + null */
constexpr int S3K_MAX_RESPONSE = 8192;

struct S3kSampleHeader {
    char name[S3K_NAME_LEN];
    uint8_t bandwidth;     /* 0=10kHz, 1=20kHz */
    uint8_t pitch;         /* Original pitch (MIDI note 21-127) */
    uint32_t length;       /* Sample length in samples */
    uint32_t start;        /* Play start offset */
    uint32_t end;          /* Play end offset */
    uint16_t sample_rate;  /* Sample rate in Hz */
    uint8_t loop_count;    /* Number of loops (0-4) */
    uint8_t play_type;     /* 0=loop, 1=loop-release, 2=no-loop, 3=play-to-end */
};

struct S3kClient {
    ScsiMidi midi;
    uint8_t channel;
};

/* Initialize client: connects to scsi2pi, enables MIDI mode.
 * Returns 0 on success. */
int s3k_init(S3kClient *c, const char *host, int port, int target, int channel);

/* Clean shutdown: disables MIDI mode. */
void s3k_close(S3kClient *c);

/* List resident sample names. Returns count (negative on error). */
int s3k_list_samples(S3kClient *c, char names[][S3K_NAME_LEN], int max);

/* List resident program names. Returns count (negative on error). */
int s3k_list_programs(S3kClient *c, char names[][S3K_NAME_LEN], int max);

/* Fetch sample header for sample N. Returns 0 on success. */
int s3k_fetch_sample_header(S3kClient *c, int sample_num, S3kSampleHeader *hdr);

/* Download sample audio data via RSPACK/SDS.
 * Allocates and returns 16-bit PCM samples. Caller must free().
 * Returns number of samples (negative on error). */
int s3k_download_sample(S3kClient *c, int sample_num,
                        int16_t **samples_out, S3kSampleHeader *hdr_out);

/* Write 16-bit PCM samples to a WAV file. Returns 0 on success. */
int s3k_write_wav(const char *path, const int16_t *samples, int num_samples,
                  int sample_rate, int channels);

/* Send a raw Akai SysEx command. Returns response payload length (negative on error). */
int s3k_command(S3kClient *c, AkaiOpcode op, const uint8_t *data, size_t data_len,
                uint8_t *response, size_t *response_len);

#endif /* S3K_CLIENT_H */
