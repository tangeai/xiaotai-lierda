/*
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from xiaotai-lierda f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93.
 * V04 ES8311: I2C0/I2S0, PA GPIO11, 8 kHz mono-right PCM16.
 * The display port owns the shared 3.3V rail. No power/reset/deinit here.
 */
#include "audio_device.h"
#include <string.h>
#include "liot_audio2.h"
#include "liot_gpio2.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include "liot_os.h"

/* Match the upstream V04 demo's useful 1..10 hardware range. Keep level 0
 * as an explicit mute in this port: ADC attenuation alone is not a mute,
 * so capture is also zeroed and AI suppresses its upload at mic level 0. */
static const uint8_t s_speaker_volume[] = {0, 30, 33, 37, 40, 43, 47, 50, 53, 57, 60};
static const uint8_t s_mic_gain[] = {0, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10};
static const uint8_t s_mic_volume[] = {0, 160, 168, 176, 183, 191, 199, 207, 214, 222, 230};
/* One 40 ms SDK transaction reduces RX stop/start gaps. The public API still
 * returns ordered 20 ms frames; only the record/control owner touches cache. */
#define CAPTURE_BATCH_FRAMES 2U
#define CAPTURE_BATCH_SAMPLES (DEMO_AI_AUDIO_FRAME_SAMPLES * CAPTURE_BATCH_FRAMES)
static __attribute__((aligned(16))) int16_t s_record_frame[CAPTURE_BATCH_SAMPLES];
/* Fixed F6D_A copies this single <=5120-byte item before returning. OP_PLAY
 * serializes its construction. No heap allocation or shared-rail toggling. */
#define WARMUP_SAMPLES (DEMO_AI_AUDIO_WARMUP_MS * (DEMO_AI_AUDIO_SAMPLE_RATE / 1000U))
#define SESSION_CHUNK_MS 300U
#define SESSION_CHUNK_SAMPLES (SESSION_CHUNK_MS * (DEMO_AI_AUDIO_SAMPLE_RATE / 1000U))
static __attribute__((aligned(16))) int16_t s_warm_frame[SESSION_CHUNK_SAMPLES];
static bool s_session_warm_pending;
/* Early warmup is valid only for the speaker level prepared before connect.
 * A changed target before the first speech falls back to the proven preamble. */
static bool s_session_prewarmed;
static uint32_t s_record_offset, s_record_cached_frames;
/* Captured samples omitted by controls/suppression still occupy media time.
 * This is a per-session modulo-32-bit millisecond accumulator, not wall time. */
static uint32_t s_discarded_capture_ms;
/* The real SDK retains this pointer, rather than copying the configuration. */
static Liot_AudHwConfig_t s_audio_config;
static bool s_audio_ready;
static bool s_play_done = true;
static bool s_stopped = true;
static bool s_levels_dirty = true;
static uint8_t s_speaker_level = DEMO_AI_AUDIO_DEFAULT_SPK_LEVEL;
static uint8_t s_mic_level = DEMO_AI_AUDIO_DEFAULT_MIC_LEVEL;
static demo_ai_audio_owner_e s_audio_owner;
static uint32_t s_audio_generation;
static uint8_t s_active;
static int s_last_record_error, s_last_play_error;
/* PCM amplitudes and control-pin samples only: no speech bytes are logged.
 * SDK callbacks update counters; the media task emits at most one report per
 * five seconds (plus the first frame and the final stop summary). */
static struct {
    uint32_t submissions, samples, nonzero, pa_high, pa_low, pa_unknown;
    uint32_t starts, finishes, errors, closes, last_log_ms;
    uint32_t peak;
    uint32_t warm_starts, warm_ms;
    bool logged;
} s_diagnostic;
/* Capture statistics describe raw ADC input before the software mute. All
 * timings are milliseconds. Only the record/control owner accesses this. */
static struct {
    uint32_t frames, errors, window_frames, window_samples, near_clip, zero_frames;
    uint32_t peak, reads, read_sum_ms, read_max_ms, gaps, gap_sum_ms, gap_max_ms;
    uint32_t last_end_ms, last_log_ms;
    uint64_t absolute_sum;
    bool have_end, logged;
} s_capture_diagnostic;

enum { OP_RECORD = 1, OP_PLAY = 2, OP_CONTROL = 4 };

