/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT
 *
 * Opus implementation shared by AI and one-to-one device calls.
 */

#ifndef DEMO_AI_OPUS_H
#define DEMO_AI_OPUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEMO_AI_OPUS_SAMPLE_RATE    16000
#define DEMO_AI_OPUS_CHANNELS       1
#define DEMO_AI_OPUS_FRAME_MS       20

#ifndef DEMO_AI_PCM_SAMPLES_20MS
#define DEMO_AI_PCM_SAMPLES_20MS    320U
#endif
#ifndef DEMO_AI_PCM_BYTES_20MS
#define DEMO_AI_PCM_BYTES_20MS      640U
#endif

#define DEMO_AI_OPUS_FRAME_SAMPLES  DEMO_AI_PCM_SAMPLES_20MS
#define DEMO_AI_OPUS_FRAME_BYTES    DEMO_AI_PCM_BYTES_20MS
#define DEMO_AI_OPUS_BITRATE        24000
/* RFC 6716 maximum Opus packet size; output bitrate is server-controlled. */
#define DEMO_AI_OPUS_MAX_PACKET     1275

/* Share the codec implementation, never the mutable codec state.  Zero-init
 * each context and serialize its operations in its owning media worker.
 * In particular, AI prefetch/reset can run without an audio hardware lease. */
typedef struct {
    void *encoder;
    void *decoder;
} demo_opus_context_t;

#define DEMO_OPUS_CONTEXT_INIT { NULL, NULL }
#define DEMO_OPUS_MAX_DECODE_SAMPLES 1920 /* 120 ms at 16 kHz */

int demo_opus_init(demo_opus_context_t *context);
void demo_opus_reset(demo_opus_context_t *context);
void demo_opus_deinit(demo_opus_context_t *context);
int demo_opus_encode_20ms(demo_opus_context_t *context,
                         const int16_t pcm[DEMO_AI_OPUS_FRAME_SAMPLES],
                         uint8_t *packet, int packet_capacity);
/* NULL/0 requests decoder PLC for pcm_capacity_samples samples.  Callers
 * decide whether a timestamp gap really warrants concealment; no FEC is
 * enabled implicitly, and a rejected packet must not advance playback. */
int demo_opus_decode(demo_opus_context_t *context,
                     const uint8_t *packet, int packet_size,
                     int16_t *pcm, int pcm_capacity_samples);
/* Inspect a bounded mono packet's duration without touching codec state.
 * A positive result is samples at 16 kHz, at most 120 ms.  Decode still
 * validates the compressed frame contents before they can be played. */
int demo_opus_packet_samples(const uint8_t *packet, int packet_size);

/* Existing AI API: idempotent private, process-lifetime AI codec context. */
int demo_ai_opus_init(void);
void demo_ai_opus_reset(void);
void demo_ai_opus_deinit(void);

/* Returns encoded byte count, or a negative Opus/custom error. */
int demo_ai_opus_encode_20ms(
    const int16_t pcm[DEMO_AI_OPUS_FRAME_SAMPLES],
    uint8_t *packet,
    int packet_capacity);

/* Returns decoded samples per channel, or a negative Opus/custom error. */
int demo_ai_opus_decode(
    const uint8_t *packet,
    int packet_size,
    int16_t *pcm,
    int pcm_capacity_samples);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_AI_OPUS_H */
