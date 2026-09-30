/* Test the complete production adapter; fake only the SDK/RTOS boundary. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio_device.h"
#include "liot_audio2.h"
#include "liot_gpio2.h"

static const int s_original_volume[] = {30,33,37,40,43,47,50,53,57,60};
static const int s_device_codec[] = {38,41,46,50,53,57,61,64,69,72};
static const int s_mic_gain[] = {7,7,8,8,8,9,9,9,10,10};
static const int s_mic_volume[] = {160,168,176,183,191,199,207,214,222,230};
static unsigned s_init_calls, s_sw_calls, s_codec_calls, s_mic_calls;
static unsigned s_play_calls, s_record_calls, s_stop_calls;
static unsigned s_pause_calls, s_resume_calls, s_power_calls;
static unsigned s_failure, s_identity_samples;
static unsigned s_level_failure_cases;
static int s_sw, s_codec, s_gain, s_mic, s_critical_depth;
static bool s_check_identity;

void liot_rtos_enter_critical(void) { ++s_critical_depth; }
void liot_rtos_exit_critical(void)
{
    assert(s_critical_depth > 0);
    --s_critical_depth;
}
void liot_trace(const char *format, ...) { (void)format; }
liot_gpioerr_e Liot_GpioInit(liot_gpio_e gpio, liot_gpiodir_e direction,
                           liot_gpiolvl_e level, liot_intcb_t *callback)
{
    assert(gpio == L_GPIO_25 && direction == L_IO_OUTPUT);
    assert(level == L_IO_HIGH && callback == NULL);
    ++s_power_calls;
    return L_GPIO_ERR_SUCCESS;
}
liot_gpioerr_e Liot_AonPowerCtl(bool enable)
{
    assert(enable);
    ++s_power_calls;
    return L_GPIO_ERR_SUCCESS;
}
liot_gpioerr_e Liot_SetVoltage(liot_powerdomain_e domain, liot_volt_e voltage)
{
    assert(domain == L_DOMAIN_ALL && voltage == L_VOLT_3_30V);
    ++s_power_calls;
    return L_GPIO_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioInit(Liot_AudHwConfig_t *config)
{
    assert(s_critical_depth == 0);
    assert(config->i2cNum == 0 && config->i2sNum == 0 && config->paGpioNum == 11);
    assert(config->codecType == L_AUD_ES8311);
    assert(config->channel == L_AUD_MONO_RIGHT && config->samples == L_AUD_16K_SAMPLES);
    assert(config->frameSize == L_AUD_FRAMESIZE_16_16);
    assert(config->role == L_AUD_ROLE_SLAVE && config->mode == L_AUD_MODE_I2S);
    assert(config->callback != NULL);
    ++s_init_calls;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioSetVolume(int volume)
{
    assert(s_critical_depth == 0);
    ++s_sw_calls;
    if (s_failure == 1) return L_AUD_ERR_EXECUTE;
    s_sw = volume;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioSetCodecVolume(int volume)
{
    assert(s_critical_depth == 0);
    ++s_codec_calls;
    if (s_failure == 2) return L_AUD_ERR_EXECUTE;
    s_codec = volume;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioSetMicVolume(uint8_t gain, int volume)
{
    assert(s_critical_depth == 0);
    ++s_mic_calls;
    if (s_failure == 3) return L_AUD_ERR_EXECUTE;
    s_gain = gain;
    s_mic = volume;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioPlayPause(void)
{
    ++s_pause_calls;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioPlayResume(void)
{
    ++s_resume_calls;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioStop(void)
{
    ++s_stop_calls;
    return L_AUD_ERR_SUCCESS;
}

/* Independent arithmetic model, NOT an emulation of I2S or hardware audio.
 * F6D_A Liot_AudioApplyVolume mode0 uses this Q8 gain then ASR8/saturation.
 * The firmware's BSS default mode is zero; no Lite caller selects mode1.
 * Keep negative division explicit so this matches ARM ASR on any host. */
