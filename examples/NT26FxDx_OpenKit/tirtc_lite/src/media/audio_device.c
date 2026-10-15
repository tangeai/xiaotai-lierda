/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT
 *
 * ES8311 audio adapter: I2C0/I2S0, PA GPIO11, 16 kHz mono-right PCM.
 */

#include "audio_device.h"

#include <stdint.h>
#include <string.h>

#include "liot_audio2.h"
#include "liot_gpio2.h"
#include "liot_log.h"
#include "liot_os.h"

/* Detailed audio tracing is opt-in; initialization and errors stay visible. */
#ifndef DEMO_AI_AUDIO_DEBUG
#define DEMO_AI_AUDIO_DEBUG 0
#endif
#define AUDIO_DEBUG_LOG(...) do { \
    if (DEMO_AI_AUDIO_DEBUG) liot_trace(__VA_ARGS__); \
} while (0)
#define AUDIO_RESULT_LOG(result, ...) do { \
    if ((result) != 0 || DEMO_AI_AUDIO_DEBUG) liot_trace(__VA_ARGS__); \
} while (0)

/*
 * Keep the UI at 1..10 while using only the useful hardware ranges found on
 * this board.  Speaker UI level 1 is the former level 5 (volume 30), and
 * microphone UI level 1 is the former level 6 (gain/volume 7/160).  The
 * remaining range is spread over all ten UI steps so every step still has a
 * measurable effect.
 */
static const uint8_t s_speaker_volume_by_level[DEMO_AI_AUDIO_LEVEL_MAX] = {
    30U, 33U, 37U, 40U, 43U, 47U, 50U, 53U, 57U, 60U
};

/* DEV receives full-range G.711 PCM. The SDK's software volume 10 is unity;
 * using the UI's codec value here first amplified PCM by 3..6x and clipped
 * it before the DAC attenuated it. Move that gain to the codec for DEV only.
 * These values preserve the former small-signal levels within 0.6 dB. */
static const uint8_t s_dev_codec_volume_by_level[DEMO_AI_AUDIO_LEVEL_MAX] = {
    38U, 41U, 46U, 50U, 53U, 57U, 61U, 64U, 69U, 72U
};

/* Keep L1 at 7/160; cap L10 at the previous L6 (9/199). */
static const uint8_t s_mic_gain_by_level[DEMO_AI_AUDIO_LEVEL_MAX] = {
    7U, 7U, 7U, 8U, 8U, 8U, 8U, 9U, 9U, 9U
};

static const uint8_t s_mic_volume_by_level[DEMO_AI_AUDIO_LEVEL_MAX] = {
    160U, 164U, 169U, 173U, 177U,
    182U, 186U, 190U, 195U, 199U
};

/* The driver DMA path requires aligned storage for capture. */
static __attribute__((aligned(16))) int16_t
    s_record_frame[DEMO_AI_AUDIO_FRAME_SAMPLES];

/* Work around isolated full-scale capture impulses observed in SDK Record
 * blocks. This does not repair the underlying capture fault or analog
 * clipping. Limit it to DEV/GROUP uplink, before their 16k-to-8k conversion.
 * Four quiet, consistent ORIGINAL neighbours are required: checking just
 * the adjacent pair would also remove legitimate 4 kHz waveform peaks.
 * The independently captured blocks must never borrow previous-frame data. */
