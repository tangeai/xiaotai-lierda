/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT
 *
 * Shared ES8311 audio adapter for TiRTC AI/WX/DEV/LIVE media paths.
 */

#ifndef DEMO_AI_AUDIO_H
#define DEMO_AI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEMO_AI_AUDIO_SAMPLE_RATE        16000U
#define DEMO_AI_AUDIO_CHANNELS           1U
#define DEMO_AI_AUDIO_BITS_PER_SAMPLE    16U
#define DEMO_AI_AUDIO_FRAME_MS           20U

#ifndef DEMO_AI_PCM_SAMPLES_20MS
#define DEMO_AI_PCM_SAMPLES_20MS         320U
#endif
#ifndef DEMO_AI_PCM_BYTES_20MS
#define DEMO_AI_PCM_BYTES_20MS           640U
#endif

#define DEMO_AI_AUDIO_FRAME_SAMPLES      DEMO_AI_PCM_SAMPLES_20MS
#define DEMO_AI_AUDIO_FRAME_BYTES        DEMO_AI_PCM_BYTES_20MS
#define DEMO_AI_AUDIO_WARMUP_MS          80U
#define DEMO_AI_AUDIO_SESSION_WARMUP_MS  1280U

#if DEMO_AI_AUDIO_FRAME_SAMPLES != 320U || DEMO_AI_AUDIO_FRAME_BYTES != 640U
#error "V04 local audio requires 16 kHz mono PCM16: 320 samples / 640 bytes"
#endif

#define DEMO_AI_AUDIO_LEVEL_MIN          0U
#define DEMO_AI_AUDIO_LEVEL_MAX          10U
#define DEMO_AI_AUDIO_DEFAULT_SPK_LEVEL  8U
#define DEMO_AI_AUDIO_DEFAULT_MIC_LEVEL  7U

/*
 * ES8311/I2S0 is a process-wide singleton.  A fixed owner plus generation
 * makes a lease immune to ABA reuse: delayed cleanup from an old session can
 * no longer stop or reconfigure the feature that acquired audio afterwards.
 * The structure is caller-owned and uses no dynamic allocation.
 */
typedef enum
{
    DEMO_AI_AUDIO_OWNER_NONE = 0,
    DEMO_AI_AUDIO_OWNER_AI,
    DEMO_AI_AUDIO_OWNER_WECHAT,
    DEMO_AI_AUDIO_OWNER_DEVICE,
    DEMO_AI_AUDIO_OWNER_LIVE,
#ifdef HWDEMO_GROUP_ROOM_EN
    DEMO_AI_AUDIO_OWNER_GROUP_ROOM,
#endif
    DEMO_AI_AUDIO_OWNER_COUNT
} demo_ai_audio_owner_e;

typedef struct
{
    demo_ai_audio_owner_e owner;
    uint32_t generation;
} demo_ai_audio_lease_t;

#define DEMO_AI_AUDIO_LEASE_INIT \
    { DEMO_AI_AUDIO_OWNER_NONE, 0U }

#define DEMO_AI_AUDIO_ERR_INVALID_LEASE  (-30)
#define DEMO_AI_AUDIO_ERR_BUSY           (-31)
#define DEMO_AI_AUDIO_ERR_WARM_PARTIAL   (-32)

int demo_ai_audio_acquire(demo_ai_audio_owner_e owner,
                          demo_ai_audio_lease_t *lease);
/* BUSY retains ownership while a SDK call is in flight or stop is missing.
 * Stop new inputs, wait for the media worker, stop(), then release(). */
int demo_ai_audio_release(demo_ai_audio_lease_t *lease);
bool demo_ai_audio_lease_is_valid(const demo_ai_audio_lease_t *lease);

/*
 * Initializes ES8311 once. Repeated successful calls are harmless.
 * The display port must already have enabled the shared 3.3V rail.
 * There is deliberately no deinit API: the SDK record call can wait forever
 * and concurrent deinit is unsafe. Keep the lease until in-flight calls exit.
 */
int demo_ai_audio_init(const demo_ai_audio_lease_t *lease);
bool demo_ai_audio_is_ready(const demo_ai_audio_lease_t *lease);

/* Prepare one session with the requested levels in a single hardware commit.
 * This avoids the duplicate volume writes caused by init() followed by
 * set_levels(), while preserving the required warm-session Pause/Resume. */
int demo_ai_audio_prepare_session(const demo_ai_audio_lease_t *lease,
                                  uint8_t speaker_level,
                                  uint8_t mic_level);

/* Returns exactly 20 ms of signed 16-bit little-endian 16 kHz PCM. The
 * network paths downsample this to the negotiated 8 kHz A-law frame when
 * required. SDK receive has no timeout; never call on UI/network callbacks. */
