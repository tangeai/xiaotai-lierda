/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT
 *
 * Opus 16 kHz mono/20 ms DEV adapter copied from Lite commit 3ff19701; context is never shared with AI.
 */

#include "dev_opus_codec.h"

#include <stddef.h>

#include "liot_log.h"
#include "opus.h"

#if TIRTC_DEV_OPUS_ENABLE
#define CALLS_DEV_OPUS_COMPLEXITY  3


static int opus_context_init(calls_dev_opus_context_t *context, const char *tag)
{
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    int error;
    int ret;

    if (context == NULL)
    {
        return OPUS_BAD_ARG;
    }
    if (context->encoder != NULL && context->decoder != NULL)
    {
        return 0;
    }

    calls_dev_opus_deinit(context);

    error = OPUS_OK;
    encoder = opus_encoder_create(CALLS_DEV_OPUS_SAMPLE_RATE,
                                   CALLS_DEV_OPUS_CHANNELS,
                                   OPUS_APPLICATION_VOIP,
                                   &error);
    context->encoder = encoder;
    if (encoder == NULL || error != OPUS_OK)
    {
        calls_dev_opus_deinit(context);
        liot_trace("[%s] encoder create failed ret=%d\r\n", tag, error);
        return (error < 0) ? error : OPUS_ALLOC_FAIL;
    }

    ret = opus_encoder_ctl(encoder,
                           OPUS_SET_BITRATE(CALLS_DEV_OPUS_BITRATE));
    if (ret == OPUS_OK)
    {
        ret = opus_encoder_ctl(encoder,
                               OPUS_SET_COMPLEXITY(CALLS_DEV_OPUS_COMPLEXITY));
    }
    if (ret == OPUS_OK)
    {
        ret = opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }
    if (ret != OPUS_OK)
    {
        liot_trace("[%s] encoder config failed ret=%d\r\n", tag, ret);
        calls_dev_opus_deinit(context);
        return ret;
    }

    error = OPUS_OK;
    decoder = opus_decoder_create(CALLS_DEV_OPUS_SAMPLE_RATE,
                                   CALLS_DEV_OPUS_CHANNELS,
                                   &error);
    context->decoder = decoder;
    if (decoder == NULL || error != OPUS_OK)
    {
        liot_trace("[%s] decoder create failed ret=%d\r\n", tag, error);
        calls_dev_opus_deinit(context);
        return (error < 0) ? error : OPUS_ALLOC_FAIL;
    }

    liot_trace("[%s] ready 16kHz/mono/20ms 24kbps complexity=%d\r\n",
               tag, CALLS_DEV_OPUS_COMPLEXITY);
    return 0;
}

int calls_dev_opus_init(calls_dev_opus_context_t *context)
{
    return opus_context_init(context, "DEV-OPUS");
}

void calls_dev_opus_reset(calls_dev_opus_context_t *context)
{
    if (context == NULL) return;
    if (context->encoder != NULL)
    {
        (void)opus_encoder_ctl((OpusEncoder *)context->encoder, OPUS_RESET_STATE);
    }
    if (context->decoder != NULL)
    {
        (void)opus_decoder_ctl((OpusDecoder *)context->decoder, OPUS_RESET_STATE);
    }
}

void calls_dev_opus_deinit(calls_dev_opus_context_t *context)
{
    if (context == NULL) return;
    if (context->encoder != NULL)
    {
        opus_encoder_destroy((OpusEncoder *)context->encoder);
        context->encoder = NULL;
    }
    if (context->decoder != NULL)
    {
        opus_decoder_destroy((OpusDecoder *)context->decoder);
        context->decoder = NULL;
    }
}

int calls_dev_opus_encode_20ms(
    calls_dev_opus_context_t *context,
    const int16_t pcm[CALLS_DEV_OPUS_FRAME_SAMPLES],
    uint8_t *packet,
    int packet_capacity)
{
    if (context == NULL || context->encoder == NULL ||
        pcm == NULL || packet == NULL ||
        packet_capacity <= 0)
    {
        return OPUS_BAD_ARG;
    }

    return opus_encode((OpusEncoder *)context->encoder,
                       pcm,
                       CALLS_DEV_OPUS_FRAME_SAMPLES,
                       packet,
                       packet_capacity);
}

int calls_dev_opus_decode(calls_dev_opus_context_t *context,
                        const uint8_t *packet,
                        int packet_size,
                        int16_t *pcm,
                        int pcm_capacity_samples)
{
    if (context == NULL || context->decoder == NULL ||
        packet_size < 0 ||
        (packet == NULL ? packet_size != 0 : packet_size == 0) ||
        pcm == NULL || pcm_capacity_samples <= 0)
    {
        return OPUS_BAD_ARG;
    }

    return opus_decode((OpusDecoder *)context->decoder,
                       packet,
                       packet_size,
                       pcm,
                       pcm_capacity_samples,
                       0);
}

int calls_dev_opus_packet_samples(const uint8_t *packet, int packet_size)
{
    int samples;

    if (packet == NULL || packet_size <= 0 ||
        packet_size > CALLS_DEV_OPUS_MAX_PACKET)
    {
        return OPUS_BAD_ARG;
    }
    if (opus_packet_get_nb_channels(packet) != CALLS_DEV_OPUS_CHANNELS)
    {
        return OPUS_INVALID_PACKET;
    }
    samples = opus_packet_get_nb_samples(packet, packet_size,
                                         CALLS_DEV_OPUS_SAMPLE_RATE);
    return (samples > 0 && samples <= CALLS_DEV_OPUS_MAX_DECODE_SAMPLES) ?
        samples : OPUS_INVALID_PACKET;
}

#endif /* TIRTC_DEV_OPUS_ENABLE */
