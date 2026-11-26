/* SPDX-License-Identifier: MIT
 * DEV-private Opus context, adapted from Lite 3ff19701. AI keeps its own codec.
 */
#ifndef CALLS_DEV_OPUS_CODEC_H
#define CALLS_DEV_OPUS_CODEC_H
#include <stddef.h>
#include <stdint.h>
#define CALLS_DEV_OPUS_SAMPLE_RATE 16000
#define CALLS_DEV_OPUS_CHANNELS 1
#define CALLS_DEV_OPUS_FRAME_SAMPLES 320U
#define CALLS_DEV_OPUS_BITRATE 24000
#define CALLS_DEV_OPUS_MAX_PACKET 1275
#define CALLS_DEV_OPUS_MAX_DECODE_SAMPLES 1920
/* Encoder belongs to DEV capture; decoder belongs to call_control. Start,
 * reset and destroy run only with both workers and RX producers quiescent. */
typedef struct { void *encoder; void *decoder; } calls_dev_opus_context_t;
#define CALLS_DEV_OPUS_CONTEXT_INIT { NULL, NULL }
int calls_dev_opus_init(calls_dev_opus_context_t *context);
void calls_dev_opus_reset(calls_dev_opus_context_t *context);
void calls_dev_opus_deinit(calls_dev_opus_context_t *context);
int calls_dev_opus_encode_20ms(calls_dev_opus_context_t *context,
    const int16_t pcm[CALLS_DEV_OPUS_FRAME_SAMPLES], uint8_t *packet, int packet_capacity);
int calls_dev_opus_decode(calls_dev_opus_context_t *context,
    const uint8_t *packet, int packet_size, int16_t *pcm, int pcm_capacity_samples);
/* Stateless packet inspection is safe in the bounded receive callback. */
int calls_dev_opus_packet_samples(const uint8_t *packet, int packet_size);
#endif
