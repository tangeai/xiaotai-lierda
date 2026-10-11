/* Production audio adapter tests: fake the SDK/clock, never its mailbox. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "audio_device.h"
#include "liot_audio2.h"
#include "liot_gpio2.h"
#include "liot_os.h"

extern void demo_ai_audio_aec_service_for_test(void);

#define FRAME_SAMPLES DEMO_AI_AUDIO_FRAME_SAMPLES
#define FRAME_BYTES DEMO_AI_AUDIO_FRAME_BYTES
#define MAX_DELIVERIES 16U
#define MAX_PLAY_BYTES (FRAME_BYTES * 128U)

typedef struct {
    uint8_t data[FRAME_BYTES * 2U];
    uint16_t bytes;
} delivery_t;

static Liot_AudRecordCb_t s_record_cb;
static Liot_AudCb_t s_event_cb;
static void *s_event_context;
static delivery_t s_deliveries[MAX_DELIVERIES];
static unsigned s_delivery_count;
static uint32_t s_now, s_deliver_at;
static uint32_t s_slept_ms;
static int s_depth;
static unsigned s_init_calls, s_aec_calls, s_ans_calls, s_agc_calls;
static unsigned s_stream_starts, s_stream_stops, s_play_stops;
static unsigned s_plain_records, s_plain_plays, s_play_calls;
static unsigned s_level_calls;
static unsigned s_fail_init, s_fail_aec, s_fail_stream, s_fail_play_call;
static bool s_fail_record_stop;
static uint32_t s_vendor_stop_delay_ms, s_worker_resume_at;
static bool s_drain_probe, s_drain_timer_blocked;
static uint32_t s_drain_waited_ms, s_drain_timer_ms, s_drain_pending_bytes;
static unsigned s_drain_record_stops, s_drain_play_calls;
static size_t s_drain_accepted_bytes;
static demo_ai_audio_lease_t *s_drain_lease;
static bool s_defer_worker;
static bool s_fail_task, s_suppress_startup_callback;
static bool s_mutex_created, s_mutex_locked, s_fail_mutex_create, s_fail_mutex_lock;
static bool s_vendor_play_latency;
static bool s_tail_during_stop;
static bool s_recording;
static bool s_vem_running;
static bool s_in_record_callback;
static bool s_startup_callback_pending;
static bool s_task_created;
static bool s_servicing;
static demo_ai_audio_lease_t *s_stop_while_waiting;
static demo_ai_audio_lease_t *s_record_after_delivery;
static int s_nested_record_ret;
static int16_t s_nested_output[FRAME_SAMPLES];
static demo_ai_audio_lease_t *s_play_before_mute;
static int s_play_before_mute_ret;
static bool s_mute_in_progress;
static void (*s_after_exit_hook)(void);
static void (*s_before_enter_hook)(void);
static unsigned s_exit_hook_count, s_enter_hook_count;
static demo_ai_audio_lease_t *s_reused_lease, *s_release_tail_lease;
static unsigned s_handoff_stops;
static int s_release_tail_play_ret, s_release_tail_release_ret;
static bool s_release_tail_ran;
static void release_tail_hook(void);
static int s_sw_volume, s_codec_volume;
static uint8_t s_mic_gain_actual;
static int s_mic_volume_actual;
static bool s_volume_reset_enabled, s_volume_reset_pending, s_volume_read_zero;
static uint32_t s_volume_reset_delay, s_volume_reset_at;
static unsigned s_volume_stream_calls, s_volume_resets, s_volume_reads;
static demo_ai_audio_lease_t *s_volume_stop_lease;
static uint8_t s_accepted[MAX_PLAY_BYTES];
static size_t s_accepted_bytes;

void liot_rtos_enter_critical(void)
{
    assert(!s_in_record_callback);
    if (s_depth == 0 && s_before_enter_hook && --s_enter_hook_count == 0) {
        void (*hook)(void) = s_before_enter_hook;
        s_before_enter_hook = NULL;
        hook();
    }
    ++s_depth;
}
void liot_rtos_exit_critical(void)
{
    assert(s_depth > 0);
    --s_depth;
    if (s_depth == 0 && s_after_exit_hook && --s_exit_hook_count == 0) {
        void (*hook)(void) = s_after_exit_hook;
        s_after_exit_hook = NULL;
        hook();
    }
}
uint32_t liot_rtos_get_running_time(void) { return s_now; }
void liot_trace(const char *format, ...) { (void)format; }

LiotOSStatus_t liot_rtos_mutex_create(liot_mutex_t *mutex)
{
    assert(mutex && !s_in_record_callback && s_depth == 0);
    if (s_fail_mutex_create) { *mutex = NULL; return -1; }
    s_mutex_created = true;
    *mutex = &s_mutex_created;
    return LIOT_OSI_SUCCESS;
}
LiotOSStatus_t liot_rtos_mutex_lock(liot_mutex_t mutex, uint32_t timeout)
{
    assert(mutex == &s_mutex_created && s_mutex_created);
    assert(!s_in_record_callback && s_depth == 0);
    if (s_fail_mutex_lock) return -1;
    if (timeout == 0 && s_mutex_locked) return -1;
    assert(!s_mutex_locked); /* A nested wait cannot progress in this scheduler. */
    s_mutex_locked = true;
    return LIOT_OSI_SUCCESS;
}
LiotOSStatus_t liot_rtos_mutex_unlock(liot_mutex_t mutex)
{
    assert(mutex == &s_mutex_created && s_mutex_locked && s_depth == 0);
    s_mutex_locked = false;
    return LIOT_OSI_SUCCESS;
}

LiotOSStatus_t liot_rtos_task_create(liot_task_t *task, uint32_t stack_bytes,
    uint8_t priority, char *name, void (*entry)(void *), void *argument, ...)
{
    (void)argument;
    assert(s_depth == 0 && !s_in_record_callback);
    assert(task && stack_bytes >= 1024 && priority > 0 && name && entry);
    if (s_fail_task) { *task = NULL; return -1; }
    *task = (void *)&s_task_created;
    s_task_created = true;
    return LIOT_OSI_SUCCESS;
}

static void emit_pcm(Liot_AudRecordCb_t callback, uint8_t *data, uint16_t bytes)
{
    int previous_depth = s_depth;
    assert(callback != NULL && !s_in_record_callback);
    s_in_record_callback = true;
    callback(data, bytes);
    s_in_record_callback = false;
    assert(s_depth == previous_depth);
}

static void deliver_queued(void)
{
    unsigned count = s_delivery_count;
    s_delivery_count = 0;
    assert(s_record_cb != NULL);
    for (unsigned i = 0; i < count; ++i) {
        emit_pcm(s_record_cb, s_deliveries[i].data, s_deliveries[i].bytes);
        /* The vendor owns/reuses callback storage immediately. */
        memset(s_deliveries[i].data, 0xDD, sizeof(s_deliveries[i].data));
    }
}