int demo_ai_audio_record_20ms(
    const demo_ai_audio_lease_t *lease,
    int16_t pcm[DEMO_AI_AUDIO_FRAME_SAMPLES]);
/* Drop remaining cached audio after the AI layer suppresses a frame (for
 * mute/echo prevention), so it cannot emerge after suppression ends. No I/O;
 * call from the media worker after record returns. BUSY keeps ownership. */
int demo_ai_audio_discard_capture(const demo_ai_audio_lease_t *lease);
/* Same safe discard, plus the duration of all cached samples discarded since
 * the previous call, including set_levels() invalidation. Success consumes
 * this counter once (0 or multiples of 20 ms); a failure leaves it and the
 * output untouched. Session prepare/lease changes reset the counter. Keep
 * the media timestamp advancing for these samples; never upload old PCM. */
int demo_ai_audio_discard_capture_timed(const demo_ai_audio_lease_t *lease,
                                      uint32_t *discarded_ms);

/*
 * Queues PCM for asynchronous playback. Liot_AudioPlay() copies the input
 * into its internal queue before returning, so the caller may immediately
 * reuse pcm. Each submission is limited to even counts of 2..320 samples
 * (the F6D_A I2S DMA requires four-byte alignment); the AI worker
 * owns bounded buffering and pacing. A non-zero return reports an error.
 */
int demo_ai_audio_play(const demo_ai_audio_lease_t *lease,
                       const int16_t *pcm, uint32_t samples);
/* After prepare_session, asynchronously queue the first 1200 ms of the cold
 * DAC preamble while the caller connects to the service. No recording, sleep
 * or speech is performed. On success queued_ms reports accepted silent time
 * (1200, or 0 when already prepared/muted outside GROUP); include it in the playback tail.
 * The first play_warm still prepends 80 ms, for 1280 ms total DAC clocks.
 * Do not stop/reinitialize after prewarming: natural FINISH preserves codec
 * volume. GROUP also prewarms at L0 and retains credit across speaker-volume
 * changes; it must wait for this tail before joining. Other owners invalidate
 * early credit when the speaker level changes before speech.
 * On failure queued_ms=0; WARM_PARTIAL requires normal safe session cleanup. */
int demo_ai_audio_prewarm_session(const demo_ai_audio_lease_t *lease,
                                 uint32_t *queued_ms);
/* Begin a new playback burst with silent PA/codec settling time and the first
 * real frame in ONE copied SDK queue item. Separate tiny silence submissions
 * can drain during a UI redraw and restart the PA before speech arrives.
 * Call only at a new burst/after underrun, never for every frame. On success
 * queued_extra_ms receives the added silence: 1280 ms for a new session's
 * first burst unless prewarm_session already queued the first 1200 ms,
 * (eight 150 ms items plus the final 80 ms/speech item), 80 ms for
 * later bursts, zero for mute. Every SDK call fits one <=5120-byte item,
 * preserving accepted-silence accounting if a later call fails.
 * Every failure sets it to zero and leaves the caller's PCM unchanged.
 * WARM_PARTIAL means silent items were already accepted: end the session
 * through normal safe cleanup; do not retry a partly submitted preamble. */
int demo_ai_audio_play_warm(const demo_ai_audio_lease_t *lease,
                          const int16_t *pcm, uint32_t samples,
                          uint32_t *queued_extra_ms);
/* Advisory SDK FINISH flag, not a DMA resource-release barrier. */
bool demo_ai_audio_play_done(const demo_ai_audio_lease_t *lease);
/* Requests SDK stop; does not cancel a blocked record or synchronously drain
 * DMA. Mutes the codec first; never frees SDK buffers or deinitializes it. */
int demo_ai_audio_stop(const demo_ai_audio_lease_t *lease);

/* Levels 0..10; 0 is mute, values above 10 clamp to 10.
 * User levels 1..10 follow the upstream board demo's useful hardware range:
 * speaker 30..60, microphone ADC gain 7..10 and digital volume 160..230.
 * DEV/Room playback uses software volume 10 (unity) and codec volume 38..72
 * to avoid clipping decoded G.711 PCM. Other owners use
 * the saved user-level curve 30..60 for both software and codec volume.
 * A user mic level is not the raw ES8311 gain register value.
 * Call from the media control worker, never UI or SDK callbacks. */
int demo_ai_audio_set_levels(const demo_ai_audio_lease_t *lease,
                             uint8_t speaker_level, uint8_t mic_level);
int demo_ai_audio_get_levels(const demo_ai_audio_lease_t *lease,
                             uint8_t *speaker_level, uint8_t *mic_level);
int demo_ai_audio_apply_levels(const demo_ai_audio_lease_t *lease);

#ifdef __cplusplus
}
#endif

#endif /* DEMO_AI_AUDIO_H */