static uint32_t lite_audio_guard_impulses(demo_ai_audio_owner_e owner,
                                        const int16_t *raw, int16_t *output)
{
    uint32_t cleaned = 0U;
    const uint32_t count = DEMO_AI_AUDIO_FRAME_SAMPLES;

    if (owner != DEMO_AI_AUDIO_OWNER_DEVICE
#ifdef HWDEMO_GROUP_ROOM_EN
        && owner != DEMO_AI_AUDIO_OWNER_GROUP_ROOM
#endif
        ) {
        return 0U;
    }
    if (count < 5U) {
        return 0U;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        uint32_t first, neighbours = 0U;
        int16_t local[4];
        int32_t value = raw[i];
        bool quiet = true;

        if (value > -28000 && value < 28000) {
            continue;
        }
        first = i > 2U ? i - 2U : 0U;
        if (first > count - 5U) first = count - 5U;
        for (uint32_t j = first; j < first + 5U; ++j) {
            if (j == i) continue;
            if (raw[j] < -2000 || raw[j] > 2000) {
                quiet = false;
                break;
            }
            local[neighbours++] = raw[j];
        }
        if (!quiet) continue;

        if (i > 0U && i + 1U < count) {
            int32_t difference = (int32_t)raw[i - 1U] - raw[i + 1U];
            if (difference < -1000 || difference > 1000) continue;
            output[i] = (int16_t)(((int32_t)raw[i - 1U] + raw[i + 1U]) / 2);
        } else {
            /* Median of the four local samples at either block edge. */
            for (uint32_t j = 1U; j < 4U; ++j) {
                uint32_t k = j;
                int16_t item = local[j];
                while (k > 0U && local[k - 1U] > item) {
                    local[k] = local[k - 1U];
                    --k;
                }
                local[k] = item;
            }
            if ((int32_t)local[3] - local[0] > 1000) continue;
            output[i] = (int16_t)(((int32_t)local[1] + local[2]) / 2);
        }
        ++cleaned;
    }
    return cleaned;
}

/* Liot_AudioInit keeps the configuration pointer for later stop/deinit and
 * callback operations.  It must therefore outlive demo_ai_audio_init(). */
static Liot_AudHwConfig_t s_audio_config;

static volatile bool s_audio_ready;
static volatile bool s_play_done = true;
static volatile bool s_levels_dirty = true;
static volatile uint8_t s_speaker_level = DEMO_AI_AUDIO_DEFAULT_SPK_LEVEL;
static volatile uint8_t s_mic_level = DEMO_AI_AUDIO_DEFAULT_MIC_LEVEL;
static demo_ai_audio_owner_e s_audio_owner = DEMO_AI_AUDIO_OWNER_NONE;
static uint32_t s_audio_generation;
#if DEMO_AI_AUDIO_AEC_ENABLE
/* The first 3A TX starts the codec asynchronously and writes SDK defaults
 * after capture has already started. A clean application setting therefore
 * still needs a bounded-rate hardware check while the stream is active. */
#define AEC_LEVELS_CHECK_MS 100U
static uint32_t s_aec_levels_checked_at;
#endif

static bool demo_ai_audio_lease_valid_locked(
    const demo_ai_audio_lease_t *lease)
{
    return lease != NULL &&
           lease->owner != DEMO_AI_AUDIO_OWNER_NONE &&
           lease->owner == s_audio_owner &&
           lease->generation != 0U &&
           lease->generation == s_audio_generation;
}


#if DEMO_AI_AUDIO_AEC_ENABLE
/* Keep the caller's generation immutable across waits. Some feature owners
 * reuse their lease object when the next session starts. */
static int demo_ai_audio_apply_levels_impl(const demo_ai_audio_lease_t *lease,
                                          bool playback_service);
static bool demo_ai_audio_snapshot_lease(const demo_ai_audio_lease_t *lease,
                                        demo_ai_audio_lease_t *snapshot)
{
    bool valid;
    liot_rtos_enter_critical();
    valid = demo_ai_audio_lease_valid_locked(lease);
    if (valid) *snapshot = *lease;
    liot_rtos_exit_critical();
    return valid;
}
#include "audio_aec.inc"
#endif

bool demo_ai_audio_lease_is_valid(const demo_ai_audio_lease_t *lease)
{
    bool valid;

    liot_rtos_enter_critical();
    valid = demo_ai_audio_lease_valid_locked(lease);
    liot_rtos_exit_critical();
    return valid;
}