void liot_rtos_task_sleep_ms(uint32_t ms)
{
    assert(s_depth == 0 && !s_in_record_callback);
    assert(ms > 0 && ms <= 1000);
    if (s_drain_probe && s_servicing && s_stream_stops == s_drain_record_stops) {
        int16_t refused[FRAME_SAMPLES] = {1234};
        int16_t output[FRAME_SAMPLES];
        /* A deliberately small DMA model checks the live drain window only.
         * It does not claim the real vendor timer always runs on schedule. */
        assert(!s_mutex_locked && !s_mute_in_progress);
        assert(s_recording && s_vem_running && s_record_cb);
        assert(s_codec_volume == 0);
        assert(s_play_calls == s_drain_play_calls);
        assert(s_accepted_bytes == s_drain_accepted_bytes);
        assert(demo_ai_audio_play(s_drain_lease, refused, FRAME_SAMPLES) < 0);
        for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 2345;
        assert(demo_ai_audio_record_20ms(s_drain_lease, output) < 0);
        for (unsigned i = 0; i < FRAME_SAMPLES; ++i) assert(output[i] == 2345);
        emit_pcm(s_record_cb, (uint8_t *)refused, FRAME_BYTES);
        s_drain_waited_ms += ms;
        s_drain_timer_ms += ms;
        while (s_drain_timer_ms >= 40U) {
            s_drain_timer_ms -= 40U;
            if (!s_drain_timer_blocked) {
                uint32_t consumed = s_drain_pending_bytes < FRAME_BYTES * 2U ?
                                    s_drain_pending_bytes : FRAME_BYTES * 2U;
                s_drain_pending_bytes -= consumed;
            }
        }
    }
    s_now += ms;
    s_slept_ms += ms;
    if (s_volume_reset_pending && (int32_t)(s_now - s_volume_reset_at) >= 0) {
        /* Real F6D_D type-9 TX is asynchronous: codecInit restores these
         * defaults after the third stream submission has returned. */
        s_volume_reset_pending = false;
        s_codec_volume = 65;
        s_mic_gain_actual = 8;
        s_mic_volume_actual = 191;
        ++s_volume_resets;
    }
    if (s_startup_callback_pending) {
        int16_t silence[FRAME_SAMPLES] = {0};
        s_startup_callback_pending = false;
        /* Codec initialization writes defaults asynchronously. Requested
         * levels must be restored after the first new-stream PCM. */
        s_codec_volume = 55;
        emit_pcm(s_record_cb, (uint8_t *)silence, FRAME_BYTES);
    }
    if (s_stop_while_waiting) {
        demo_ai_audio_lease_t *lease = s_stop_while_waiting;
        s_stop_while_waiting = NULL;
        assert(demo_ai_audio_stop(lease) == 0);
    }
    if (s_delivery_count && s_recording && (int32_t)(s_now - s_deliver_at) >= 0) {
        deliver_queued();
        if (s_record_after_delivery) {
            demo_ai_audio_lease_t *lease = s_record_after_delivery;
            s_record_after_delivery = NULL;
            /* Another task runs after ISR publication but before the first
             * record caller resumes and consumes its completed mailbox. */
            s_nested_record_ret = demo_ai_audio_record_20ms(lease, s_nested_output);
        }
    }
    if (s_defer_worker && (int32_t)(s_now - s_worker_resume_at) >= 0)
        s_defer_worker = false;
    if (s_task_created && !s_servicing && !s_defer_worker) {
        s_servicing = true;
        demo_ai_audio_aec_service_for_test();
        s_servicing = false;
    }
    assert(s_slept_ms < 120000); /* Permit uptime wrap and long-idle jumps. */
}

static void queue_bytes(const void *data, uint16_t bytes)
{
    assert(s_delivery_count < MAX_DELIVERIES && bytes <= sizeof(s_deliveries[0].data));
    memcpy(s_deliveries[s_delivery_count].data, data, bytes);
    s_deliveries[s_delivery_count++].bytes = bytes;
    s_deliver_at = s_now + 1U;
}

static void fill_ramp(int16_t *pcm, unsigned count, int base)
{
    for (unsigned i = 0; i < count; ++i) pcm[i] = (int16_t)(base + (int)i);
}

static void expect_unchanged(const int16_t *pcm, int16_t value)
{
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) assert(pcm[i] == value);
}

liot_gpioerr_e Liot_GpioInit(liot_gpio_e gpio, liot_gpiodir_e direction,
                           liot_gpiolvl_e level, liot_intcb_t *callback)
{
    assert(gpio == L_GPIO_25 && direction == L_IO_OUTPUT);
    assert(level == L_IO_HIGH && callback == NULL);
    return L_GPIO_ERR_SUCCESS;
}
liot_gpioerr_e Liot_AonPowerCtl(bool enable)
{ assert(enable); return L_GPIO_ERR_SUCCESS; }
liot_gpioerr_e Liot_SetVoltage(liot_powerdomain_e domain, liot_volt_e voltage)
{
    assert(domain == L_DOMAIN_ALL && voltage == L_VOLT_3_30V);
    return L_GPIO_ERR_SUCCESS;
}