static void discard_capture_buffer(void)
{
    s_discarded_capture_ms += s_record_cached_frames * DEMO_AI_AUDIO_FRAME_MS;
    s_record_offset = s_record_cached_frames = 0;
}

static bool lease_valid(const demo_ai_audio_lease_t *lease)
{
    return lease && lease->owner != DEMO_AI_AUDIO_OWNER_NONE &&
           lease->owner == s_audio_owner && lease->generation != 0 &&
           lease->generation == s_audio_generation;
}

bool demo_ai_audio_lease_is_valid(const demo_ai_audio_lease_t *lease)
{
    bool valid;
    liot_rtos_enter_critical();
    valid = lease_valid(lease);
    liot_rtos_exit_critical();
    return valid;
}

/* Locks cover only bookkeeping; never SDK I/O, logging or waiting. Control
 * operations are serialized with PCM calls. Record and playback may coexist. */
static int enter(const demo_ai_audio_lease_t *lease, uint8_t op, bool needs_ready)
{
    int result = 0;
    liot_rtos_enter_critical();
    if (!lease_valid(lease)) result = DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    else if ((op == OP_CONTROL && s_active) || (s_active & (op | OP_CONTROL)))
        result = DEMO_AI_AUDIO_ERR_BUSY;
    else if (needs_ready && (!s_audio_ready || s_stopped)) result = -1;
    else s_active |= op;
    liot_rtos_exit_critical();
    return result;
}

static void leave(uint8_t op)
{
    liot_rtos_enter_critical();
    s_active &= (uint8_t)~op;
    liot_rtos_exit_critical();
}

int demo_ai_audio_acquire(demo_ai_audio_owner_e owner, demo_ai_audio_lease_t *lease)
{
    int result = 0;
    if (!lease || owner <= DEMO_AI_AUDIO_OWNER_NONE || owner >= DEMO_AI_AUDIO_OWNER_COUNT)
        return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    liot_rtos_enter_critical();
    if (s_audio_owner != DEMO_AI_AUDIO_OWNER_NONE) result = DEMO_AI_AUDIO_ERR_BUSY;
    else {
        if (++s_audio_generation == 0) ++s_audio_generation;
        s_audio_owner = owner;
        discard_capture_buffer();
        s_discarded_capture_ms = 0;
        lease->owner = owner;
        lease->generation = s_audio_generation;
    }
    liot_rtos_exit_critical();
    if (!result) liot_trace("[AI-AUDIO] acquired owner=%u generation=%lu\r\n",
                           (unsigned)owner, (unsigned long)lease->generation);
    return result;
}

int demo_ai_audio_release(demo_ai_audio_lease_t *lease)
{
    int result = 0;
    liot_rtos_enter_critical();
    if (!lease_valid(lease)) result = DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    else if (s_active || (s_audio_ready && !s_stopped)) result = DEMO_AI_AUDIO_ERR_BUSY;
    else {
        s_audio_owner = DEMO_AI_AUDIO_OWNER_NONE;
        discard_capture_buffer();
        s_discarded_capture_ms = 0;
        lease->owner = DEMO_AI_AUDIO_OWNER_NONE;
        lease->generation = 0;
    }
    liot_rtos_exit_critical();
    if (!result) liot_trace("[AI-AUDIO] lease released\r\n");
    return result;
}

static uint8_t clamp(uint8_t level)
{
    return level > DEMO_AI_AUDIO_LEVEL_MAX ? DEMO_AI_AUDIO_LEVEL_MAX : level;
}

static void audio_callback(Liot_AudEvent_e event, void *context)
{
    (void)context;
    /* The SDK invokes these from its side task, not the DMA ISR. Delayed
     * FINISH is advisory only; callers must never use it to free DMA state. */
    liot_rtos_enter_critical();
    if (event == L_AUD_EVT_START) ++s_diagnostic.starts;
    else if (event == L_AUD_EVT_FINISH) ++s_diagnostic.finishes;
    else if (event == L_AUD_EVT_ERROR) ++s_diagnostic.errors;
    else if (event == L_AUD_EVT_CLOSE) ++s_diagnostic.closes;
    if (event == L_AUD_EVT_FINISH || event == L_AUD_EVT_ERROR || event == L_AUD_EVT_CLOSE)
        s_play_done = true;
    liot_rtos_exit_critical();
}