static int sdk_gain_q8(int volume)
{
    return 256 * (volume / 10) + 26 * (volume % 10);
}
static int sdk_pcm_model(int sample, int volume)
{
    int product = sample * sdk_gain_q8(volume);
    int output = product < 0 ? -((-product + 255) / 256) : product / 256;
    if (output > INT16_MAX) return INT16_MAX;
    if (output < INT16_MIN) return INT16_MIN;
    return output;
}
Liot_AudErr_e Liot_AudioPlay(uint8_t *data, int length)
{
    assert(s_critical_depth == 0 && length > 0 && length % 2 == 0);
    ++s_play_calls;
    if (s_check_identity) {
        for (int i = 0; i < length / 2; ++i) {
            int16_t sample;
            memcpy(&sample, data + i * 2, sizeof(sample));
            assert(sdk_pcm_model(sample, s_sw) == sample);
            ++s_identity_samples;
        }
    }
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioRecord(uint8_t *data, int length)
{
    assert(length == DEMO_AI_AUDIO_FRAME_BYTES);
    memset(data, 0, (size_t)length);
    ++s_record_calls;
    return L_AUD_ERR_SUCCESS;
}

static unsigned gain_writes(void) { return s_sw_calls + s_codec_calls + s_mic_calls; }
static void expect_levels(demo_ai_audio_owner_e owner, unsigned speaker, unsigned mic)
{
    assert(s_sw == (owner == DEMO_AI_AUDIO_OWNER_DEVICE ? 10 : s_original_volume[speaker-1]));
    assert(s_codec == (owner == DEMO_AI_AUDIO_OWNER_DEVICE ? s_device_codec[speaker-1] : s_original_volume[speaker-1]));
    assert(s_gain == s_mic_gain[mic-1] && s_mic == s_mic_volume[mic-1]);
    assert(s_critical_depth == 0);
}
static void close_session(demo_ai_audio_lease_t *lease)
{
    assert(demo_ai_audio_stop(lease) == 0);
    assert(demo_ai_audio_release(lease) == 0);
    assert(!demo_ai_audio_lease_is_valid(lease));
}
static void check_pending_level_failure(const demo_ai_audio_lease_t *lease,
                                        demo_ai_audio_owner_e owner)
{
    int16_t pcm[DEMO_AI_AUDIO_FRAME_SAMPLES];
    int16_t expected[DEMO_AI_AUDIO_FRAME_SAMPLES];
    const int16_t silence[DEMO_AI_AUDIO_FRAME_SAMPLES] = {0};
    for (unsigned failure = 1; failure <= 3; ++failure) {
        for (unsigned playing = 0; playing <= 1; ++playing) {
            assert(demo_ai_audio_prepare_session(lease, 8, 7) == 0);
            for (unsigned i = 0; i < DEMO_AI_AUDIO_FRAME_SAMPLES; ++i)
                pcm[i] = (int16_t)(1234 + i);
            memcpy(expected, pcm, sizeof(pcm));
            if (playing)
                assert(demo_ai_audio_play(lease, pcm, DEMO_AI_AUDIO_FRAME_SAMPLES) == 0);
            bool done = demo_ai_audio_play_done(lease);
            assert(done == !playing);
            unsigned writes = gain_writes();
            unsigned records = s_record_calls, plays = s_play_calls;
            unsigned stops = s_stop_calls, pauses = s_pause_calls;
            unsigned resumes = s_resume_calls, inits = s_init_calls;

            /* A failed update leaves dirty pending. Neither I/O operation may
             * use partially applied levels or change the existing done state. */
            s_failure = failure;
            assert(demo_ai_audio_set_levels(lease, 6, 5) == -2);
            assert(demo_ai_audio_record_20ms(lease, pcm) == -2);
            assert(s_record_calls == records && s_play_calls == plays);
            assert(memcmp(pcm, expected, sizeof(pcm)) == 0);
            assert(demo_ai_audio_play_done(lease) == done);
            assert(demo_ai_audio_play(lease, pcm, DEMO_AI_AUDIO_FRAME_SAMPLES) == -2);
            assert(s_record_calls == records && s_play_calls == plays);
            assert(memcmp(pcm, expected, sizeof(pcm)) == 0);
            assert(demo_ai_audio_play_done(lease) == done);
            assert(gain_writes() == writes + 9);

            /* Capture can retry the still-dirty update without a new UI action. */
            s_failure = 0;
            assert(demo_ai_audio_record_20ms(lease, pcm) == 0);
            assert(s_record_calls == records + 1 && s_play_calls == plays);
            assert(memcmp(pcm, silence, sizeof(pcm)) == 0);
            assert(demo_ai_audio_play_done(lease) == done);
            assert(gain_writes() == writes + 12);
            expect_levels(owner, 6, 5);
            assert(demo_ai_audio_play(lease, pcm, DEMO_AI_AUDIO_FRAME_SAMPLES) == 0);
            assert(s_play_calls == plays + 1 && !demo_ai_audio_play_done(lease));
            assert(gain_writes() == writes + 12);

            /* Playback must also retry correctly when it is the next service
             * point; the following clean capture needs no additional writes. */
            s_failure = failure;
            assert(demo_ai_audio_set_levels(lease, 8, 7) == -2);
            s_failure = 0;
            memcpy(pcm, expected, sizeof(pcm));
            assert(demo_ai_audio_play(lease, pcm, DEMO_AI_AUDIO_FRAME_SAMPLES) == 0);
            assert(s_play_calls == plays + 2 && !demo_ai_audio_play_done(lease));
            assert(memcmp(pcm, expected, sizeof(pcm)) == 0);
            assert(gain_writes() == writes + 18);
            expect_levels(owner, 8, 7);
            assert(demo_ai_audio_record_20ms(lease, pcm) == 0);
            assert(s_record_calls == records + 2 && gain_writes() == writes + 18);
            assert(memcmp(pcm, silence, sizeof(pcm)) == 0);
            assert(s_stop_calls == stops && s_pause_calls == pauses);
            assert(s_resume_calls == resumes && s_init_calls == inits);
            ++s_level_failure_cases;
        }
    }
}
static void check_owner(demo_ai_audio_owner_e owner)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    int16_t pcm[DEMO_AI_AUDIO_FRAME_SAMPLES] = {0};
    uint8_t speaker, mic;
    assert(demo_ai_audio_acquire(owner, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) == 0);
    assert(demo_ai_audio_is_ready(&lease));
    for (uint8_t s = 1; s <= 10; ++s) {
        for (uint8_t m = 1; m <= 10; ++m) {
            unsigned before = gain_writes();
            assert(demo_ai_audio_set_levels(&lease, s, m) == 0);
            expect_levels(owner, s, m);
            assert(gain_writes() == before + 3);
            before = gain_writes();
            assert(demo_ai_audio_apply_levels(&lease) == 0);
            assert(demo_ai_audio_play(&lease, pcm, DEMO_AI_AUDIO_FRAME_SAMPLES) == 0);
            assert(demo_ai_audio_record_20ms(&lease, pcm) == 0);
            assert(gain_writes() == before); /* Clean hot path makes no register writes. */
        }
    }
    assert(demo_ai_audio_set_levels(&lease, 0, 0) == 0);
    assert(demo_ai_audio_get_levels(&lease, &speaker, &mic) == 0);
    assert(speaker == 1 && mic == 1);
    expect_levels(owner, 1, 1);
    assert(demo_ai_audio_set_levels(&lease, 255, 255) == 0);
    expect_levels(owner, 10, 10);
    for (s_failure = 1; s_failure <= 3; ++s_failure) {
        unsigned before = gain_writes();
        assert(demo_ai_audio_set_levels(&lease, 8, 7) == -2);
        assert(gain_writes() == before + 3);
        unsigned failure = s_failure;
        s_failure = 0;
        assert(demo_ai_audio_apply_levels(&lease) == 0);
        assert(gain_writes() == before + 6); /* Failed writes retain dirty. */
        expect_levels(owner, 8, 7);
        assert(demo_ai_audio_apply_levels(&lease) == 0);
        assert(gain_writes() == before + 6);
        s_failure = failure;
    }
    s_failure = 0;
    check_pending_level_failure(&lease, owner);
    close_session(&lease);
}