Liot_AudErr_e Liot_AudioInit(Liot_AudHwConfig_t *config)
{
    assert(s_depth == 0 && config != NULL);
    assert(config->use3A == 1);
    assert(config->i2cNum == 0 && config->i2sNum == 0 && config->paGpioNum == 11);
    assert(config->codecType == L_AUD_ES8311 && config->samples == L_AUD_16K_SAMPLES);
    assert(config->frameSize == L_AUD_FRAMESIZE_16_16);
    assert(config->callback != NULL);
    s_event_cb = config->callback;
    s_event_context = config->cbContext;
    ++s_init_calls;
    return s_fail_init ? L_AUD_ERR_EXECUTE : L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioSetAec(Liot_AudAecConfig_t *config)
{
    assert(s_depth == 0 && config != NULL && config->bypass == 0);
    ++s_aec_calls;
    return s_fail_aec ? L_AUD_ERR_EXECUTE : L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioSetAns(Liot_AudAnsConfig_t *config)
{ assert(s_depth == 0 && config != NULL && config->bypass == 1); ++s_ans_calls; return L_AUD_ERR_SUCCESS; }
Liot_AudErr_e Liot_AudioSetAgc(Liot_AudAgcConfig_t *config)
{ assert(s_depth == 0 && config != NULL && config->bypass == 1); ++s_agc_calls; return L_AUD_ERR_SUCCESS; }
Liot_AudErr_e Liot_AudioSetVolume(int volume)
{ assert(s_depth == 0); s_sw_volume = volume; ++s_level_calls; return L_AUD_ERR_SUCCESS; }
Liot_AudErr_e Liot_AudioSetCodecVolume(int volume)
{
    assert(s_depth == 0);
    if (volume == 0 && s_play_before_mute) {
        int16_t input[FRAME_SAMPLES] = {100};
        demo_ai_audio_lease_t *lease = s_play_before_mute;
        unsigned play_stops = s_play_stops;
        unsigned record_stops = s_stream_stops;
        unsigned record_starts = s_stream_starts;
        bool servicing = s_servicing;
        s_play_before_mute = NULL;
        /* Preempt in the codec register write itself. The worker must not
         * dismantle or recreate the codec until this write has returned. */
        s_mute_in_progress = true;
        s_servicing = true;
        demo_ai_audio_aec_service_for_test();
        s_servicing = servicing;
        s_mute_in_progress = false;
        assert(s_play_stops == play_stops && s_stream_stops == record_stops);
        assert(s_stream_starts == record_starts);
        s_play_before_mute_ret = demo_ai_audio_play(lease, input, FRAME_SAMPLES);
    }
    s_codec_volume = volume;
    ++s_level_calls;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioSetMicVolume(uint8_t gain, int volume)
{
    assert(s_depth == 0 && gain <= 10 && volume >= 0);
    s_mic_gain_actual = gain;
    s_mic_volume_actual = volume;
    ++s_level_calls;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioGetCodecVolume(int *volume)
{
    assert(s_depth == 0 && !s_in_record_callback && s_mutex_locked && volume);
    assert(!s_mute_in_progress);
    ++s_volume_reads;
    /* Mirror register quantization, not the application's comparison.
     * The actual SDK also reports a failed bus read as success with zero. */
    unsigned reg = (unsigned)s_codec_volume * 255U / 100U;
    *volume = s_volume_read_zero ? 0 : (int)(reg * 100U / 255U);
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioPlayPause(void)
{ assert(s_depth == 0); return L_AUD_ERR_SUCCESS; }
Liot_AudErr_e Liot_AudioPlayResume(void)
{ assert(s_depth == 0); return L_AUD_ERR_SUCCESS; }
Liot_AudErr_e Liot_AudioStop(void)
{ assert(s_depth == 0); return L_AUD_ERR_SUCCESS; }
Liot_AudErr_e Liot_AudioRecord(uint8_t *data, int bytes)
{ (void)data; (void)bytes; ++s_plain_records; return L_AUD_ERR_EXECUTE; }
Liot_AudErr_e Liot_AudioPlay(uint8_t *data, int bytes)
{ (void)data; (void)bytes; ++s_plain_plays; return L_AUD_ERR_EXECUTE; }
Liot_AudErr_e Liot_AudioRecordStream(Liot_AudRecordCb_t callback)
{
    assert(s_depth == 0 && callback != NULL);
    assert(!s_mute_in_progress);
    ++s_stream_starts;
    s_record_cb = callback;
    if (s_fail_stream) return L_AUD_ERR_EXECUTE;
    /* Calling Start with VEM already running only replaces its callback.
     * It cannot repair I2S that PlayStreamStop has dismantled. */
    if (!s_vem_running) {
        s_vem_running = true;
        s_recording = true;
        s_startup_callback_pending = !s_suppress_startup_callback;
    }
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioRecordStreamStop(void)
{
    assert(s_depth == 0);
    assert(!s_mute_in_progress);
    if (s_drain_probe) {
        assert(s_stream_stops == s_drain_record_stops);
        assert(s_drain_waited_ms >= 200U && s_drain_waited_ms <= 210U);
        assert(s_recording && s_vem_running && s_codec_volume == 0);
        assert(s_play_calls == s_drain_play_calls);
        assert(s_accepted_bytes == s_drain_accepted_bytes);
    }
    ++s_stream_stops;
    if (s_fail_record_stop) return L_AUD_ERR_EXECUTE;
    if (s_tail_during_stop && s_record_cb) {
        int16_t tail[FRAME_SAMPLES];
        fill_ramp(tail, FRAME_SAMPLES, 9000);
        emit_pcm(s_record_cb, (uint8_t *)tail, FRAME_BYTES);
    }
    s_recording = false;
    s_vem_running = false;
    s_startup_callback_pending = false;
    s_record_cb = NULL;
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioPlayStream(uint8_t *data, uint16_t bytes)
{
    assert(s_depth == 0 && data != NULL && bytes > 0 && bytes % 2 == 0);
    ++s_play_calls;
    if (s_vendor_play_latency) s_now += 2U + s_play_calls % 4U;
    if (s_fail_play_call && s_play_calls >= s_fail_play_call) return L_AUD_ERR_EXECUTE;
    assert(s_accepted_bytes + bytes <= sizeof(s_accepted));
    memcpy(s_accepted + s_accepted_bytes, data, bytes);
    s_accepted_bytes += bytes;
    if (++s_volume_stream_calls == 3U && s_volume_reset_enabled) {
        s_volume_reset_pending = true;
        s_volume_reset_at = s_now + s_volume_reset_delay;
    }
    return L_AUD_ERR_SUCCESS;
}
Liot_AudErr_e Liot_AudioPlayStreamStop(void)
{
    assert(s_depth == 0);
    assert(!s_mute_in_progress);
    /* The SDK's own DeInit stops recording/VEM before playback. With live
     * RX, playback stop can enter shared I2S teardown before RX is stopped. */
    assert(!s_recording && !s_vem_running);
    ++s_play_stops;
    s_volume_stream_calls = 0;
    s_volume_reset_pending = false;
    if (s_drain_probe && s_drain_pending_bytes != 0U) {
        /* Once RX/DMA is stopped, the vendor timer cannot drain this ring.
         * Its public Stop still has to reach the normal 5-second fallback. */
        s_vendor_stop_delay_ms = 5000U;
    }
    while (s_vendor_stop_delay_ms) {
        uint32_t wait = s_vendor_stop_delay_ms < 100U ?
                        s_vendor_stop_delay_ms : 100U;
        s_vendor_stop_delay_ms -= wait;
        liot_rtos_task_sleep_ms(wait);
    }
    if (s_drain_probe) s_drain_pending_bytes = 0U;
    if (s_release_tail_lease) {
        /* The worker's acknowledgement is the next critical entry; release
         * committing its owner change is the one after that. */
        s_before_enter_hook = release_tail_hook;
        s_enter_hook_count = 2;
    }
    if (s_tail_during_stop && s_record_cb) {
        int16_t tail[FRAME_SAMPLES];
        fill_ramp(tail, FRAME_SAMPLES, 9000);
        emit_pcm(s_record_cb, (uint8_t *)tail, FRAME_BYTES);
    }
    /* Playback teardown must not implicitly stand in for stopping VEM. */
    s_recording = false;
    s_startup_callback_pending = false;
    return L_AUD_ERR_SUCCESS;
}

static demo_ai_audio_lease_t open_session(demo_ai_audio_owner_e owner)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    assert(demo_ai_audio_acquire(owner, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) == 0);
    assert(demo_ai_audio_is_ready(&lease));
    assert(s_init_calls > 0 && s_aec_calls > 0 && s_ans_calls > 0 && s_agc_calls > 0);
    return lease;
}
static void close_session(demo_ai_audio_lease_t *lease)
{
    assert(demo_ai_audio_stop(lease) == 0);
    assert(demo_ai_audio_release(lease) == 0);
    assert(!demo_ai_audio_lease_is_valid(lease));
}

static void test_capture(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES * 2U], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES * 2U, 100);
    queue_bytes(input, 128);
    queue_bytes((uint8_t *)input + 128, 512);
    queue_bytes(input + FRAME_SAMPLES, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    /* Bytes arriving after a completed request must not accumulate for it. */
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 12345;
    assert(demo_ai_audio_record_20ms(&lease, output) < 0);
    expect_unchanged(output, 12345);
    /* A large callback copies exactly one frame and discards its remainder. */
    queue_bytes(input, FRAME_BYTES * 2U);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    queue_bytes(input + FRAME_SAMPLES, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input + FRAME_SAMPLES, FRAME_BYTES) == 0);
    close_session(&lease);
}

static void pump(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 10)
        liot_rtos_task_sleep_ms(10);
}

static void test_timeout(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t output[FRAME_SAMPLES], input[FRAME_SAMPLES];
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 12345;
    uint32_t before = s_now;
    assert(demo_ai_audio_record_20ms(&lease, output) ==
           DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT);
    assert(s_now > before && s_now - before <= 200);
    expect_unchanged(output, 12345);
    assert(s_record_cb != NULL);
    emit_pcm(s_record_cb, NULL, 0);
    emit_pcm(s_record_cb, NULL, FRAME_BYTES);
    fill_ramp(input, FRAME_SAMPLES, 200);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    close_session(&lease);
}

static void test_capture_ready_contention(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES, 600);
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) s_nested_output[i] = 12345;
    s_record_after_delivery = &lease;
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(s_record_after_delivery == NULL && s_nested_record_ret < 0);
    expect_unchanged(s_nested_output, 12345);
    assert(memcmp(input, output, FRAME_BYTES) == 0);
    close_session(&lease);
}

static void test_overflow(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES, 200);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    for (unsigned frame = 0; frame < 512; ++frame) {
        for (unsigned i = 0; i < FRAME_SAMPLES; ++i) input[i] = (int16_t)frame;
        emit_pcm(s_record_cb, (uint8_t *)input, FRAME_BYTES);
    }
    /* Idle callbacks are not a backlog: a new request sees only fresh PCM. */
    fill_ramp(input, FRAME_SAMPLES, 1500);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    close_session(&lease);
}