static void playback_diagnostic(const int16_t *pcm, uint32_t samples, bool final)
{
    uint32_t peak = 0, nonzero = 0;
    uint32_t starts, finishes, errors, closes;
    /* F6D_A returns milliseconds; its header's seconds description is stale. */
    uint32_t now_ms = liot_rtos_get_running_time();
    int pa = -1, volume = -1;
    for (uint32_t i = 0; i < samples; ++i) {
        int32_t value = pcm[i];
        uint32_t absolute = (uint32_t)(value < 0 ? -value : value);
        if (absolute > peak) peak = absolute;
        if (value) ++nonzero;
    }
    if (samples) {
        /* This readback is a momentary pin level, not proof of speaker output. */
        pa = (int)Liot_GpioGetLevel(L_GPIO_11);
        ++s_diagnostic.submissions;
        s_diagnostic.samples += samples;
        s_diagnostic.nonzero += nonzero;
        if (peak > s_diagnostic.peak) s_diagnostic.peak = peak;
        if (pa == L_IO_HIGH) ++s_diagnostic.pa_high;
        else if (pa == L_IO_LOW) ++s_diagnostic.pa_low;
        else ++s_diagnostic.pa_unknown;
    }
    if (!s_diagnostic.submissions || (!final && s_diagnostic.logged &&
        (uint32_t)(now_ms - s_diagnostic.last_log_ms) < 5000U)) return;
    liot_rtos_enter_critical();
    starts = s_diagnostic.starts; finishes = s_diagnostic.finishes;
    errors = s_diagnostic.errors; closes = s_diagnostic.closes;
    liot_rtos_exit_critical();
    (void)Liot_AudioGetVolume(&volume);
    TIRTC_LOG_DEBUG("[AI-AUDIO] playback%s frames=%lu samples=%lu nonzero=%lu peak=%lu "
               "pa=%d high/low/unknown=%lu/%lu/%lu events(start/finish/error/close)=%lu/%lu/%lu/%lu sw=%d warm=%lu/%lums\r\n",
               final ? " stop" : "", (unsigned long)s_diagnostic.submissions,
               (unsigned long)s_diagnostic.samples, (unsigned long)s_diagnostic.nonzero,
               (unsigned long)s_diagnostic.peak, pa, (unsigned long)s_diagnostic.pa_high,
               (unsigned long)s_diagnostic.pa_low, (unsigned long)s_diagnostic.pa_unknown,
               (unsigned long)starts, (unsigned long)finishes, (unsigned long)errors,
               (unsigned long)closes, volume, (unsigned long)s_diagnostic.warm_starts,
               (unsigned long)s_diagnostic.warm_ms);
    s_diagnostic.peak = 0;
    s_diagnostic.last_log_ms = now_ms;
    s_diagnostic.logged = true;
}

static void capture_report(uint32_t now_ms, bool final)
{
    if (!s_capture_diagnostic.reads || (!final && s_capture_diagnostic.logged &&
        (uint32_t)(now_ms - s_capture_diagnostic.last_log_ms) < 5000U)) return;
    TIRTC_LOG_DEBUG("[AI-AUDIO] capture%s batches=%lu errors=%lu window_batches=%lu samples=%lu "
               "peak=%lu meanabs=%lu near_clip=%lu zero=%lu read_ms(avg/max)=%lu/%lu "
               "gap_ms(avg/max)=%lu/%lu mic=%u\r\n", final ? " stop" : "",
               (unsigned long)s_capture_diagnostic.frames, (unsigned long)s_capture_diagnostic.errors,
               (unsigned long)s_capture_diagnostic.window_frames, (unsigned long)s_capture_diagnostic.window_samples,
               (unsigned long)s_capture_diagnostic.peak,
               (unsigned long)(s_capture_diagnostic.window_samples ?
                   s_capture_diagnostic.absolute_sum / s_capture_diagnostic.window_samples : 0),
               (unsigned long)s_capture_diagnostic.near_clip, (unsigned long)s_capture_diagnostic.zero_frames,
               (unsigned long)(s_capture_diagnostic.read_sum_ms / s_capture_diagnostic.reads),
               (unsigned long)s_capture_diagnostic.read_max_ms,
               (unsigned long)(s_capture_diagnostic.gaps ? s_capture_diagnostic.gap_sum_ms / s_capture_diagnostic.gaps : 0),
               (unsigned long)s_capture_diagnostic.gap_max_ms, s_mic_level);
    s_capture_diagnostic.window_frames = s_capture_diagnostic.window_samples = 0;
    s_capture_diagnostic.near_clip = s_capture_diagnostic.zero_frames = 0;
    s_capture_diagnostic.peak = s_capture_diagnostic.reads = s_capture_diagnostic.read_sum_ms = 0;
    s_capture_diagnostic.read_max_ms = s_capture_diagnostic.gaps = s_capture_diagnostic.gap_sum_ms = 0;
    s_capture_diagnostic.gap_max_ms = 0;
    s_capture_diagnostic.absolute_sum = 0;
    s_capture_diagnostic.last_log_ms = now_ms;
    s_capture_diagnostic.logged = true;
}

