/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT
 *
 * ITU-T G.711 A-law conversion for signed 16-bit linear PCM.
 */

#include "g711_codec.h"

static uint8_t demo_g711_alaw_find_segment(uint16_t magnitude)
{
#if TIRTC_F9_G711_FAST
    /* The first segment includes every magnitude through 31. For the
     * following segments, one ARM CLZ replaces the table-search loop. */
    return (uint8_t)(27U - (unsigned)__builtin_clz((uint32_t)magnitude | 31U));
#else
    static const uint16_t segment_end[8] = {
        0x001FU, 0x003FU, 0x007FU, 0x00FFU,
        0x01FFU, 0x03FFU, 0x07FFU, 0x0FFFU
    };
    uint8_t segment;

    for (segment = 0U; segment < 8U; ++segment)
    {
        if (magnitude <= segment_end[segment])
        {
            break;
        }
    }

    return segment;
#endif
}

uint8_t demo_g711_alaw_encode_sample(int16_t pcm_sample)
{
    uint16_t magnitude;
    uint8_t mask;
    uint8_t segment;
    uint8_t alaw;

    if (pcm_sample >= 0)
    {
        mask = 0xD5U;
        magnitude = (uint16_t)pcm_sample >> 3;
    }
    else
    {
        mask = 0x55U;
        magnitude = (uint16_t)(((uint32_t)(-(int32_t)pcm_sample) - 1U) >> 3);
    }

    segment = demo_g711_alaw_find_segment(magnitude);
    if (segment >= 8U)
    {
        return (uint8_t)(0x7FU ^ mask);
    }

    alaw = (uint8_t)(segment << 4);
    if (segment < 2U)
    {
        alaw |= (uint8_t)((magnitude >> 1) & 0x0FU);
    }
    else
    {
        alaw |= (uint8_t)((magnitude >> segment) & 0x0FU);
    }

    return (uint8_t)(alaw ^ mask);
}

int16_t demo_g711_alaw_decode_sample(uint8_t alaw_sample)
{
#if TIRTC_F9_G711_FAST
    /* ITU-T G.711 A-law code word to signed PCM, in unsigned byte order. */
    static const int16_t pcm_by_code[256] = {
         -5504,  -5248,  -6016,  -5760,  -4480,  -4224,  -4992,  -4736,
         -7552,  -7296,  -8064,  -7808,  -6528,  -6272,  -7040,  -6784,
         -2752,  -2624,  -3008,  -2880,  -2240,  -2112,  -2496,  -2368,
         -3776,  -3648,  -4032,  -3904,  -3264,  -3136,  -3520,  -3392,
        -22016, -20992, -24064, -23040, -17920, -16896, -19968, -18944,
        -30208, -29184, -32256, -31232, -26112, -25088, -28160, -27136,
        -11008, -10496, -12032, -11520,  -8960,  -8448,  -9984,  -9472,
        -15104, -14592, -16128, -15616, -13056, -12544, -14080, -13568,
          -344,   -328,   -376,   -360,   -280,   -264,   -312,   -296,
          -472,   -456,   -504,   -488,   -408,   -392,   -440,   -424,
           -88,    -72,   -120,   -104,    -24,     -8,    -56,    -40,
          -216,   -200,   -248,   -232,   -152,   -136,   -184,   -168,
         -1376,  -1312,  -1504,  -1440,  -1120,  -1056,  -1248,  -1184,
         -1888,  -1824,  -2016,  -1952,  -1632,  -1568,  -1760,  -1696,
          -688,   -656,   -752,   -720,   -560,   -528,   -624,   -592,
          -944,   -912,  -1008,   -976,   -816,   -784,   -880,   -848,
          5504,   5248,   6016,   5760,   4480,   4224,   4992,   4736,
          7552,   7296,   8064,   7808,   6528,   6272,   7040,   6784,
          2752,   2624,   3008,   2880,   2240,   2112,   2496,   2368,
          3776,   3648,   4032,   3904,   3264,   3136,   3520,   3392,
         22016,  20992,  24064,  23040,  17920,  16896,  19968,  18944,
         30208,  29184,  32256,  31232,  26112,  25088,  28160,  27136,
         11008,  10496,  12032,  11520,   8960,   8448,   9984,   9472,
         15104,  14592,  16128,  15616,  13056,  12544,  14080,  13568,
           344,    328,    376,    360,    280,    264,    312,    296,
           472,    456,    504,    488,    408,    392,    440,    424,
            88,     72,    120,    104,     24,      8,     56,     40,
           216,    200,    248,    232,    152,    136,    184,    168,
          1376,   1312,   1504,   1440,   1120,   1056,   1248,   1184,
          1888,   1824,   2016,   1952,   1632,   1568,   1760,   1696,
           688,    656,    752,    720,    560,    528,    624,    592,
           944,    912,   1008,    976,    816,    784,    880,    848,
    };
    return pcm_by_code[alaw_sample];
#else
    uint16_t magnitude;
    uint8_t segment;

    alaw_sample ^= 0x55U;
    magnitude = (uint16_t)(alaw_sample & 0x0FU) << 4;
    segment = (uint8_t)((alaw_sample & 0x70U) >> 4);

    if (segment == 0U)
    {
        magnitude += 8U;
    }
    else
    {
        magnitude += 0x0108U;
        if (segment > 1U)
        {
            magnitude <<= (uint8_t)(segment - 1U);
        }
    }

    if ((alaw_sample & 0x80U) != 0U)
    {
        return (int16_t)magnitude;
    }

    return (int16_t)(-(int32_t)magnitude);
#endif
}

size_t demo_g711_alaw_encode(const int16_t *pcm,
                             uint8_t *alaw,
                             size_t sample_count)
{
    size_t i;

    if (sample_count != 0U && (pcm == NULL || alaw == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < sample_count; ++i)
    {
        alaw[i] = demo_g711_alaw_encode_sample(pcm[i]);
    }

    return sample_count;
}

size_t demo_g711_alaw_decode(const uint8_t *alaw,
                             int16_t *pcm,
                             size_t sample_count)
{
    size_t i;

    if (sample_count != 0U && (alaw == NULL || pcm == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < sample_count; ++i)
    {
        pcm[i] = demo_g711_alaw_decode_sample(alaw[i]);
    }

    return sample_count;
}