static void test_stop_owner(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI), stale;
    int16_t input[FRAME_SAMPLES * 2U], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES * 2U, 100);
    queue_bytes(input, sizeof(input));
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    /* Cancel before the worker submits any old-owner playback. */
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
    pump(10); /* Enter the partial-slot wait before cancellation. */
    assert(s_accepted_bytes == 0);
    stale = lease;
    Liot_AudRecordCb_t tail = s_record_cb;
    s_tail_during_stop = true;
    assert(demo_ai_audio_stop(&lease) == 0);
    emit_pcm(tail, (uint8_t *)input, FRAME_BYTES); /* Late delivery while stopped. */
    assert(demo_ai_audio_release(&lease) == 0);
    lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    /* A complete new-owner frame must win over old queued/stop-tail data. */
    fill_ramp(input, FRAME_SAMPLES, 1500);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    assert(s_accepted_bytes == 0);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
    pump(200);
    assert(s_accepted_bytes >= FRAME_BYTES);
    assert(memcmp(s_accepted, input, FRAME_BYTES) == 0);
    for (size_t i = FRAME_BYTES; i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    unsigned stops = s_stream_stops + s_play_stops;
    assert(demo_ai_audio_stop(&stale) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(stops == s_stream_stops + s_play_stops);
    s_tail_during_stop = false;
    close_session(&lease);
}

static void test_permissions(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    demo_ai_audio_lease_t wrong = lease;
    int16_t output[FRAME_SAMPLES];
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 23456;
    ++wrong.generation;
    unsigned levels = s_level_calls, starts = s_stream_starts, plays = s_play_calls;
    assert(demo_ai_audio_record_20ms(&wrong, output) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_play(&wrong, output, FRAME_SAMPLES) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_set_levels(&wrong, 1, 1) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_stop(&wrong) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_release(&wrong) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(levels == s_level_calls && starts == s_stream_starts && plays == s_play_calls);
    expect_unchanged(output, 23456);
    close_session(&lease);
}

static void test_stop_wait(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t output[FRAME_SAMPLES];
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 12345;
    assert(demo_ai_audio_play(&lease, output, FRAME_SAMPLES) == 0);
    pump(160); /* Exercise stop callbacks on an active duplex engine. */
    s_tail_during_stop = true;
    s_stop_while_waiting = &lease;
    assert(demo_ai_audio_record_20ms(&lease, output) < 0);
    assert(s_stop_while_waiting == NULL);
    expect_unchanged(output, 12345);
    s_tail_during_stop = false;
    assert(demo_ai_audio_release(&lease) == 0);
}

static void test_stream_rebuild(bool played)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES, 1200);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    if (played) {
        assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
        pump(200);
        assert(s_play_calls > 0);
    }
    unsigned starts = s_stream_starts, stops = s_stream_stops, play_stops = s_play_stops;
    assert(demo_ai_audio_stop(&lease) == 0);
    s_accepted_bytes = 0;
    fill_ramp(input, FRAME_SAMPLES, 3200);
    queue_bytes(input, FRAME_BYTES);
    /* Leave room for asynchronous stream reconstruction before this fresh
     * business frame arrives. Its first silent callback is not our payload. */
    s_deliver_at = s_now + 25U;
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    if (played) {
        assert(s_play_stops > play_stops);
        assert(s_stream_starts > starts && s_stream_stops > stops);
    } else {
        /* No TX was started: avoiding the destructive vendor PlayStop is
         * the supported way to retain recording without a rebuild. */
        assert(s_play_stops == play_stops);
        assert(s_stream_starts == starts && s_stream_stops == stops);
    }
    assert(s_codec_volume == 64); /* DEV level 8 survived codec reinitialization. */
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
    pump(300);
    assert(s_accepted_bytes >= FRAME_BYTES);
    assert(memcmp(s_accepted, input, FRAME_BYTES) == 0);
    close_session(&lease);
}

static void test_rebuild_failure(bool no_callback)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES * 2U, 1900);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(10);
    assert(s_play_calls > 0);
    unsigned stops = s_stream_stops;
    if (no_callback) s_suppress_startup_callback = true;
    else s_fail_stream = 1;
    assert(demo_ai_audio_stop(&lease) < 0);
    assert(s_stream_stops >= stops + 2U); /* Tear down the partial restart too. */
    assert(!s_recording && !s_vem_running && !demo_ai_audio_is_ready(&lease));
    assert(s_codec_volume == 0);
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 12345;
    assert(demo_ai_audio_record_20ms(&lease, output) < 0);
    expect_unchanged(output, 12345);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) < 0);
    s_suppress_startup_callback = false;
    s_fail_stream = 0;
    unsigned starts = s_stream_starts;
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(s_stream_starts == starts && !demo_ai_audio_is_ready(&lease));
    assert(demo_ai_audio_release(&lease) == 0);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(s_stream_starts == starts && !demo_ai_audio_is_ready(&lease));
    assert(demo_ai_audio_release(&lease) == 0);
}