static void capture_diagnostic(const int16_t *pcm, uint32_t samples, int result,
                               uint32_t started_ms, uint32_t ended_ms)
{
    uint32_t peak = 0, elapsed = ended_ms - started_ms;
    ++s_capture_diagnostic.reads;
    s_capture_diagnostic.read_sum_ms += elapsed;
    if (elapsed > s_capture_diagnostic.read_max_ms) s_capture_diagnostic.read_max_ms = elapsed;
    if (s_capture_diagnostic.have_end) {
        uint32_t gap = started_ms - s_capture_diagnostic.last_end_ms;
        ++s_capture_diagnostic.gaps; s_capture_diagnostic.gap_sum_ms += gap;
        if (gap > s_capture_diagnostic.gap_max_ms) s_capture_diagnostic.gap_max_ms = gap;
    }
    s_capture_diagnostic.have_end = true; s_capture_diagnostic.last_end_ms = ended_ms;
    if (result) ++s_capture_diagnostic.errors;
    else {
        ++s_capture_diagnostic.frames; ++s_capture_diagnostic.window_frames;
        s_capture_diagnostic.window_samples += samples;
        for (uint32_t i = 0; i < samples; ++i) {
            int32_t value = pcm[i];
            uint32_t absolute = (uint32_t)(value < 0 ? -value : value);
            if (absolute > peak) peak = absolute;
            s_capture_diagnostic.absolute_sum += absolute;
            if (absolute >= 32760U) ++s_capture_diagnostic.near_clip;
        }
        if (!peak) ++s_capture_diagnostic.zero_frames;
        if (peak > s_capture_diagnostic.peak) s_capture_diagnostic.peak = peak;
    }
    capture_report(ended_ms, false);
}

bool demo_ai_audio_is_ready(const demo_ai_audio_lease_t *lease)
{
    bool ready;
    liot_rtos_enter_critical();
    ready = lease_valid(lease) && s_audio_ready && !s_stopped;
    liot_rtos_exit_critical();
    return ready;
}

/* The caller owns OP_CONTROL. No capture/play path performs I2C writes. */
static int apply_levels(void)
{
    Liot_AudErr_e sw, codec, mic;
    if (!s_audio_ready) return -1;
    if (!s_levels_dirty) return 0;
    sw = Liot_AudioSetVolume(s_speaker_volume[s_speaker_level]);
    codec = Liot_AudioSetCodecVolume(s_speaker_volume[s_speaker_level]);
    mic = Liot_AudioSetMicVolume(s_mic_gain[s_mic_level], s_mic_volume[s_mic_level]);
    /* Only a real commit reaches here; unchanged settings never flood logs. */
    liot_trace("[AI-AUDIO] levels speaker=%u sw/codec=%u mic=%u gain=%u adc=%u ret=%d/%d/%d\r\n",
               s_speaker_level, s_speaker_volume[s_speaker_level], s_mic_level,
               s_mic_gain[s_mic_level], s_mic_volume[s_mic_level], sw, codec, mic);
    if (sw || codec || mic) {
        return -20;
    }
    s_levels_dirty = false;
    return 0;
}

int demo_ai_audio_apply_levels(const demo_ai_audio_lease_t *lease)
{
    int result = enter(lease, OP_CONTROL, false);
    if (result) return result;
    result = apply_levels();
    leave(OP_CONTROL);
    return result;
}

int demo_ai_audio_set_levels(const demo_ai_audio_lease_t *lease, uint8_t speaker, uint8_t mic)
{
    int result = enter(lease, OP_CONTROL, false);
    if (result) return result;
    speaker = clamp(speaker); mic = clamp(mic);
    if (s_session_prewarmed && speaker != s_speaker_level) {
        s_session_warm_pending = true;
        s_session_prewarmed = false;
    }
    if (speaker != s_speaker_level || mic != s_mic_level) {
        s_levels_dirty = true;
        discard_capture_buffer();
    }
    s_speaker_level = speaker; s_mic_level = mic;
    result = s_audio_ready ? apply_levels() : 0;
    leave(OP_CONTROL);
    return result;
}

