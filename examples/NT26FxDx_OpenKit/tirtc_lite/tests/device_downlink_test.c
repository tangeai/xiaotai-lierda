/* Production receive/decode/scheduling functions; fake only RTOS and audio I/O. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio_device.h"
#include "g711_codec.h"
#include "tirtc/tiRTC.h"
#include "device_downlink_types.inc"

#define DEMO_DEV_CHAT_IN_CALL 7U
#define DEMO_DEV_CHAT_STOPPING 8U
#define LIOT_OSI_SUCCESS 0
#define LIOT_NO_WAIT 0U
#define NETWORK_SAMPLES_PER_MS (DEV_AUDIO_FRAME_BYTES / DEMO_AI_AUDIO_FRAME_MS)
#define PCM_SAMPLES_PER_MS (DEMO_AI_AUDIO_SAMPLE_RATE / 1000U)
static unsigned s_state, s_generation;
static tirtc_conn_t s_conn;
static void *s_audio_queue = (void *)1;
static dev_audio_frame_t s_audio_pool[DEV_AUDIO_QUEUE_DEPTH];
static bool s_audio_pool_used[DEV_AUDIO_QUEUE_DEPTH];
static uint32_t s_audio_queued, s_audio_queued_samples, s_audio_producers;
static uint32_t s_rx_dropped, s_rx_frames, s_prebuffer_start_ms, s_play_tail_ms;
static uint8_t s_play_tail_fraction;
static bool s_prebuffering, s_play_continuing, s_downlink_reject_logged, s_playback_error_logged;
static int16_t s_play_pcm[DEV_PLAY_PCM_MAX_SAMPLES];
static demo_ai_audio_lease_t s_audio_lease = DEMO_AI_AUDIO_LEASE_INIT;
static dev_rx_session_stats_t s_rx_session;
static uint8_t queue[DEV_AUDIO_QUEUE_DEPTH];
static unsigned queue_count, critical_depth, accepted, restarts, gap_ms;
static bool play_done, ever_played, enqueue_fail, play_fail;
static void (*enqueue_hook)(void);
static uint64_t wall_ms, driver_tail_samples;
static unsigned play_cost_ms, receive_sequence, checked_samples;
static unsigned worker_returns;
/* Keep an independent FIFO of callback input, not a copy of production PCM.
 * It catches incorrect payload copying, packet order and both duplicated samples. */
typedef struct {
    unsigned length;
    uint8_t payload[DEV_AUDIO_PAYLOAD_MAX];
} expected_audio_t;
static expected_audio_t expected_queue[DEV_AUDIO_QUEUE_DEPTH], expected_play;
static const uint8_t *callback_payload;
static unsigned callback_length;
static bool expected_play_valid;