static void test_playback(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 3U], expected[FRAME_SAMPLES * 3U];
    for (unsigned i = 0; i < FRAME_SAMPLES * 3U; ++i)
        input[i] = i % 2 ? INT16_MIN : INT16_MAX;
    memcpy(expected, input, sizeof(input));
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 3U) == 0);
    assert(!demo_ai_audio_play_done(&lease));
    if (s_event_cb) s_event_cb(L_AUD_EVT_FINISH, s_event_context);
    assert(!demo_ai_audio_play_done(&lease));
    assert(memcmp(input, expected, sizeof(expected)) == 0);
    memset(input, 0, sizeof(input));
    pump(300);
    assert(s_accepted_bytes >= sizeof(expected));
    assert(memcmp(s_accepted, expected, sizeof(expected)) == 0);
    /* Any remainder is priming/tail silence, never a duplicate accepted block. */
    for (size_t i = sizeof(expected); i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    assert(s_codec_volume > 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

static void test_short_packet_continuity(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U];
    fill_ramp(input, FRAME_SAMPLES * 2U, 1000);
    /* Two ordinary network packets arrive on their 20-ms source clock.
     * Both are available before the vendor's three-slot playback startup.
     * The adapter must combine them instead of inserting 20 ms of zeroes
     * after the first packet merely because the feeder ran in between. */
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
    pump(20);
    assert(demo_ai_audio_play(&lease, input + FRAME_SAMPLES, FRAME_SAMPLES) == 0);
    pump(300);
    assert(s_accepted_bytes >= sizeof(input));
    assert(memcmp(s_accepted, input, sizeof(input)) == 0);
    for (size_t i = sizeof(input); i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

static void test_continuous_short_packets(void)
{
    enum { FRAMES = 50 };
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES];
    uint32_t before = s_now;
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        while (s_now - before < frame * 20U) liot_rtos_task_sleep_ms(1);
        fill_ramp(input, FRAME_SAMPLES, (int)(1000U + frame * 10U));
        assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
    }
    pump(300);
    assert(s_accepted_bytes >= FRAMES * sizeof(input));
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        fill_ramp(input, FRAME_SAMPLES, (int)(1000U + frame * 10U));
        assert(memcmp(s_accepted + frame * sizeof(input), input, sizeof(input)) == 0);
    }
    for (size_t i = FRAMES * sizeof(input); i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

static void test_uneven_packet_boundary(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 4U];
    fill_ramp(input, FRAME_SAMPLES * 4U, 1700);
    /* A 60-ms decoded burst leaves half a DMA slot. Its continuation arrives
     * on the source clock before the partial-slot wait expires. */
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 3U) == 0);
    pump(60);
    assert(demo_ai_audio_play(&lease, input + FRAME_SAMPLES * 3U, FRAME_SAMPLES) == 0);
    pump(300);
    assert(s_accepted_bytes >= sizeof(input));
    assert(memcmp(s_accepted, input, sizeof(input)) == 0);
    for (size_t i = sizeof(input); i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

static void test_short_tail(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES / 2U];
    fill_ramp(input, FRAME_SAMPLES / 2U, 800);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES / 2U) == 0);
    pump(30);
    assert(s_accepted_bytes == 0);
    pump(30);
    assert(s_accepted_bytes >= sizeof(input));
    assert(memcmp(s_accepted, input, sizeof(input)) == 0);
    pump(200);
    for (size_t i = sizeof(input); i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

static void test_stop_play_contention(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U] = {100};
    s_play_before_mute = &lease;
    assert(demo_ai_audio_stop(&lease) == 0);
    assert(s_play_before_mute == NULL && s_play_before_mute_ret < 0);
    assert(s_codec_volume == 0 && s_accepted_bytes == 0);
    /* Exercise the destructive vendor-stop path too: a capture-only stop
     * would skip it and hide the codec teardown versus mute race. */
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(10);
    assert(s_play_calls > 0 && s_accepted_bytes == sizeof(input));
    unsigned stops = s_play_stops;
    s_play_before_mute = &lease;
    assert(demo_ai_audio_stop(&lease) == 0);
    assert(s_play_before_mute == NULL && s_play_before_mute_ret < 0);
    assert(s_play_stops > stops && s_codec_volume == 0);
    assert(demo_ai_audio_is_ready(&lease));
    close_session(&lease);
}

static void reuse_lease_hook(void)
{
    int16_t input[FRAME_SAMPLES];
    uint32_t previous_generation = s_reused_lease->generation;
    assert(s_depth == 0 && !s_mutex_locked);
    assert(demo_ai_audio_release(s_reused_lease) == 0);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, s_reused_lease) == 0);
    assert(s_reused_lease->generation != previous_generation);
    assert(demo_ai_audio_prepare_session(s_reused_lease, 8, 7) == 0);
    fill_ramp(input, FRAME_SAMPLES, 2400);
    assert(demo_ai_audio_play(s_reused_lease, input, FRAME_SAMPLES) == 0);
    s_handoff_stops = s_play_stops;
}

static void test_stop_lease_reuse(void)
{
    /* Preempt after the entry snapshot, and again after the public validity
     * check but before private stop changes its epoch. The caller's lease
     * object is reused for the same owner with a newer generation. */
    for (unsigned boundary = 1; boundary <= 2; ++boundary) {
        demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
        int16_t expected[FRAME_SAMPLES];
        s_accepted_bytes = 0;
        s_reused_lease = &lease;
        s_exit_hook_count = boundary;
        s_after_exit_hook = reuse_lease_hook;
        assert(demo_ai_audio_stop(&lease) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
        assert(s_after_exit_hook == NULL && s_play_stops == s_handoff_stops);
        assert(demo_ai_audio_is_ready(&lease) && s_codec_volume > 0);
        pump(200);
        fill_ramp(expected, FRAME_SAMPLES, 2400);
        assert(s_accepted_bytes >= sizeof(expected));
        assert(memcmp(s_accepted, expected, sizeof(expected)) == 0);
        for (size_t i = sizeof(expected); i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
        close_session(&lease);
        s_reused_lease = NULL;
    }
}

static void release_tail_hook(void)
{
    int16_t input[FRAME_SAMPLES] = {123};
    demo_ai_audio_lease_t *lease = s_release_tail_lease;
    s_release_tail_lease = NULL;
    assert(lease && s_depth == 0 && !s_mutex_locked && s_codec_volume == 0);
    s_release_tail_play_ret = demo_ai_audio_play(lease, input, FRAME_SAMPLES);
    s_release_tail_release_ret = demo_ai_audio_release(lease);
    s_release_tail_ran = true;
}

static void test_release_tail(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U] = {100};
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(10);
    s_accepted_bytes = 0;
    s_release_tail_lease = &lease;
    assert(demo_ai_audio_release(&lease) == 0);
    assert(s_release_tail_ran && !s_before_enter_hook);
    assert(s_release_tail_play_ret < 0 && s_release_tail_release_ret < 0);
    assert(s_accepted_bytes == 0 && s_codec_volume == 0);
    assert(!demo_ai_audio_lease_is_valid(&lease));
    lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    assert(demo_ai_audio_is_ready(&lease));
    close_session(&lease);
}

static void test_playback_pacing(void)
{
    enum { CHUNKS = 50, SAMPLES = FRAME_SAMPLES * 2U };
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[SAMPLES];
    uint32_t before = s_now;
    s_vendor_play_latency = true;
    for (unsigned frame = 0; frame < CHUNKS; ++frame) {
        /* Keep six 40-ms chunks buffered without exceeding the local ring. */
        uint32_t due = frame < 6 ? 0U : (frame - 5U) * 40U;
        while (s_now - before < due) liot_rtos_task_sleep_ms(1);
        fill_ramp(input, SAMPLES, (int)(frame * 100U));
        assert(demo_ai_audio_play(&lease, input, SAMPLES) == 0);
    }
    while (s_now - before < CHUNKS * 40U + 5U) liot_rtos_task_sleep_ms(1);
    assert(s_accepted_bytes >= CHUNKS * sizeof(input));
    for (unsigned frame = 0; frame < CHUNKS; ++frame) {
        fill_ramp(input, SAMPLES, (int)(frame * 100U));
        assert(memcmp(s_accepted + frame * sizeof(input), input, sizeof(input)) == 0);
    }
    close_session(&lease);
}

static void test_backpressure(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U], rejected[FRAME_SAMPLES * 2U];
    fill_ramp(input, FRAME_SAMPLES * 2U, 300);
    fill_ramp(rejected, FRAME_SAMPLES * 2U, 7000);
    for (unsigned i = 0; i < 6; ++i)
        assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    assert(demo_ai_audio_play(&lease, rejected, FRAME_SAMPLES * 2U) < 0);
    assert(s_accepted_bytes == 0); /* No synchronous blocking in SDK queue. */
    pump(300);
    assert(s_accepted_bytes >= sizeof(input) * 6U);
    for (unsigned i = 0; i < 6; ++i)
        assert(memcmp(s_accepted + i * sizeof(input), input, sizeof(input)) == 0);
    /* The rejected submission contributed no prefix to the software queue. */
    for (size_t i = sizeof(input) * 6U; i < s_accepted_bytes; ++i) assert(s_accepted[i] == 0);
    close_session(&lease);
    assert(s_play_stops > 0);
}