int demo_ai_audio_get_levels(const demo_ai_audio_lease_t *lease, uint8_t *speaker, uint8_t *mic)
{
    int result = 0;
    liot_rtos_enter_critical();
    if (!lease_valid(lease)) result = DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    else {
        if (speaker) *speaker = s_speaker_level;
        if (mic) *mic = s_mic_level;
    }
    liot_rtos_exit_critical();
    return result;
}

/* OP_CONTROL held. The app initializes the shared rail before reaching here. */
static int init_audio(void)
{
    int result;
    if (s_audio_ready) {
        if (!s_stopped) return apply_levels();
        /* Local F6D_A Pause sets state=3; Resume accepts 3 and restores idle=1.
         * Stop leaves state=5, which otherwise prevents TryPlay starting DMA. */
        int paused = Liot_AudioPlayPause();
        int resumed = Liot_AudioPlayResume();
        if (paused || resumed) {
            liot_trace("[AI-AUDIO] warm reset failed ret=%d/%d\r\n", paused, resumed);
            return -21;
        }
    } else {
        memset(&s_audio_config, 0, sizeof(s_audio_config));
        s_audio_config.i2cNum = 0;
        s_audio_config.i2sNum = 0;
        s_audio_config.paGpioNum = 11;
        s_audio_config.codecType = L_AUD_ES8311;
        s_audio_config.channel = L_AUD_MONO_RIGHT;
        s_audio_config.role = L_AUD_ROLE_SLAVE;
        s_audio_config.mode = L_AUD_MODE_I2S;
        s_audio_config.frameSize = L_AUD_FRAMESIZE_16_16;
        s_audio_config.samples = L_AUD_08K_SAMPLES;
        s_audio_config.callback = audio_callback;
        s_audio_config.use3A = 0; /* F6D_A's 3A entry points return NO_SUPPORT. */
        result = Liot_AudioInit(&s_audio_config);
        if (result) {
            liot_trace("[AI-AUDIO] ES8311 init failed ret=%d\r\n", result);
            return -result - 1;
        }
        s_audio_ready = true;
        liot_trace("[AI-AUDIO] ES8311 I2C0/I2S0 PA11 8000Hz/16bit/mono no-AEC\r\n");
    }
    s_levels_dirty = true;
    result = apply_levels();
    if (!result) {
        s_stopped = false;
        s_play_done = true;
        s_session_warm_pending = true;
        s_session_prewarmed = false;
        s_last_record_error = s_last_play_error = 0;
        discard_capture_buffer();
        liot_rtos_enter_critical();
        memset(&s_diagnostic, 0, sizeof(s_diagnostic));
        memset(&s_capture_diagnostic, 0, sizeof(s_capture_diagnostic));
        liot_rtos_exit_critical();
    }
    return result;
}

int demo_ai_audio_init(const demo_ai_audio_lease_t *lease)
{
    int result = enter(lease, OP_CONTROL, false);
    if (result) return result;
    discard_capture_buffer();
    s_discarded_capture_ms = 0;
    result = init_audio();
    leave(OP_CONTROL);
    return result;
}

int demo_ai_audio_prepare_session(const demo_ai_audio_lease_t *lease, uint8_t speaker, uint8_t mic)
{
    int result = enter(lease, OP_CONTROL, false);
    if (result) return result;
    discard_capture_buffer();
    s_discarded_capture_ms = 0;
    if (s_session_prewarmed && clamp(speaker) != s_speaker_level) {
        s_session_warm_pending = true;
        s_session_prewarmed = false;
    }
    if (clamp(speaker) != s_speaker_level || clamp(mic) != s_mic_level) {
        s_levels_dirty = true;
        discard_capture_buffer();
    }
    s_speaker_level = clamp(speaker);
    s_mic_level = clamp(mic);
    result = init_audio();
    leave(OP_CONTROL);
    return result;
}