static void check_handoff(demo_ai_audio_owner_e other, uint8_t level)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    demo_ai_audio_lease_t stale, busy = DEMO_AI_AUDIO_LEASE_INIT;
    int16_t pcm[DEMO_AI_AUDIO_FRAME_SAMPLES] = {0};
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, level, 7) == 0);
    for (unsigned direction = 0; direction < 2; ++direction) {
        demo_ai_audio_owner_e target = direction ? DEMO_AI_AUDIO_OWNER_DEVICE : other;
        stale = lease;
        close_session(&lease);
        unsigned before = gain_writes();
        unsigned old_stop = s_stop_calls, old_play = s_play_calls, old_record = s_record_calls;
        unsigned old_pause = s_pause_calls, old_init = s_init_calls;
        assert(demo_ai_audio_acquire(target, &lease) == 0);
        assert(lease.generation != stale.generation);
        assert(demo_ai_audio_acquire(target, &busy) == DEMO_AI_AUDIO_ERR_BUSY);
        assert(!demo_ai_audio_lease_is_valid(&stale));
        assert(demo_ai_audio_set_levels(&stale, 1, 1) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_apply_levels(&stale) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_init(&stale) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_prepare_session(&stale, 1, 1) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_stop(&stale) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_play(&stale, pcm, 320) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_record_20ms(&stale, pcm) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(demo_ai_audio_release(&stale) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(gain_writes() == before && s_stop_calls == old_stop);
        assert(s_play_calls == old_play && s_record_calls == old_record);
        assert(s_pause_calls == old_pause && s_init_calls == old_init);
        /* Deliberately omit set_levels/init: acquire must dirty a changed owner
         * even when both sessions kept precisely the same UI settings. */
        assert(demo_ai_audio_apply_levels(&lease) == 0);
        assert(gain_writes() == before + 3);
        expect_levels(target, level, 7);
        assert(demo_ai_audio_apply_levels(&lease) == 0);
        assert(gain_writes() == before + 3);
    }
    close_session(&lease);
}

static void check_device_signal(void)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    int16_t pcm[256];
    double max_error = 0.0;
    assert(sdk_pcm_model(7808, 53) == INT16_MAX); /* Observed Pro->Lite peak, old L8. */
    assert(sdk_pcm_model(-7808, 53) == INT16_MIN);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) == 0);
    s_check_identity = true;
    for (uint8_t level = 1; level <= 10; ++level) {
        assert(demo_ai_audio_set_levels(&lease, level, 7) == 0);
        int old = s_original_volume[level-1];
        /* es8311SetVolume writes floor(v*2550/1000) to register 0x32.
         * ES8311 datasheet: 0xBF=0dB, steps=0.5dB. Values <=0xBF attenuate.
         * https://files.waveshare.com/wiki/common/ES8311.DS.pdf */
        int new_register = s_codec * 2550 / 1000;
        double old_db = 20.0 * log10(sdk_gain_q8(old) / 256.0)
                        + (old * 2550 / 1000 - 191) * 0.5;
        double new_db = (new_register - 191) * 0.5;
        double error = fabs(new_db - old_db);
        assert(new_register >= 0 && new_register <= 183);
        assert(error <= 0.6);
        if (error > max_error) max_error = error;
        for (int first = INT16_MIN; first <= INT16_MAX; first += 256) {
            for (int i = 0; i < 256; ++i) pcm[i] = (int16_t)(first + i);
            assert(demo_ai_audio_play(&lease, pcm, 256) == 0);
        }
    }
    s_check_identity = false;
    assert(s_identity_samples == 10U * 65536U);
    close_session(&lease);
    printf("PASS: 65536 PCM values x 10 DEV levels; max small-signal error %.6f dB\n", max_error);
}

int main(void)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_NONE, &lease) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_COUNT, &lease) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, NULL) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    for (int owner = DEMO_AI_AUDIO_OWNER_AI; owner < DEMO_AI_AUDIO_OWNER_COUNT; ++owner) {
        check_owner((demo_ai_audio_owner_e)owner);
        if (owner == DEMO_AI_AUDIO_OWNER_DEVICE) continue;
        for (uint8_t level = 1; level <= 10; ++level)
            check_handoff((demo_ai_audio_owner_e)owner, level);
    }
    check_device_signal();
    assert(s_init_calls == 1 && s_power_calls == 3);
    assert(s_pause_calls == s_resume_calls);
    assert(s_sw_calls == s_codec_calls && s_sw_calls == s_mic_calls);
    assert(s_critical_depth == 0);
    assert(s_level_failure_cases == 6U * (DEMO_AI_AUDIO_OWNER_COUNT - DEMO_AI_AUDIO_OWNER_AI));
    printf("PASS: %u pending-level failure/retry cases across all owners and both playback states\n",
           s_level_failure_cases);
    puts("PASS: owner gain/mic levels, clean-path writes, owner handoff, stale leases, write retry and hardware format");
    return 0;
}