/* Reference arithmetic for the ordinary SDK path retained by AEC-off. */
static int16_t original_sdk_gain(int16_t input, unsigned volume)
{
    int gain = 256 * (int)(volume / 10U) + 26 * (int)(volume % 10U);
    int product = input * gain;
    int output = product < 0 ? -((-product + 255) / 256) : product / 256;
    if (output > INT16_MAX) output = INT16_MAX;
    if (output < INT16_MIN) output = INT16_MIN;
    return (int16_t)output;
}

static void test_gain(void)
{
    static const unsigned volume[10] = {30,33,37,40,43,47,50,53,57,60};
    static const unsigned device_codec[10] = {38,41,46,50,53,57,61,64,69,72};
    static const int16_t values[] = {0,1,-1,100,-100,10000,-10000,INT16_MIN,INT16_MAX};
    for (unsigned owner = DEMO_AI_AUDIO_OWNER_AI; owner < DEMO_AI_AUDIO_OWNER_COUNT; ++owner) {
        for (unsigned level = 1; level <= 10; ++level) {
            demo_ai_audio_lease_t lease = open_session((demo_ai_audio_owner_e)owner);
            int16_t input[FRAME_SAMPLES], expected[FRAME_SAMPLES];
            s_accepted_bytes = 0;
            assert(demo_ai_audio_set_levels(&lease, (uint8_t)level, 7) == 0);
            for (unsigned i = 0; i < FRAME_SAMPLES; ++i) {
                input[i] = values[i % (sizeof(values) / sizeof(values[0]))];
                expected[i] = original_sdk_gain(input[i],
                    owner == DEMO_AI_AUDIO_OWNER_DEVICE ? 10 : volume[level - 1]);
            }
            assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) == 0);
            pump(160);
            assert(s_accepted_bytes >= sizeof(expected));
            assert(memcmp(s_accepted, expected, sizeof(expected)) == 0);
            assert(s_codec_volume == (int)(owner == DEMO_AI_AUDIO_OWNER_DEVICE ?
                device_codec[level - 1] : volume[level - 1]));
            close_session(&lease);
        }
    }
}

static void test_volume_reset(unsigned delay)
{
    const unsigned levels[] = {1U, 10U};
    const int codec[] = {30, 60}, device_codec[] = {38, 72};
    s_volume_reset_enabled = true;
    s_volume_reset_delay = delay;
    for (unsigned owner = DEMO_AI_AUDIO_OWNER_AI; owner < DEMO_AI_AUDIO_OWNER_COUNT; ++owner) {
        for (unsigned which = 0; which < 2U; ++which) {
            demo_ai_audio_lease_t lease = open_session((demo_ai_audio_owner_e)owner);
            int16_t input[FRAME_SAMPLES * 6U];
            fill_ramp(input, FRAME_SAMPLES * 6U, 100);
            unsigned resets = s_volume_resets, reads = s_volume_reads;
            int expected = owner == DEMO_AI_AUDIO_OWNER_DEVICE ?
                           device_codec[which] : codec[which];
            s_accepted_bytes = 0;
            assert(demo_ai_audio_set_levels(&lease, (uint8_t)levels[which], 7) == 0);
            assert(s_codec_volume == expected && s_mic_volume_actual == 207);
            assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
            /* No record, play, or explicit apply call from here. Even a
             * final short reply needs the worker to repair first-TX init. */
            pump(500);
            assert(s_volume_resets == resets + 1U);
            assert(s_codec_volume == expected);
            assert(s_mic_gain_actual == 9 && s_mic_volume_actual == 207);
            assert(s_volume_reads > reads && s_volume_reads - reads <= 6U);
            unsigned writes = s_level_calls;
            pump(300);
            /* A quantized, correct read must not rewrite every 100 ms. */
            assert(s_level_calls == writes);
            close_session(&lease);
        }
    }
}

static void test_volume_read_retry(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES * 6U] = {100};
    assert(demo_ai_audio_set_levels(&lease, 1, 7) == 0);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
    s_volume_read_zero = true;
    unsigned reads = s_volume_reads, writes = s_level_calls;
    pump(500);
    assert(s_volume_reads > reads && s_volume_reads - reads <= 6U);
    assert(s_level_calls > writes && s_level_calls - writes <= 18U);
    assert(demo_ai_audio_is_ready(&lease));
    /* Returning success/zero does not disable verification or latch fault. */
    s_volume_read_zero = false;
    s_codec_volume = 65;
    s_mic_gain_actual = 8;
    s_mic_volume_actual = 191;
    pump(105);
    assert(s_codec_volume == 30 && s_mic_volume_actual == 207);
    close_session(&lease);
}

static void test_volume_stop_owner(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    demo_ai_audio_lease_t old = lease;
    int16_t input[FRAME_SAMPLES * 6U] = {100};
    assert(demo_ai_audio_set_levels(&lease, 1, 7) == 0);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
    pump(200);
    unsigned reads = s_volume_reads;
    s_play_before_mute = &lease; /* Existing fake runs worker inside mute. */
    assert(demo_ai_audio_stop(&lease) == 0);
    assert(s_volume_reads == reads && s_codec_volume == 0);
    pump(300);
    assert(s_codec_volume == 0 && s_volume_reads == reads);
    assert(demo_ai_audio_release(&lease) == 0);
    lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    assert(demo_ai_audio_set_levels(&lease, 10, 7) == 0);
    assert(demo_ai_audio_apply_levels(&old) == DEMO_AI_AUDIO_ERR_INVALID_LEASE);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
    pump(200);
    s_codec_volume = 65;
    s_mic_volume_actual = 191;
    pump(105);
    assert(s_codec_volume == 72 && s_mic_volume_actual == 207);
    close_session(&lease);
}

static void volume_stop_snapshot_hook(void)
{
    assert(s_volume_stop_lease && !s_mutex_locked && !s_servicing);
    assert(demo_ai_audio_stop(s_volume_stop_lease) == 0);
    assert(s_codec_volume == 0);
}

static void test_volume_stale_service(void)
{
    /* Suspend the service after it has seen eligible playback, then let
     * foreground Stop finish without releasing that same lease. */
    for (unsigned boundary = 1; boundary <= 3; ++boundary) {
        demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
        int16_t input[FRAME_SAMPLES * 6U] = {100};
        assert(demo_ai_audio_set_levels(&lease, 1, 7) == 0);
        assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
        pump(200);
        s_volume_stop_lease = &lease;
        s_exit_hook_count = boundary;
        s_after_exit_hook = volume_stop_snapshot_hook;
        demo_ai_audio_aec_service_for_test();
        assert(s_after_exit_hook == NULL);
        assert(s_codec_volume == 0 && demo_ai_audio_is_ready(&lease));
        close_session(&lease);
        s_volume_stop_lease = NULL;
    }
}