int demo_ai_audio_record_20ms(const demo_ai_audio_lease_t *lease,
                            int16_t pcm[DEMO_AI_AUDIO_FRAME_SAMPLES])
{
    int result;
    uint32_t started_ms, ended_ms;
    if (!pcm) return -1;
    result = enter(lease, OP_RECORD, true);
    if (result) return result;
    if (!s_record_cached_frames) {
        started_ms = liot_rtos_get_running_time();
        result = Liot_AudioRecord((uint8_t *)s_record_frame, (int)sizeof(s_record_frame));
        ended_ms = liot_rtos_get_running_time();
        capture_diagnostic(s_record_frame, CAPTURE_BATCH_SAMPLES, result, started_ms, ended_ms);
        if (result) {
            discard_capture_buffer();
            memset(pcm, 0, DEMO_AI_AUDIO_FRAME_BYTES);
            if (result != s_last_record_error)
                liot_trace("[AI-AUDIO] record failed ret=%d\r\n", result);
            s_last_record_error = result;
            leave(OP_RECORD);
            return -result - 1;
        }
        s_record_offset = 0;
        s_record_cached_frames = CAPTURE_BATCH_FRAMES;
    }
    if (!s_mic_level) memset(pcm, 0, DEMO_AI_AUDIO_FRAME_BYTES);
    else memcpy(pcm, s_record_frame + s_record_offset, DEMO_AI_AUDIO_FRAME_BYTES);
    s_record_offset += DEMO_AI_AUDIO_FRAME_SAMPLES;
    --s_record_cached_frames;
    s_last_record_error = 0;
    leave(OP_RECORD);
    return 0;
}

int demo_ai_audio_discard_capture(const demo_ai_audio_lease_t *lease)
{
    return demo_ai_audio_discard_capture_timed(lease, NULL);
}

int demo_ai_audio_discard_capture_timed(const demo_ai_audio_lease_t *lease,
                                      uint32_t *discarded_ms)
{
    int result = enter(lease, OP_CONTROL, false);
    if (result) return result;
    discard_capture_buffer();
    if (discarded_ms) *discarded_ms = s_discarded_capture_ms;
    s_discarded_capture_ms = 0;
    leave(OP_CONTROL);
    return 0;
}

/* OP_PLAY held. Queue 1200 ms without waiting for DMA: connection work can
 * run while these samples advance the codec ramp. The final 80 ms remains
 * attached to the first speech item, including after a natural queue FINISH. */
static int queue_session_silence(uint32_t *accepted_ms)
{
    int result;
    *accepted_ms = 0;
    memset(s_warm_frame, 0, sizeof(s_warm_frame));
    for (uint32_t left = DEMO_AI_AUDIO_SESSION_WARMUP_MS - DEMO_AI_AUDIO_WARMUP_MS;
         left; left -= SESSION_CHUNK_MS) {
        result = Liot_AudioPlay((uint8_t *)s_warm_frame, (int)sizeof(s_warm_frame));
        if (result) return result;
        *accepted_ms += SESSION_CHUNK_MS;
    }
    return 0;
}

int demo_ai_audio_prewarm_session(const demo_ai_audio_lease_t *lease, uint32_t *queued_ms)
{
    int result;
    uint32_t accepted_ms = 0;
    if (!queued_ms) return -1;
    *queued_ms = 0;
    result = enter(lease, OP_PLAY, true);
    if (result) return result;
    if (!s_speaker_level || !s_session_warm_pending) { leave(OP_PLAY); return 0; }
    liot_rtos_enter_critical();
    s_play_done = false;
    liot_rtos_exit_critical();
    result = queue_session_silence(&accepted_ms);
    if (!result) {
        s_session_warm_pending = false;
        s_session_prewarmed = true;
        *queued_ms = accepted_ms;
    }
    s_last_play_error = result;
    leave(OP_PLAY);
    liot_trace("[AI-AUDIO] connect prewarm queued=%lums ret=%d\r\n",
               (unsigned long)accepted_ms, result);
    if (result && accepted_ms) return DEMO_AI_AUDIO_ERR_WARM_PARTIAL;
    return result ? -result - 10 : 0;
}