static void liot_rtos_enter_critical(void) { ++critical_depth; }
static void liot_rtos_exit_critical(void) { assert(critical_depth); --critical_depth; }
static uint32_t liot_rtos_get_running_time(void) { return (uint32_t)wall_ms; }
static void liot_trace(const char *format, ...) { (void)format; assert(!critical_depth); }
static void dev_signal(void) { assert(!critical_depth); }
static int liot_rtos_queue_release(void *q, unsigned size, const void *item, unsigned timeout)
{
    assert(q == s_audio_queue && size == 1U && timeout == LIOT_NO_WAIT && !critical_depth);
    if (enqueue_hook) enqueue_hook();
    if (enqueue_fail || queue_count == DEV_AUDIO_QUEUE_DEPTH) return -1;
    assert(callback_payload && callback_length <= DEV_AUDIO_PAYLOAD_MAX);
    expected_queue[queue_count].length = callback_length;
    memcpy(expected_queue[queue_count].payload, callback_payload, callback_length);
    queue[queue_count++] = *(const uint8_t *)item;
    return LIOT_OSI_SUCCESS;
}
static int liot_rtos_queue_wait(void *q, void *item, unsigned size, unsigned timeout)
{
    assert(q == s_audio_queue && size == 1U && timeout == LIOT_NO_WAIT && !critical_depth);
    if (!queue_count) return -1;
    *(uint8_t *)item = queue[0];
    expected_play = expected_queue[0];
    expected_play_valid = true;
    memmove(queue, queue + 1, --queue_count);
    memmove(expected_queue, expected_queue + 1, queue_count * sizeof(expected_queue[0]));
    return LIOT_OSI_SUCCESS;
}
bool demo_ai_audio_play_done(const demo_ai_audio_lease_t *lease)
{
    assert(lease == &s_audio_lease); return play_done;
}
static int16_t reference_alaw(uint8_t code)
{
    unsigned value = code ^ 0x55U;
    unsigned exponent = (value >> 4) & 7U;
    unsigned mantissa = value & 15U;
    int magnitude = exponent ? (int)((2U * mantissa + 33U) << (exponent + 2U)) :
                               (int)(16U * mantissa + 8U);
    return (int16_t)((value & 128U) ? magnitude : -magnitude);
}
int demo_ai_audio_play(const demo_ai_audio_lease_t *lease, const int16_t *pcm, uint32_t samples)
{
    uint64_t now_samples;
    assert(lease == &s_audio_lease && !critical_depth && samples && !(samples & 1U));
    assert(expected_play_valid && samples == expected_play.length * 2U);
    for (unsigned i = 0; i < expected_play.length; ++i) {
        int16_t expected = reference_alaw(expected_play.payload[i]);
        assert(pcm[i * 2U] == expected && pcm[i * 2U + 1U] == expected);
    }
    checked_samples += samples;
    expected_play_valid = false;
    /* A slow successful SDK call advances time before accepting its PCM.
     * This is a scheduling fake, not a model of a physical DMA completion. */
    wall_ms += play_cost_ms;
    now_samples = wall_ms * PCM_SAMPLES_PER_MS;
    if (play_fail) return -11;
    if (driver_tail_samples <= now_samples) {
        if (ever_played && driver_tail_samples < now_samples) {
            ++restarts; gap_ms += (unsigned)((now_samples - driver_tail_samples) / PCM_SAMPLES_PER_MS);
        }
        driver_tail_samples = now_samples;
    }
    driver_tail_samples += samples;
    play_done = false; ever_played = true; ++accepted;
    return 0;
}

#include "device_downlink_functions.inc"