static void test_volume_service_contention(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES * 6U] = {100};
    assert(demo_ai_audio_set_levels(&lease, 1, 7) == 0);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
    pump(200);
    s_codec_volume = 65;
    s_now += 101U;
    uint32_t before = s_now;
    unsigned reads = s_volume_reads;
    s_mutex_locked = true; /* Foreground owns the codec while worker runs. */
    demo_ai_audio_aec_service_for_test();
    assert(s_now == before && s_volume_reads == reads && s_codec_volume == 65);
    s_mutex_locked = false;
    pump(10);
    assert(s_volume_reads == reads + 1 && s_codec_volume == 30);
    close_session(&lease);
}

static void test_sdk_failure(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 6U];
    fill_ramp(input, FRAME_SAMPLES * 6U, 100);
    s_fail_play_call = 2;
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 6U) == 0);
    pump(160);
    assert(s_play_calls >= 2 && s_accepted_bytes > 0 && s_accepted_bytes < sizeof(input));
    assert(memcmp(s_accepted, input, s_accepted_bytes) == 0);
    assert(s_codec_volume == 0 && s_play_stops > 0);
    unsigned levels = s_level_calls;
    assert(demo_ai_audio_set_levels(&lease, 10, 10) < 0);
    assert(s_codec_volume == 0 && s_level_calls == levels);
    unsigned calls = s_play_calls;
    size_t accepted = s_accepted_bytes;
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES) < 0);
    pump(100);
    assert(s_play_calls == calls && s_accepted_bytes == accepted);
    /* Vendor failure is deliberately latched until reboot, even after drain. */
    assert(!demo_ai_audio_is_ready(&lease));
    /* Cleanup may have fully stopped VEM already, making Stop a successful
     * no-op; this must not clear the latched failure or restore readiness. */
    assert(demo_ai_audio_stop(&lease) <= 0);
    assert(!demo_ai_audio_is_ready(&lease));
    assert(s_codec_volume == 0);
    assert(demo_ai_audio_release(&lease) == 0);
}

static void test_config_failure(bool aec)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_AI, &lease) == 0);
    if (aec) s_fail_aec = 1; else s_fail_init = 1;
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(!demo_ai_audio_is_ready(&lease));
    assert(s_play_calls == 0);
    assert(demo_ai_audio_release(&lease) == 0);
}

static void test_stream_failure(void)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    int16_t output[FRAME_SAMPLES];
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_AI, &lease) == 0);
    for (unsigned i = 0; i < FRAME_SAMPLES; ++i) output[i] = 23456;
    s_fail_stream = 1;
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(!demo_ai_audio_is_ready(&lease));
    assert(demo_ai_audio_record_20ms(&lease, output) < 0);
    expect_unchanged(output, 23456);
    close_session(&lease);
}

static void test_startup_failure(bool worker)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_AI, &lease) == 0);
    if (worker) s_fail_task = true; else s_suppress_startup_callback = true;
    uint32_t before = s_now;
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(!demo_ai_audio_is_ready(&lease));
    if (worker) assert(s_stream_starts == 0);
    else assert(s_now - before >= 1000U && s_now - before < 1500U);
    assert(s_plain_records == 0 && s_play_calls == 0);
    close_session(&lease);
}

static void test_mutex_failure(void)
{
    demo_ai_audio_lease_t lease = DEMO_AI_AUDIO_LEASE_INIT;
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_AI, &lease) == 0);
    s_fail_mutex_create = true;
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(!demo_ai_audio_is_ready(&lease) && s_stream_starts == 0);
    close_session(&lease);
}

static void test_levels_contention(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    unsigned calls = s_level_calls;
    uint32_t before = s_now;
    s_fail_mutex_lock = true;
    assert(demo_ai_audio_set_levels(&lease, 2, 3) == 0);
    assert(s_now - before >= 100 && s_now - before <= 110);
    assert(calls == s_level_calls && demo_ai_audio_is_ready(&lease));
    s_fail_mutex_lock = false;
    assert(demo_ai_audio_apply_levels(&lease) == 0);
    assert(s_level_calls > calls && s_codec_volume == 41 && s_sw_volume == 10);
    calls = s_level_calls;
    assert(demo_ai_audio_apply_levels(&lease) == 0);
    assert(calls == s_level_calls); /* Pending change was committed only once. */
    close_session(&lease);
}

static void test_worker_timeout(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U] = {100};
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(10);
    uint32_t before = s_now;
    /* A vendor call stuck in the worker leaves control callers able to run. */
    s_servicing = true;
    assert(demo_ai_audio_stop(&lease) < 0);
    assert(s_now - before >= 10000U && s_now - before <= 10010U);
    assert(s_codec_volume == 0 && !demo_ai_audio_is_ready(&lease));
    before = s_now;
    assert(demo_ai_audio_release(&lease) == 0);
    assert(s_now - before <= 10U); /* Do not spend a second full stop timeout. */
    s_servicing = false;
    demo_ai_audio_aec_service_for_test();
    assert(s_play_stops > 0);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(!demo_ai_audio_is_ready(&lease));
    assert(s_codec_volume == 0); /* Late cleanup cannot revive a failed session. */
    assert(demo_ai_audio_release(&lease) == 0);
}

static void test_slow_cleanup(bool foreground_wait)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES * 2U], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES * 2U, 2100);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(10);
    assert(s_play_calls > 0);
    uint32_t before = s_now;
    if (foreground_wait) {
        /* A nested fake vendor sleep suspends this entire C call stack and
         * cannot exercise the foreground timeout. Hold the worker back while
         * the foreground actually polls for 5.1 seconds, then let it finish. */
        s_defer_worker = true;
        s_worker_resume_at = s_now + 5100U;
    } else {
        /* RecordStop may leave a nonempty vendor ring with DMA stopped.
         * PlayStop then reaches its ~5-second drain limit before resetting. */
        s_vendor_stop_delay_ms = 5100U;
    }
    assert(demo_ai_audio_stop(&lease) == 0);
    assert(s_now - before >= 5300U && s_now - before < 5400U);
    assert(!s_defer_worker && s_vendor_stop_delay_ms == 0U);
    assert(demo_ai_audio_is_ready(&lease) && s_codec_volume == 0);
    assert(demo_ai_audio_release(&lease) == 0);

    lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    fill_ramp(input, FRAME_SAMPLES * 2U, 4300);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    s_accepted_bytes = 0U;
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(200);
    assert(s_accepted_bytes >= sizeof(input));
    assert(memcmp(s_accepted, input, sizeof(input)) == 0);
    close_session(&lease);
}

static void test_drain_grace(bool timer_blocked)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES * 2U], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES * 2U, 1200);
    /* Eight accepted slots allow three priming slots plus a realistic
     * five-slot residual if timer consumption was previously delayed. */
    for (unsigned i = 0; i < 8U; ++i) {
        assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
        pump(40);
    }
    assert(s_play_calls >= 8U && s_recording && s_vem_running);
    s_drain_probe = true;
    s_drain_timer_blocked = timer_blocked;
    s_drain_pending_bytes = FRAME_BYTES * 10U;
    s_drain_record_stops = s_stream_stops;
    s_drain_play_calls = s_play_calls;
    s_drain_accepted_bytes = s_accepted_bytes;
    s_drain_lease = &lease;
    uint32_t before = s_now;
    assert(demo_ai_audio_stop(&lease) == 0);
    uint32_t elapsed = s_now - before;
    assert(s_drain_waited_ms >= 200U && s_drain_waited_ms <= 210U);
    assert(elapsed >= (timer_blocked ? 5200U : 200U));
    assert(elapsed < (timer_blocked ? 5250U : 250U));
    assert(s_drain_pending_bytes == 0U && s_vendor_stop_delay_ms == 0U);
    assert(s_stream_stops == s_drain_record_stops + 1U);
    assert(s_play_calls == s_drain_play_calls);
    assert(s_accepted_bytes == s_drain_accepted_bytes);
    assert(s_codec_volume == 0 && demo_ai_audio_is_ready(&lease));
    s_drain_probe = false;
    s_drain_lease = NULL;
    /* PCM arriving during the muted drain cannot satisfy the next request. */
    fill_ramp(input, FRAME_SAMPLES, 6100);
    queue_bytes(input, FRAME_BYTES);
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(input, output, FRAME_BYTES) == 0);
    close_session(&lease);
}