static int queue_playback(const demo_ai_audio_lease_t *lease, const int16_t *pcm,
                          uint32_t samples, bool warm, uint32_t *extra_ms)
{
    int result;
    uint32_t queued_samples = samples;
    uint32_t extra = warm ? DEMO_AI_AUDIO_WARMUP_MS : 0;
    uint32_t silent_queued = 0;
    const int16_t *queued_pcm = pcm;
    if (extra_ms) *extra_ms = 0;
    /* Speech remains bounded to 20 ms. Only the burst-start item includes
     * explicit silence, so scheduling cannot split preheat from its speech. */
    /* F6D_A Liot_I2sSend truncates bytes to a multiple of four. Reject odd
     * PCM16 counts here instead of silently losing the final speech sample;
     * the AI ring coalesces odd network tails and pads a final lone sample. */
    if (!pcm || !samples || (samples & 1U) || samples > DEMO_AI_AUDIO_FRAME_SAMPLES) return -1;
    result = enter(lease, OP_PLAY, true);
    if (result) return result;
    if (!s_speaker_level) { leave(OP_PLAY); return 0; }
    liot_rtos_enter_critical();
    s_play_done = false;
    liot_rtos_exit_critical();
    if (warm && s_session_warm_pending) {
        /* The vendor Start leaves DAC register 0x37=0x48: at 8 kHz it
         * takes 4 ms/0.25 dB. Our maximum codec level 60 maps to code153,
         * so the worst 0->153 restoration needs 1224 ms of DAC clocks.
         * Only the first reply gets 1280 ms of valid silent I2S samples.
         * Every 4800-byte item stays below the SDK's 5120-byte item limit. */
        result = queue_session_silence(&silent_queued);
        if (result) goto play_result;
        extra += silent_queued;
    }
    if (warm) {
        memset(s_warm_frame, 0, WARMUP_SAMPLES * sizeof(*s_warm_frame));
        memcpy(s_warm_frame + WARMUP_SAMPLES, pcm, samples * sizeof(*pcm));
        queued_pcm = s_warm_frame;
        queued_samples += WARMUP_SAMPLES;
    }
    result = Liot_AudioPlay((uint8_t *)(uintptr_t)queued_pcm, (int)(queued_samples * sizeof(*queued_pcm)));
play_result:
    if (result) {
        liot_rtos_enter_critical();
        s_play_done = true;
        liot_rtos_exit_critical();
        if (result != s_last_play_error) liot_trace("[AI-AUDIO] play failed ret=%d\r\n", result);
    } else {
        if (warm) {
            s_session_warm_pending = false;
            s_session_prewarmed = false;
            ++s_diagnostic.warm_starts;
            s_diagnostic.warm_ms += extra;
            if (extra_ms) *extra_ms = extra;
        }
        playback_diagnostic(pcm, samples, false);
    }
    s_last_play_error = result;
    leave(OP_PLAY);
    if (result && silent_queued) {
        liot_trace("[AI-AUDIO] session warm partial=%lums; safe stop required\r\n",
                   (unsigned long)silent_queued);
        return DEMO_AI_AUDIO_ERR_WARM_PARTIAL;
    }
    return result ? -result - 10 : 0;
}

int demo_ai_audio_play(const demo_ai_audio_lease_t *lease, const int16_t *pcm, uint32_t samples)
{
    return queue_playback(lease, pcm, samples, false, NULL);
}

int demo_ai_audio_play_warm(const demo_ai_audio_lease_t *lease, const int16_t *pcm,
                          uint32_t samples, uint32_t *queued_extra_ms)
{
    if (!queued_extra_ms) return -1;
    return queue_playback(lease, pcm, samples, true, queued_extra_ms);
}

bool demo_ai_audio_play_done(const demo_ai_audio_lease_t *lease)
{
    bool done;
    liot_rtos_enter_critical();
    done = !lease_valid(lease) || s_play_done;
    liot_rtos_exit_critical();
    return done;
}

int demo_ai_audio_stop(const demo_ai_audio_lease_t *lease)
{
    int result = enter(lease, OP_CONTROL, false);
    if (result) return result;
    discard_capture_buffer();
    if (s_audio_ready && !s_stopped) {
        capture_report(liot_rtos_get_running_time(), true);
        playback_diagnostic(NULL, 0, true);
        /* Mute before the asynchronous stop request. No shared-power toggle
         * and no attempt to deinit a blocked DMA or free SDK-owned buffers. */
        int sw = Liot_AudioSetVolume(0);
        int codec = Liot_AudioSetCodecVolume(0);
        int stopped = Liot_AudioStop();
        if (sw || codec || stopped) {
            liot_trace("[AI-AUDIO] stop failed ret=%d/%d/%d\r\n", sw, codec, stopped);
            result = -22;
        } else {
            s_stopped = true;
            s_levels_dirty = true;
        }
    }
    leave(OP_CONTROL);
    return result;
}