int demo_ai_audio_acquire(demo_ai_audio_owner_e owner,
                          demo_ai_audio_lease_t *lease)
{
    int ret = 0;
    demo_ai_audio_owner_e held_owner = DEMO_AI_AUDIO_OWNER_NONE;

    if (owner <= DEMO_AI_AUDIO_OWNER_NONE ||
        owner >= DEMO_AI_AUDIO_OWNER_COUNT || lease == NULL)
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }

    liot_rtos_enter_critical();
    if (s_audio_owner != DEMO_AI_AUDIO_OWNER_NONE)
    {
        ret = DEMO_AI_AUDIO_ERR_BUSY;
        held_owner = s_audio_owner;
    }
    else
    {
        s_audio_generation++;
        if (s_audio_generation == 0U)
        {
            s_audio_generation++;
        }
        s_audio_owner = owner;
        /* An unchanged UI level can still require another owner's gain map. */
        s_levels_dirty = true;
        lease->owner = owner;
        lease->generation = s_audio_generation;
    }
    liot_rtos_exit_critical();

    if (ret != 0)
    {
        liot_trace("[AI-AUDIO] acquire owner=%u rejected; held by=%u\r\n",
                   (unsigned int)owner, (unsigned int)held_owner);
    }
    else
    {
        AUDIO_DEBUG_LOG("[AI-AUDIO] acquired owner=%u generation=%u\r\n",
                   (unsigned int)owner,
                   (unsigned int)lease->generation);
    }
    return ret;
}