static void test_record_stop_failure(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_AI);
    int16_t input[FRAME_SAMPLES * 2U] = {100};
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(10);
    unsigned play_stops = s_play_stops, starts = s_stream_starts;
    s_fail_record_stop = true;
    assert(demo_ai_audio_stop(&lease) < 0);
    assert(s_play_stops == play_stops && s_stream_starts == starts);
    assert(s_recording && s_vem_running && s_codec_volume == 0);
    assert(!demo_ai_audio_is_ready(&lease));
    assert(demo_ai_audio_release(&lease) == 0);
    assert(demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_DEVICE, &lease) == 0);
    assert(demo_ai_audio_prepare_session(&lease, 8, 7) < 0);
    assert(s_stream_starts == starts && !demo_ai_audio_is_ready(&lease));
    assert(demo_ai_audio_release(&lease) == 0);
}

static void test_long_idle(void)
{
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 12U];
    fill_ramp(input, FRAME_SAMPLES * 12U, 1900);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    pump(300);
    assert(demo_ai_audio_play_done(&lease));
    /* A subscribed commercial room can remain open for over 24.8 days.
     * An expired 40/160-ms deadline must not become "future" again. */
    s_now += 0x80000100U;
    assert(demo_ai_audio_play_done(&lease));
    s_accepted_bytes = 0;
    unsigned calls = s_play_calls;
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 12U) == 0);
    pump(30);
    assert(s_play_calls - calls == 1U); /* Expired clock must not cause a burst. */
    pump(500);
    assert(s_accepted_bytes >= sizeof(input));
    assert(memcmp(s_accepted, input, sizeof(input)) == 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

static void test_clock_wrap(void)
{
    s_now = UINT32_MAX - 2U;
    demo_ai_audio_lease_t lease = open_session(DEMO_AI_AUDIO_OWNER_DEVICE);
    int16_t input[FRAME_SAMPLES * 2U], output[FRAME_SAMPLES];
    fill_ramp(input, FRAME_SAMPLES * 2U, 2300);
    assert(demo_ai_audio_play(&lease, input, FRAME_SAMPLES * 2U) == 0);
    queue_bytes(input, FRAME_BYTES);
    s_deliver_at = s_now + 20U;
    assert(demo_ai_audio_record_20ms(&lease, output) == 0);
    assert(memcmp(output, input, FRAME_BYTES) == 0);
    pump(300);
    assert(s_now < 1000U);
    assert(s_accepted_bytes >= sizeof(input));
    assert(memcmp(s_accepted, input, sizeof(input)) == 0);
    assert(demo_ai_audio_play_done(&lease));
    close_session(&lease);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "capture")) test_capture();
    else if (!strcmp(argv[1], "capture-ready-contention")) test_capture_ready_contention();
    else if (!strcmp(argv[1], "timeout")) test_timeout();
    else if (!strcmp(argv[1], "overflow")) test_overflow();
    else if (!strcmp(argv[1], "stop-owner")) test_stop_owner();
    else if (!strcmp(argv[1], "stop-wait")) test_stop_wait();
    else if (!strcmp(argv[1], "record-only-rebuild")) test_stream_rebuild(false);
    else if (!strcmp(argv[1], "duplex-rebuild")) test_stream_rebuild(true);
    else if (!strcmp(argv[1], "rebuild-failure")) test_rebuild_failure(false);
    else if (!strcmp(argv[1], "rebuild-timeout")) test_rebuild_failure(true);
    else if (!strcmp(argv[1], "permissions")) test_permissions();
    else if (!strcmp(argv[1], "playback")) test_playback();
    else if (!strcmp(argv[1], "short-packet-continuity")) test_short_packet_continuity();
    else if (!strcmp(argv[1], "continuous-short-packets")) test_continuous_short_packets();
    else if (!strcmp(argv[1], "uneven-packet-boundary")) test_uneven_packet_boundary();
    else if (!strcmp(argv[1], "short-tail")) test_short_tail();
    else if (!strcmp(argv[1], "stop-play-contention")) test_stop_play_contention();
    else if (!strcmp(argv[1], "stop-lease-reuse")) test_stop_lease_reuse();
    else if (!strcmp(argv[1], "release-tail")) test_release_tail();
    else if (!strcmp(argv[1], "playback-pacing")) test_playback_pacing();
    else if (!strcmp(argv[1], "gain")) test_gain();
    else if (!strcmp(argv[1], "volume-reset-1")) test_volume_reset(1);
    else if (!strcmp(argv[1], "volume-reset-40")) test_volume_reset(40);
    else if (!strcmp(argv[1], "volume-reset-120")) test_volume_reset(120);
    else if (!strcmp(argv[1], "volume-read-retry")) test_volume_read_retry();
    else if (!strcmp(argv[1], "volume-stop-owner")) test_volume_stop_owner();
    else if (!strcmp(argv[1], "volume-stale-service")) test_volume_stale_service();
    else if (!strcmp(argv[1], "volume-service-contention")) test_volume_service_contention();
    else if (!strcmp(argv[1], "backpressure")) test_backpressure();
    else if (!strcmp(argv[1], "sdk-failure")) test_sdk_failure();
    else if (!strcmp(argv[1], "init-failure")) test_config_failure(false);
    else if (!strcmp(argv[1], "aec-failure")) test_config_failure(true);
    else if (!strcmp(argv[1], "stream-failure")) test_stream_failure();
    else if (!strcmp(argv[1], "worker-failure")) test_startup_failure(true);
    else if (!strcmp(argv[1], "startup-timeout")) test_startup_failure(false);
    else if (!strcmp(argv[1], "mutex-failure")) test_mutex_failure();
    else if (!strcmp(argv[1], "levels-contention")) test_levels_contention();
    else if (!strcmp(argv[1], "worker-timeout")) test_worker_timeout();
    else if (!strcmp(argv[1], "slow-vendor-stop")) test_slow_cleanup(false);
    else if (!strcmp(argv[1], "slow-worker-completion")) test_slow_cleanup(true);
    else if (!strcmp(argv[1], "drain-grace")) test_drain_grace(false);
    else if (!strcmp(argv[1], "drain-grace-blocked")) test_drain_grace(true);
    else if (!strcmp(argv[1], "record-stop-failure")) test_record_stop_failure();
    else if (!strcmp(argv[1], "long-idle")) test_long_idle();
    else if (!strcmp(argv[1], "clock-wrap")) test_clock_wrap();
    else assert(!"Unknown scenario");
    assert(s_depth == 0 && !s_mutex_locked && s_plain_records == 0 && s_plain_plays == 0);
    printf("PASS: Lite AEC %s\n", argv[1]);
    return 0;
}