static void at(uint64_t time)
{
    assert(time >= wall_ms); /* Slow I/O tests must never rewind the fake clock. */
    wall_ms = time;
    if (ever_played && driver_tail_samples <= wall_ms * PCM_SAMPLES_PER_MS) play_done = true;
}
static void reset(uint64_t time)
{
    assert(!critical_depth);
    wall_ms = time; driver_tail_samples = time * PCM_SAMPLES_PER_MS;
    memset(s_audio_pool_used, 0, sizeof(s_audio_pool_used));
    s_audio_queued = s_audio_queued_samples = s_audio_producers = queue_count = 0;
    s_rx_dropped = s_rx_frames = accepted = restarts = gap_ms = 0;
    s_state = DEMO_DEV_CHAT_IN_CALL; s_generation = 1; s_conn = (tirtc_conn_t)1;
    play_done = true; ever_played = enqueue_fail = play_fail = false; enqueue_hook = NULL;
    play_cost_ms = receive_sequence = checked_samples = worker_returns = 0;
    callback_payload = NULL; callback_length = 0; expected_play_valid = false;
    s_downlink_reject_logged = s_playback_error_logged = false;
    dev_reset_downlink(); dev_reset_rx_session_stats();
}
static void receive(unsigned length)
{
    TIRTCFRAMEINFO frame;
    uint8_t data[DEV_AUDIO_PAYLOAD_MAX];
    memset(&frame, 0, sizeof(frame));
    assert(length <= sizeof(data));
    ++receive_sequence;
    for (unsigned i = 0; i < length; ++i)
        data[i] = (uint8_t)(receive_sequence * 67U + i * 29U +
                            (i >> 8) * 11U + (receive_sequence >> 3) * 17U);
    frame.stream_id = DEV_STREAM_ID; frame.media = TIRTC_AUDIO_ALAW;
    frame.flags = TIRTC_AUDIOSAMPLE_8K16B1C; frame.length = length;
    callback_payload = data; callback_length = length;
    dev_on_audio(s_conn, &frame, data);
    callback_payload = NULL;
    memset(data, 0xa5, sizeof(data)); /* Callback memory is no longer borrowed. */
}
/* Feed coalesced packets without assuming one packet fills the prebuffer. */
static unsigned receive_samples(unsigned samples)
{
    unsigned packets = 0;
    while (samples) {
        unsigned count = samples < DEV_AUDIO_PAYLOAD_MAX ? samples : DEV_AUDIO_PAYLOAD_MAX;
        receive(count); samples -= count; ++packets;
    }
    return packets;
}
static unsigned receive_ms(unsigned duration)
{
    return receive_samples(duration * NETWORK_SAMPLES_PER_MS);
}
static void assert_empty_pool(void)
{
    assert(!s_audio_queued && !s_audio_queued_samples && !queue_count && !s_audio_producers);
    for (unsigned i = 0; i < DEV_AUDIO_QUEUE_DEPTH; ++i) assert(!s_audio_pool_used[i]);
}
static void test_cadence(void)
{
    for (unsigned period = DEMO_AI_AUDIO_FRAME_MS;
         period <= 2U * DEMO_AI_AUDIO_FRAME_MS; period += DEMO_AI_AUDIO_FRAME_MS)
        for (unsigned service = DEMO_AI_AUDIO_FRAME_MS;
             service <= DEMO_AI_AUDIO_FRAME_MS + 3U; ++service)
            for (unsigned phase = 0; phase < period; ++phase) {
                reset(UINT32_MAX - 100U);
                uint64_t base = wall_ms;
                for (unsigned t = 0; t < 4000; ++t) {
                    at(base + t);
                    if (t >= phase && (t - phase) % period == 0)
                        for (unsigned n = 0; n < period / DEMO_AI_AUDIO_FRAME_MS; ++n)
                            receive(DEV_AUDIO_FRAME_BYTES);
                    if (t % service == 0) dev_service_downlink();
                }
                assert(accepted && s_rx_dropped == 0 && restarts == 0);
            }
    puts("PASS: regular packets / paired bursts, service phases and clock wrap");
}
static void test_prebuffer_and_late_finish(void)
{
    reset(0);
    unsigned initial = receive_ms(DEV_PREBUFFER_MS);
    assert(initial <= DEV_PLAY_BATCH_MAX);
    dev_service_downlink();
    assert(accepted == initial && s_play_tail_ms == DEV_PREBUFFER_MS && !s_play_tail_fraction);
    at(DEV_PREBUFFER_MS - DEMO_AI_AUDIO_FRAME_MS / 2U);
    receive(DEV_AUDIO_FRAME_BYTES); receive(DEV_AUDIO_FRAME_BYTES);
    play_done = true; /* delayed FINISH for an older submission */
    dev_service_downlink();
    assert(accepted == initial + 2U && s_audio_queued == 0);
    assert(s_play_tail_ms == DEV_PREBUFFER_MS + 2U * DEMO_AI_AUDIO_FRAME_MS && restarts == 0);

    reset(UINT32_MAX - 30U); uint64_t base = wall_ms;
    receive(DEV_AUDIO_FRAME_BYTES); dev_service_downlink(); assert(!accepted);
    at(base + DEV_PREBUFFER_TIMEOUT_MS - 1U); dev_service_downlink(); assert(!accepted);
    at(base + DEV_PREBUFFER_TIMEOUT_MS); dev_service_downlink(); assert(accepted == 1);

    reset(0); receive(1); receive(1); receive(1); dev_service_downlink(); assert(!accepted);
    at(DEV_PREBUFFER_TIMEOUT_MS); dev_service_downlink();
    assert(accepted == 3 && s_play_tail_fraction == 6);
    for (unsigned i = 0; i < 5; ++i) receive(1);
    dev_service_downlink();
    assert(s_play_tail_ms == DEV_PREBUFFER_TIMEOUT_MS + 1U && !s_play_tail_fraction);
    assert_empty_pool();
    puts("PASS: duration-based coalesced/short packet buffering, timeout and late FINISH");
}
static void test_delayed_network_bursts(void)
{
    /* Source keeps producing while delivery stalls, then catches up in order. */
    const unsigned delays[] = {100U, 120U, 160U};
    for (unsigned delay_index = 0; delay_index < sizeof(delays) / sizeof(delays[0]); ++delay_index)
        for (unsigned service = DEMO_AI_AUDIO_FRAME_MS;
             service <= DEMO_AI_AUDIO_FRAME_MS + 3U; ++service)
            for (unsigned service_phase = 0; service_phase < service; ++service_phase) {
                reset(UINT32_MAX - 250U); uint64_t base = wall_ms;
                unsigned held = 0, arrivals = 0;
                unsigned release = 1000U + delays[delay_index];
                for (unsigned t = 0; t < 3200U; ++t) {
                    at(base + t);
                    if (t % DEMO_AI_AUDIO_FRAME_MS == 0U) {
                        if (t >= 1000U && t <= release) ++held;
                        else { receive(DEV_AUDIO_FRAME_BYTES); ++arrivals; }
                    }
                    if (t == release) {
                        assert(held && held < DEV_AUDIO_QUEUE_DEPTH);
                        while (held) { receive(DEV_AUDIO_FRAME_BYTES); --held; ++arrivals; }
                    }
                    if (t >= service_phase && (t - service_phase) % service == 0U)
                        dev_service_downlink();
                }
                assert(arrivals == 3200U / DEMO_AI_AUDIO_FRAME_MS);
                assert(accepted && !s_rx_dropped && !restarts && !gap_ms);
                assert(!s_rx_session.rebuffer_count);
            }
    puts("PASS: 100/120/160ms delivery stalls and ordered catch-up bursts keep playback continuous");
}
static void test_real_underrun_rebuffers(void)
{
    reset(100U); unsigned initial = receive_ms(DEV_PREBUFFER_MS);
    dev_service_downlink(); assert(accepted == initial);
    uint64_t returned = wall_ms + DEV_PREBUFFER_MS + DEMO_AI_AUDIO_FRAME_MS;
    at(returned); receive(DEV_AUDIO_FRAME_BYTES); dev_service_downlink();
    assert(accepted == initial && s_prebuffering && !s_play_continuing);
    assert(s_rx_session.rebuffer_count == 1U);
    at(returned + DEV_PREBUFFER_TIMEOUT_MS - 1U); dev_service_downlink();
    assert(accepted == initial && s_rx_session.rebuffer_count == 1U);
    unsigned extra = receive_ms(DEV_PREBUFFER_MS - DEMO_AI_AUDIO_FRAME_MS);
    dev_service_downlink();
    assert(accepted == initial + 1U + extra && restarts == 1U);
    assert(s_rx_session.rebuffer_count == 1U && !s_prebuffering);
    assert_empty_pool();
    puts("PASS: actual underrun waits for a new prebuffer instead of playing each isolated arrival");
}
static void test_large_burst_is_bounded(void)
{
    reset(0);
    /* Current profile: 24 slots / 8 submissions. Read both from production. */
    assert(DEV_AUDIO_QUEUE_DEPTH == 3U * DEV_PLAY_BATCH_MAX);
    assert(DEV_AUDIO_QUEUE_DEPTH * DEV_AUDIO_FRAME_BYTES >= DEV_PREBUFFER_MS * NETWORK_SAMPLES_PER_MS);
    for (unsigned i = 0; i < DEV_AUDIO_QUEUE_DEPTH; ++i) receive(DEV_AUDIO_FRAME_BYTES);
    for (unsigned round = 1; round <= 3U; ++round) {
        at((round - 1U) * DEMO_AI_AUDIO_FRAME_MS);
        unsigned before = accepted;
        dev_service_downlink();
        assert(accepted - before == DEV_PLAY_BATCH_MAX);
        assert(accepted == round * DEV_PLAY_BATCH_MAX);
        assert(queue_count == DEV_AUDIO_QUEUE_DEPTH - accepted);
        assert(s_audio_queued_samples == queue_count * DEV_AUDIO_FRAME_BYTES);
    }
    assert(!s_rx_dropped && !restarts);
    assert_empty_pool();
    puts("PASS: full receive queue drains in three bounded playback batches");
}
static void test_slot_pressure_prebuffer(void)
{
    const unsigned threshold = DEV_AUDIO_QUEUE_DEPTH - DEV_PLAY_BATCH_MAX;
    for (unsigned wrap = 0; wrap < 2U; ++wrap) {
        reset(wrap ? UINT32_MAX - 30U : 0U);
        uint64_t base = wall_ms;
        for (unsigned i = 0; i < threshold - 1U; ++i) receive(40U);
        dev_service_downlink();
        assert(!accepted && s_prebuffering);
        receive(40U); dev_service_downlink();
        assert(accepted == DEV_PLAY_BATCH_MAX && !s_prebuffering);
        while (queue_count) {
            at(wall_ms + DEMO_AI_AUDIO_FRAME_MS);
            dev_service_downlink();
        }
        assert(accepted == threshold && !s_rx_dropped);
        assert_empty_pool();
        at(base + threshold * 5U + DEMO_AI_AUDIO_FRAME_MS);
        for (unsigned i = 0; i < threshold - 1U; ++i) receive(40U);
        dev_service_downlink();
        assert(accepted == threshold && s_rx_session.rebuffer_count == 1U);
        receive(40U); dev_service_downlink();
        assert(accepted == threshold + DEV_PLAY_BATCH_MAX);
        assert(!s_prebuffering && !s_rx_dropped);
    }
    /* Ordinary 20-ms frames must still wait for 200 ms of audio. */
    reset(0);
    for (unsigned i = 0; i < DEV_PREBUFFER_MS / DEMO_AI_AUDIO_FRAME_MS - 1U; ++i)
        receive(DEV_AUDIO_FRAME_BYTES);
    dev_service_downlink(); assert(!accepted);
    receive(DEV_AUDIO_FRAME_BYTES); dev_service_downlink();
    assert(accepted == DEV_PLAY_BATCH_MAX && !s_prebuffering);
    puts("PASS: short-packet slot pressure starts/restarts early; normal 200ms threshold preserved");
}
static void test_continuous_short_packets(void)
{
    const unsigned periods[] = {5U, 8U};
    for (unsigned p = 0; p < sizeof(periods) / sizeof(periods[0]); ++p)
        for (unsigned service = 20U; service <= 23U; ++service)
            for (unsigned phase = 0; phase < service; ++phase)
                for (unsigned packet_phase = 0; packet_phase < periods[p]; ++packet_phase) {
                    reset(UINT32_MAX - 100U);
                    uint64_t base = wall_ms;
                    unsigned high_water = 0;
                    for (unsigned t = 0; t < 2400U; ++t) {
                        at(base + t);
                        if (t >= packet_phase && (t - packet_phase) % periods[p] == 0U)
                            receive(periods[p] * NETWORK_SAMPLES_PER_MS);
                        if (queue_count > high_water) high_water = queue_count;
                        if (t >= phase && (t - phase) % service == 0U) dev_service_downlink();
                    }
                    assert(accepted && !s_rx_dropped && high_water < DEV_AUDIO_QUEUE_DEPTH);
                    assert(!s_rx_session.pool_fail && !s_rx_session.queue_fail);
                    assert(accepted + queue_count == s_rx_session.audio_received);
                    assert(!restarts && !gap_ms);
                }
    puts("PASS: continuous 5/8ms packets across 20..23ms worker phases and clock wrap; no pool loss");
}
static void service_and_mark_worker_return(void)
{
    dev_service_downlink();
    /* The production worker can now reach its next Record call. This marker
     * proves return from RX service, not real ADC cadence or RTOS fairness. */
    ++worker_returns;
}
static void test_slow_play_budget(void)
{
    const unsigned costs[] = {20U, 7U, 45U};
    assert(DEV_PLAY_BUDGET_MS == 20U);
    for (unsigned wrap = 0; wrap < 2U; ++wrap)
        for (unsigned n = 0; n < sizeof(costs) / sizeof(costs[0]); ++n) {
            reset(wrap ? UINT32_MAX - 5U : 100U);
            uint64_t base = wall_ms;
            for (unsigned i = 0; i < DEV_AUDIO_QUEUE_DEPTH; ++i)
                receive(DEV_AUDIO_FRAME_BYTES);
            play_cost_ms = costs[n];
            unsigned expected = (DEV_PLAY_BUDGET_MS + play_cost_ms - 1U) / play_cost_ms;
            if (expected > DEV_PLAY_BATCH_MAX) expected = DEV_PLAY_BATCH_MAX;
            service_and_mark_worker_return();
            assert(accepted == expected && worker_returns == 1U);
            assert(wall_ms - base == expected * play_cost_ms);
            assert((uint32_t)((uint32_t)wall_ms - (uint32_t)base) == expected * play_cost_ms);
            assert(queue_count == DEV_AUDIO_QUEUE_DEPTH - expected);
            assert(s_audio_queued_samples == queue_count * DEV_AUDIO_FRAME_BYTES);
            assert(!s_rx_dropped && !s_rx_session.play_fail);
            /* One slow call may exceed the budget; it cannot be interrupted.
             * Remaining packets must survive and play in their original order. */
            if (play_cost_ms > DEV_PLAY_BUDGET_MS)
                assert(accepted == 1U && wall_ms - base > DEV_PLAY_BUDGET_MS);
            play_cost_ms = 0;
            while (queue_count) {
                at(wall_ms + DEMO_AI_AUDIO_FRAME_MS);
                unsigned before = accepted;
                service_and_mark_worker_return();
                assert(accepted > before && accepted - before <= DEV_PLAY_BATCH_MAX);
            }
            assert(accepted == DEV_AUDIO_QUEUE_DEPTH);
            assert(checked_samples == DEV_AUDIO_QUEUE_DEPTH * DEV_AUDIO_FRAME_BYTES * 2U);
            assert(!s_rx_dropped && !s_rx_session.pool_fail && !s_rx_session.queue_fail);
            assert_empty_pool();
        }
    puts("PASS: slow Play 20/7/45ms and clock wrap return to worker; queued audio survives in order (no hard SDK time limit)");
}
static void test_varied_payload_order(void)
{
    const unsigned lengths[] = {1U, 159U, 160U, 161U, 319U, 320U, 639U, 640U};
    reset(0);
    unsigned total = 0;
    for (unsigned i = 0; i < DEV_AUDIO_QUEUE_DEPTH; ++i) {
        unsigned length = lengths[i % (sizeof(lengths) / sizeof(lengths[0]))];
        receive(length); total += length;
    }
    while (queue_count) service_and_mark_worker_return();
    assert(accepted == DEV_AUDIO_QUEUE_DEPTH && checked_samples == total * 2U);
    assert(worker_returns == (DEV_AUDIO_QUEUE_DEPTH + DEV_PLAY_BATCH_MAX - 1U) / DEV_PLAY_BATCH_MAX);
    assert(!s_rx_dropped);
    assert_empty_pool();
    puts("PASS: varying short/odd/coalesced payloads match every independently decoded sample and packet order");
}
static void test_hangup_clears_large_queue(void)
{
    reset(0);
    for (unsigned i = 0; i < DEV_AUDIO_QUEUE_DEPTH; ++i) receive(DEV_AUDIO_FRAME_BYTES);
    assert(queue_count == DEV_AUDIO_QUEUE_DEPTH);
    dev_service_downlink();
    assert(queue_count == DEV_AUDIO_QUEUE_DEPTH - DEV_PLAY_BATCH_MAX);
    /* Refill released slots, then follow the production hangup reset/drain. */
    for (unsigned i = 0; i < DEV_PLAY_BATCH_MAX; ++i) receive(DEV_AUDIO_FRAME_BYTES);
    assert(queue_count == DEV_AUDIO_QUEUE_DEPTH);
    s_state = DEMO_DEV_CHAT_STOPPING;
    dev_reset_downlink(); dev_drain_audio();
    assert_empty_pool();
    assert(!s_prebuffering && !s_play_continuing && !s_play_tail_ms && !s_play_tail_fraction);
    receive(DEV_AUDIO_FRAME_BYTES); assert_empty_pool();
    puts("PASS: hangup drains every slot of the enlarged queue and rejects late arrivals");
}
static void change_session_during_enqueue(void)
{
    s_state = DEMO_DEV_CHAT_STOPPING; ++s_generation;
    dev_reset_rx_session_stats(); enqueue_fail = true;
}
static void test_failures_and_lifecycle(void)
{
    dev_rx_session_stats_t snapshot;
    reset(50);
    for (unsigned n = 0; n < DEV_AUDIO_QUEUE_DEPTH + 1; ++n) receive(DEV_AUDIO_FRAME_BYTES);
    assert(s_rx_session.audio_received == DEV_AUDIO_QUEUE_DEPTH + 1U && s_rx_session.pool_fail == 1);
    assert(s_audio_queued_samples == DEV_AUDIO_QUEUE_DEPTH * DEV_AUDIO_FRAME_BYTES && s_audio_producers == 0);
    dev_drain_audio(); assert(!s_audio_queued && !s_audio_queued_samples && !queue_count);
    enqueue_fail = true; receive(DEV_AUDIO_FRAME_BYTES); enqueue_fail = false;
    assert(s_rx_session.queue_fail == 1 && !s_audio_queued_samples);
    receive(0); assert(s_rx_session.format_reject == 1);
    receive_ms(DEV_PREBUFFER_MS); play_fail = true; dev_service_downlink();
    assert(s_rx_session.play_fail == 1 && !accepted && !s_play_continuing);
    play_fail = false;
    receive_samples(DEV_PREBUFFER_MS * NETWORK_SAMPLES_PER_MS - s_audio_queued_samples);
    dev_service_downlink(); assert(s_play_tail_ms == 50U + DEV_PREBUFFER_MS);
    assert_empty_pool();

    s_state = DEMO_DEV_CHAT_STOPPING;
    dev_snapshot_rx_session(&snapshot); dev_reset_downlink(); dev_drain_audio();
    receive(DEV_AUDIO_FRAME_BYTES); assert(s_rx_session.audio_received == snapshot.audio_received);
    assert(!s_play_continuing && !s_prebuffering && !s_audio_queued_samples);
    ++s_generation; dev_reset_rx_session_stats();
    s_state = DEMO_DEV_CHAT_IN_CALL; s_rx_frames = 0; play_done = true;
    dev_rx_note(DEV_RX_QUEUE_FAIL, s_generation - 1); assert(!s_rx_session.queue_fail);
    receive(DEV_AUDIO_FRAME_BYTES); s_audio_pool[queue[0]].generation = s_generation - 1;
    assert(dev_play_one() == 1 && !s_audio_queued_samples && !s_play_continuing);

    reset(300); enqueue_hook = change_session_during_enqueue; receive(DEV_AUDIO_FRAME_BYTES); enqueue_hook = NULL;
    assert(!s_rx_session.queue_fail && !s_audio_queued_samples && !s_audio_producers);
    dev_drain_audio();

    reset(UINT32_MAX - 15U); uint64_t base = wall_ms;
    receive(DEV_AUDIO_FRAME_BYTES); at(base + 40); receive(DEV_AUDIO_FRAME_BYTES);
    assert(s_rx_session.max_callback_gap_ms == 40);
    at(base + 9040); s_state = DEMO_DEV_CHAT_STOPPING; dev_snapshot_rx_session(&snapshot);
    assert(snapshot.max_callback_gap_ms == 40 && snapshot.tail_silence_ms == 9000);
    reset(0); at(5000); dev_snapshot_rx_session(&snapshot);
    assert(!snapshot.max_callback_gap_ms && !snapshot.tail_silence_ms);
    puts("PASS: pool/queue/format/play failures, stale generation, reconnect reset, separate silent tail stats");
}
int main(void)
{
    test_cadence(); test_prebuffer_and_late_finish(); test_delayed_network_bursts();
    test_slot_pressure_prebuffer(); test_continuous_short_packets();
    test_real_underrun_rebuffers(); test_large_burst_is_bounded();
    test_slow_play_budget(); test_varied_payload_order();
    test_hangup_clears_large_queue(); test_failures_and_lifecycle();
    assert(!critical_depth);
    return 0;
}