int demo_ai_audio_release(demo_ai_audio_lease_t *lease)
{
    demo_ai_audio_owner_e owner;
    uint32_t generation;
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
#endif

    liot_rtos_enter_critical();
    if (!demo_ai_audio_lease_valid_locked(lease))
    {
        liot_rtos_exit_critical();
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
#if DEMO_AI_AUDIO_AEC_ENABLE
    if (s_aec_retiring) {
        liot_rtos_exit_critical();
        return DEMO_AI_AUDIO_ERR_BUSY;
    }
    s_aec_retiring = true;
    snapshot = *lease;
    liot_rtos_exit_critical();
    /* Retire pending speaker data before making the singleton available. */
    (void)demo_ai_audio_stop(&snapshot);
    liot_rtos_enter_critical();
    if (!demo_ai_audio_lease_valid_locked(&snapshot) ||
        lease->owner != snapshot.owner || lease->generation != snapshot.generation) {
        liot_rtos_exit_critical();
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
#endif
    owner = lease->owner;
    generation = lease->generation;
    s_audio_owner = DEMO_AI_AUDIO_OWNER_NONE;
    lease->owner = DEMO_AI_AUDIO_OWNER_NONE;
    lease->generation = 0U;
#if DEMO_AI_AUDIO_AEC_ENABLE
    s_aec_retiring = false;
#endif
    liot_rtos_exit_critical();

    AUDIO_DEBUG_LOG("[AI-AUDIO] released owner=%u generation=%u\r\n",
               (unsigned int)owner, (unsigned int)generation);
    return 0;
}

static uint8_t demo_ai_audio_clamp_level(uint8_t level)
{
    if (level < DEMO_AI_AUDIO_LEVEL_MIN)
    {
        return DEMO_AI_AUDIO_LEVEL_MIN;
    }
    if (level > DEMO_AI_AUDIO_LEVEL_MAX)
    {
        return DEMO_AI_AUDIO_LEVEL_MAX;
    }
    return level;
}

static void demo_ai_audio_callback(Liot_AudEvent_e event, void *context)
{
    (void)context;

    if (event == L_AUD_EVT_FINISH ||
        event == L_AUD_EVT_ERROR ||
        event == L_AUD_EVT_CLOSE)
    {
        s_play_done = true;
    }
}

bool demo_ai_audio_is_ready(const demo_ai_audio_lease_t *lease)
{
    return demo_ai_audio_lease_is_valid(lease) && s_audio_ready
#if DEMO_AI_AUDIO_AEC_ENABLE
        && !s_aec_fault
#endif
        ;
}

static int demo_ai_audio_apply_levels_impl(const demo_ai_audio_lease_t *lease,
                                          bool playback_service)
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    if (!demo_ai_audio_snapshot_lease(lease, &snapshot))
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    lease = &snapshot;
#else
    (void)playback_service;
#endif
    uint8_t speaker_level;
    uint8_t mic_level;
    uint8_t speaker_volume;
    uint8_t codec_volume;
    uint8_t mic_gain;
    uint8_t mic_volume;
    Liot_AudErr_e sw_ret;
    Liot_AudErr_e codec_ret;
    Liot_AudErr_e mic_ret;

    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    if (!s_audio_ready)
    {
        return -1;
    }
#if DEMO_AI_AUDIO_AEC_ENABLE
    if (s_aec_stopping || s_aec_retiring || s_aec_stop_requested || s_aec_fault) return AEC_ERR_BUSY;
#endif
    if (!s_levels_dirty
#if DEMO_AI_AUDIO_AEC_ENABLE
        && (!s_aec_play_active || s_aec_primed < 3U ||
            (uint32_t)(liot_rtos_get_running_time() - s_aec_levels_checked_at) <
                AEC_LEVELS_CHECK_MS)
#endif
        )
    {
        return 0;
    }

#if DEMO_AI_AUDIO_AEC_ENABLE
    bool levels_locked = playback_service ?
        (s_aec_levels_mutex != NULL &&
         liot_rtos_mutex_lock(s_aec_levels_mutex, 0U) == 0) :
        aec_levels_lock(100U);
    if (!levels_locked) {
        /* Ordinary UI contention must not turn a healthy call into an audio
         * failure. The playback worker never waits here: retry next step so
         * a volume update cannot consume a 40-ms playback deadline. */
        return s_aec_stopping || s_aec_retiring || s_aec_fault ? AEC_ERR_BUSY : 0;
    }
    if (!demo_ai_audio_lease_is_valid(lease) || s_aec_stopping || s_aec_retiring ||
        s_aec_stop_requested || s_aec_fault) {
        (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
        return AEC_ERR_BUSY;
    }
    /* Stop can finish after the worker's lease snapshot without releasing
     * the lease. Only an explicit caller may resume that stopped session. */
    if (playback_service &&
        (!s_aec_running || !s_aec_play_active || s_aec_primed < 3U)) {
        (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
        return 0;
    }
#endif
    speaker_level = demo_ai_audio_clamp_level(s_speaker_level);
    mic_level = demo_ai_audio_clamp_level(s_mic_level);

    speaker_volume = s_speaker_volume_by_level[speaker_level - 1U];
    codec_volume = speaker_volume;
    if (lease->owner == DEMO_AI_AUDIO_OWNER_DEVICE)
    {
        speaker_volume = 10U;
        codec_volume = s_dev_codec_volume_by_level[speaker_level - 1U];
    }
    mic_gain = s_mic_gain_by_level[mic_level - 1U];
    mic_volume = s_mic_volume_by_level[mic_level - 1U];

#if DEMO_AI_AUDIO_AEC_ENABLE
    if (!s_levels_dirty) {
        int actual = -1;
        /* ES8311 SetVolume writes floor(255*v/100); GetVolume converts
         * that register back with floor(reg*100/255). Comparing directly
         * with the requested level would rewrite most settings forever. */
        int expected = ((255 * (int)codec_volume / 100) * 100) / 255;
        Liot_AudErr_e read_ret;
        if (!s_aec_play_active || s_aec_primed < 3U ||
            (uint32_t)(liot_rtos_get_running_time() - s_aec_levels_checked_at) <
                AEC_LEVELS_CHECK_MS) {
            (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
            return 0;
        }
        read_ret = Liot_AudioGetCodecVolume(&actual);
        s_aec_levels_checked_at = liot_rtos_get_running_time();
        if (read_ret == L_AUD_ERR_SUCCESS && actual == expected) {
            (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
            return 0;
        }
        /* codecInit also overwrites mic gain/volume with 8/191. Restore
         * the complete setting. The SDK getter hides I2C read errors as
         * zero: a successful return is not proof of a successful read or
         * write, so subsequent 100-ms checks remain enabled after retry. */
        s_levels_dirty = true;
        AUDIO_RESULT_LOG(read_ret, "[AI-AUDIO] codec level drift: read=%d expected=%d ret=%d; restoring\r\n",
                   actual, expected, (int)read_ret);
    }
    /* A read may yield to Stop or an owner change. Never restore volume
     * after cancellation has published its gate; Stop mutes after us. */
    if (!demo_ai_audio_lease_is_valid(lease) || s_aec_stopping || s_aec_retiring ||
        s_aec_stop_requested || s_aec_fault) {
        (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
        return AEC_ERR_BUSY;
    }
    if (playback_service &&
        (!s_aec_running || !s_aec_play_active || s_aec_primed < 3U)) {
        (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
        return 0;
    }
#endif

    sw_ret = Liot_AudioSetVolume(speaker_volume);
    codec_ret = Liot_AudioSetCodecVolume(codec_volume);
    mic_ret = Liot_AudioSetMicVolume(mic_gain, mic_volume);

    if (sw_ret != L_AUD_ERR_SUCCESS ||
        codec_ret != L_AUD_ERR_SUCCESS ||
        mic_ret != L_AUD_ERR_SUCCESS)
    {
        s_levels_dirty = true;
#if DEMO_AI_AUDIO_AEC_ENABLE
        (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
#endif
        liot_trace("[AI-AUDIO] level apply failed ret=%d/%d/%d\r\n",
                   (int)sw_ret, (int)codec_ret, (int)mic_ret);
        return -2;
    }

    /* Stop may publish its gate while waiting for our codec writes. Preserve
     * dirty in that case so a later session restores the level after mute. */
    s_levels_dirty = false;
#if DEMO_AI_AUDIO_AEC_ENABLE
    s_aec_levels_checked_at = liot_rtos_get_running_time();
    if (s_aec_stopping) s_levels_dirty = true;
    (void)liot_rtos_mutex_unlock(s_aec_levels_mutex);
#endif
    AUDIO_DEBUG_LOG("[AI-AUDIO] levels applied: speaker L%u sw=%u codec=%u, "
               "mic L%u gain=%u volume=%u\r\n",
               (unsigned int)speaker_level,
               (unsigned int)speaker_volume,
               (unsigned int)codec_volume,
               (unsigned int)mic_level,
               (unsigned int)mic_gain,
               (unsigned int)mic_volume);
    /* Preserve a simultaneous UI update for the next service point. */
    if (speaker_level != s_speaker_level || mic_level != s_mic_level)
    {
        s_levels_dirty = true;
    }
    return 0;
}

int demo_ai_audio_apply_levels(const demo_ai_audio_lease_t *lease)
{
    return demo_ai_audio_apply_levels_impl(lease, false);
}

int demo_ai_audio_set_levels(const demo_ai_audio_lease_t *lease,
                             uint8_t speaker_level, uint8_t mic_level)
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    liot_rtos_enter_critical();
    if (!demo_ai_audio_lease_valid_locked(lease) || s_aec_retiring) {
        liot_rtos_exit_critical();
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    snapshot = *lease;
    lease = &snapshot;
    s_speaker_level = demo_ai_audio_clamp_level(speaker_level);
    s_mic_level = demo_ai_audio_clamp_level(mic_level);
    s_levels_dirty = true;
    liot_rtos_exit_critical();
#else
    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    s_speaker_level = demo_ai_audio_clamp_level(speaker_level);
    s_mic_level = demo_ai_audio_clamp_level(mic_level);
    s_levels_dirty = true;
#endif

    if (s_audio_ready)
    {
        return demo_ai_audio_apply_levels(lease);
    }
    return 0;
}

int demo_ai_audio_get_levels(const demo_ai_audio_lease_t *lease,
                             uint8_t *speaker_level, uint8_t *mic_level)
{
    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    if (speaker_level != NULL)
    {
        *speaker_level = s_speaker_level;
    }
    if (mic_level != NULL)
    {
        *mic_level = s_mic_level;
    }
    return 0;
}

int demo_ai_audio_init(const demo_ai_audio_lease_t *lease)
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    if (!demo_ai_audio_snapshot_lease(lease, &snapshot))
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    lease = &snapshot;
#endif
    Liot_AudErr_e ret;
#if !DEMO_AI_AUDIO_AEC_ENABLE
    Liot_AudErr_e pause_ret;
    Liot_AudErr_e resume_ret;
#endif

    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }

    if (s_audio_ready)
    {
#if DEMO_AI_AUDIO_AEC_ENABLE
        int reset_ret = aec_stop(lease);
        if (reset_ret != 0) return reset_ret;
        s_levels_dirty = true;
#else
        /* Liot_AudioStop() leaves F6D_A's internal playback state at STOP.
         * Pause then resume is the supported transition back to IDLE; without
         * it a later Liot_AudioPlay() accepts data but never starts DMA. */
        pause_ret = Liot_AudioPlayPause();
        resume_ret = Liot_AudioPlayResume();
        s_play_done = true;
        s_levels_dirty = true;
        if (pause_ret != L_AUD_ERR_SUCCESS ||
            resume_ret != L_AUD_ERR_SUCCESS)
        {
            liot_trace("[AI-AUDIO] session reset failed ret=%d/%d\r\n",
                       (int)pause_ret, (int)resume_ret);
            return -21;
        }
#endif
        if (demo_ai_audio_apply_levels(lease) != 0)
        {
            return -20;
        }
        AUDIO_DEBUG_LOG("[AI-AUDIO] session playback state reset\r\n");
        return 0;
    }

    Liot_AonPowerCtl(true);
    Liot_SetVoltage(L_DOMAIN_ALL, L_VOLT_3_30V);
    Liot_GpioInit(L_GPIO_25, L_IO_OUTPUT, L_IO_HIGH, NULL);

    memset(&s_audio_config, 0, sizeof(s_audio_config));
    s_audio_config.i2cNum = 0;
    s_audio_config.i2sNum = 0;
    s_audio_config.paGpioNum = 11;
    s_audio_config.codecType = L_AUD_ES8311;
    s_audio_config.channel = L_AUD_MONO_RIGHT;
    s_audio_config.role = L_AUD_ROLE_SLAVE;
    s_audio_config.mode = L_AUD_MODE_I2S;
    s_audio_config.frameSize = L_AUD_FRAMESIZE_16_16;
    s_audio_config.samples = L_AUD_16K_SAMPLES;
    s_audio_config.callback = demo_ai_audio_callback;
    s_audio_config.cbContext = NULL;
    s_audio_config.use3A = DEMO_AI_AUDIO_AEC_ENABLE;

#if DEMO_AI_AUDIO_AEC_ENABLE
    if (!s_aec_hw_ready) {
        /* Preserve manual gain/volume: enable AEC and ANS, keep AGC bypassed.
         * Setters persist SDK parameters; configure once before VEM starts. */
        Liot_AudAnsConfig_t ans = { .bypass = 0, .mode = 1 };
        Liot_AudAgcConfig_t agc = { .bypass = 1, .targetLevel = 3,
                                  .compressionGain = 6, .limiterEnable = 1 };
        Liot_AudAecConfig_t aec = { .bypass = 0, .delay = DEMO_AI_AUDIO_AEC_DELAY,
                                  .cngMode = 1, .echoMode = 1, .nlpFlag = 0 };
        if (Liot_AudioSetAns(&ans) != L_AUD_ERR_SUCCESS ||
            Liot_AudioSetAgc(&agc) != L_AUD_ERR_SUCCESS ||
            Liot_AudioSetAec(&aec) != L_AUD_ERR_SUCCESS) {
            liot_trace("[AI-AEC] parameter setup failed\r\n");
            return AEC_ERR_FAULT;
        }
        ret = Liot_AudioInit(&s_audio_config);
        if (ret != L_AUD_ERR_SUCCESS) return -(int)ret - 1;
        s_aec_hw_ready = true;
    }
    {
        int start_ret = aec_start();
        if (start_ret != 0) {
            liot_trace("[AI-AEC] stream start failed ret=%d\r\n", start_ret);
            return start_ret;
        }
    }
    ret = L_AUD_ERR_SUCCESS;
#else
    ret = Liot_AudioInit(&s_audio_config);
#endif
    if (ret != L_AUD_ERR_SUCCESS)
    {
        liot_trace("[AI-AUDIO] ES8311 init failed ret=%d\r\n", (int)ret);
        return -(int)ret - 1;
    }

    s_audio_ready = true;
    s_play_done = true;
    s_levels_dirty = true;
    if (demo_ai_audio_apply_levels(lease) != 0)
    {
        return -20;
    }

    liot_trace("[AI-AUDIO] ready ES8311 I2C0/I2S0 PA11 "
               "16kHz/16bit/mono-right AEC=%u\r\n",
               (unsigned int)DEMO_AI_AUDIO_AEC_ENABLE);
    return 0;
}

int demo_ai_audio_prepare_session(const demo_ai_audio_lease_t *lease,
                                  uint8_t speaker_level,
                                  uint8_t mic_level)
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    liot_rtos_enter_critical();
    if (!demo_ai_audio_lease_valid_locked(lease) || s_aec_retiring) {
        liot_rtos_exit_critical();
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    snapshot = *lease;
    lease = &snapshot;
    s_speaker_level = demo_ai_audio_clamp_level(speaker_level);
    s_mic_level = demo_ai_audio_clamp_level(mic_level);
    s_levels_dirty = true;
    liot_rtos_exit_critical();
#else
    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }

    /* demo_ai_audio_init() commits dirty levels on both cold and warm paths.
     * Store the desired values first so the driver is programmed once. */
    s_speaker_level = demo_ai_audio_clamp_level(speaker_level);
    s_mic_level = demo_ai_audio_clamp_level(mic_level);
    s_levels_dirty = true;
#endif
    return demo_ai_audio_init(lease);
}

int demo_ai_audio_record_20ms(
    const demo_ai_audio_lease_t *lease,
    int16_t pcm[DEMO_AI_AUDIO_FRAME_SAMPLES])
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    if (!demo_ai_audio_snapshot_lease(lease, &snapshot))
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    lease = &snapshot;
#endif
#if !DEMO_AI_AUDIO_AEC_ENABLE
    Liot_AudErr_e ret;
#endif
    int levels_ret;

    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    if (!s_audio_ready || pcm == NULL)
    {
        return -1;
    }

    levels_ret = demo_ai_audio_apply_levels(lease);
    if (levels_ret != 0)
    {
        return levels_ret;
    }
#if DEMO_AI_AUDIO_AEC_ENABLE
    return aec_record(lease, pcm);
#else
    ret = Liot_AudioRecord((uint8_t *)s_record_frame,
                           (int)DEMO_AI_AUDIO_FRAME_BYTES);
    if (ret != L_AUD_ERR_SUCCESS)
    {
        return -(int)ret - 1;
    }

    memcpy(pcm, s_record_frame, DEMO_AI_AUDIO_FRAME_BYTES);
    (void)lite_audio_guard_impulses(lease->owner, s_record_frame, pcm);
    return 0;
#endif
}

int demo_ai_audio_play(const demo_ai_audio_lease_t *lease,
                       const int16_t *pcm, uint32_t samples)
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    if (!demo_ai_audio_snapshot_lease(lease, &snapshot))
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    lease = &snapshot;
#endif
#if !DEMO_AI_AUDIO_AEC_ENABLE
    Liot_AudErr_e ret;
    size_t bytes;
#endif
    int levels_ret;

    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    if (!s_audio_ready || pcm == NULL || samples == 0U)
    {
        return -1;
    }
    if (samples > ((size_t)INT32_MAX / sizeof(*pcm)))
    {
        return -2;
    }

    levels_ret = demo_ai_audio_apply_levels(lease);
    if (levels_ret != 0)
    {
        return levels_ret;
    }
#if DEMO_AI_AUDIO_AEC_ENABLE
    return aec_play(lease, pcm, samples);
#else
    bytes = samples * sizeof(*pcm);
    s_play_done = false;
    ret = Liot_AudioPlay((uint8_t *)(uintptr_t)pcm, (int)bytes);
    if (ret != L_AUD_ERR_SUCCESS)
    {
        s_play_done = true;
        return -(int)ret - 10;
    }
    return 0;
#endif
}

bool demo_ai_audio_play_done(const demo_ai_audio_lease_t *lease)
{
    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return true;
    }
#if DEMO_AI_AUDIO_AEC_ENABLE
    return aec_play_done();
#else
    return s_play_done;
#endif
}

int demo_ai_audio_stop(const demo_ai_audio_lease_t *lease)
{
#if DEMO_AI_AUDIO_AEC_ENABLE
    demo_ai_audio_lease_t snapshot;
    if (!demo_ai_audio_snapshot_lease(lease, &snapshot))
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    lease = &snapshot;
#endif
#if !DEMO_AI_AUDIO_AEC_ENABLE
    Liot_AudErr_e ret;
#endif

    if (!demo_ai_audio_lease_is_valid(lease))
    {
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    }
    if (!s_audio_ready)
    {
        s_play_done = true;
        return 0;
    }

#if DEMO_AI_AUDIO_AEC_ENABLE
    return aec_stop(lease);
#else
    ret = Liot_AudioStop();
    s_play_done = true;
    return (ret == L_AUD_ERR_SUCCESS) ? 0 : (-(int)ret - 1);
#endif
}
