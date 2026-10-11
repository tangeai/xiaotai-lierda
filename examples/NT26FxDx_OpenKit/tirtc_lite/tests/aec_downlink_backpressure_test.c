/* Actual feature functions are included below. No production test hooks. */
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
#include "backpressure_types.inc"

#define DEMO_WECHAT_IN_CALL 7
#define DEMO_DEV_CHAT_IN_CALL 7
#define DEMO_GROUP_CALL_NONE 0
#define DEMO_TIRTC_OWNER_WECHAT 1
#define DEMO_TIRTC_OWNER_LIVE 2
#define DEMO_TIRTC_OWNER_GROUP_ROOM 3
#define LIOT_OSI_SUCCESS 0
#define LIOT_NO_WAIT 0
typedef uint8_t uint8;
static unsigned clock_ms, critical_depth, captures, sends, attempts, accepted;
static unsigned busy_remaining;
static int terminal_error;
static bool playback_done;
static int16_t accepted_tags[64];
static uint32_t s_generation, s_epoch, s_tirtc_session_generation, s_tirtc_generation;
static tirtc_conn_t s_conn;
static bool s_tirtc_owned, s_audio_owned, s_request_hangup, s_remote_hangup_pending;
static bool s_conn_error_pending, s_disconnected_pending, s_home_allowed, s_active;
static bool s_remote_audio_enabled, s_stop_requested, s_remote_disconnected;
static bool s_tirtc_claimed, s_tirtc_release_pending, s_request_conn_error;
static unsigned s_tx_dropped, s_tx_frames, s_tx_session_queue_full;
static int s_state, s_connection_error;
static demo_ai_audio_lease_t s_audio_lease = DEMO_AI_AUDIO_LEASE_INIT;
static demo_ai_audio_lease_t s_audio = DEMO_AI_AUDIO_LEASE_INIT;
static int16_t s_capture_pcm[320], s_capture[320];
static uint8_t s_uplink_alaw[160], s_alaw[160];

#if TEST_KIND != 3
/* LIVE uses different names for the same queue boundary. */
#define s_rx_queue s_audio_queue
#define s_rx_pool s_audio_pool
#define s_rx_pool_used s_audio_pool_used
#define s_rx_queued s_audio_queued
#define s_rx_producers s_audio_producers
static void *s_audio_queue = (void *)1;
static RX_FRAME s_audio_pool[RX_DEPTH];
static bool s_audio_pool_used[RX_DEPTH];
static uint32_t s_audio_queued, s_audio_queued_samples, s_audio_producers;
static unsigned s_rx_dropped, s_rx_frames, s_prebuffer_start_ms;
static bool s_playback_error_logged;
static int16_t s_play_pcm[1280];
static uint8_t queue[RX_DEPTH];
static unsigned queue_count;
#if TEST_KIND == 1
static dev_rx_session_stats_t s_rx_session;
static uint32_t s_play_tail_ms;
static uint8_t s_play_tail_fraction;
static bool s_prebuffering, s_play_continuing;
static void *s_uplink_queue = (void *)2;
#endif
#else
static room_audio_slot_t s_rx[ROOM_RX_SLOTS];
static uint8_t s_rx_ring[ROOM_RX_SLOTS], s_rx_read, s_rx_write, s_rx_count;
static uint32_t s_rx_bytes, s_producers, s_mic_capture_generation, s_session_started;
static bool s_mic_gate, s_accept_audio, s_enabled, s_foreground;
static int s_call;
static int16_t s_play[ROOM_RX_MAX * 2U];
static struct { unsigned rx_dropped, tx_dropped; } s_snapshot;
static int s_commands[2];
static unsigned s_cmd_read, s_cmd_write, s_cmd_count;
#endif

static void liot_rtos_enter_critical(void) { ++critical_depth; }
static void liot_rtos_exit_critical(void) { assert(critical_depth); --critical_depth; }
static uint32_t liot_rtos_get_running_time(void) { return clock_ms; }
static void liot_trace(const char *format, ...) { (void)format; assert(!critical_depth); }
const char *TiRtcGetErrorStr(int error) { (void)error; return "fake boundary error"; }
#if TEST_KIND != 3
static int liot_rtos_queue_wait(void *q, void *item, unsigned size, unsigned timeout)
{
    assert(q == s_audio_queue && size == 1 && timeout == LIOT_NO_WAIT && !critical_depth);
    if (!queue_count) return -1;
    *(uint8_t *)item = queue[0];
    memmove(queue, queue + 1, --queue_count);
    return LIOT_OSI_SUCCESS;
}
#if TEST_KIND == 1
static int liot_rtos_queue_release(void *q, unsigned size, uint8 *item, unsigned timeout)
{
    dev_uplink_job_t *job = (dev_uplink_job_t *)item;
    assert(q == s_uplink_queue && size == sizeof(*job) && !critical_depth && !timeout);
    assert(job->generation == s_generation && job->tirtc_generation == s_tirtc_generation);
    assert(job->frame.length == 160 && job->frame.flags == TIRTC_AUDIOSAMPLE_8K16B1C);
    ++sends; return 0;
}
#endif
#else
static void room_zero(void *p, size_t n) { memset(p, 0, n); }
static void room_fail(int error) { (void)error; assert(!"unexpected uplink failure"); }
#endif

bool demo_ai_audio_play_done(const demo_ai_audio_lease_t *lease)
{
    (void)lease; assert(!critical_depth); return playback_done;
}
int demo_ai_audio_play(const demo_ai_audio_lease_t *lease, const int16_t *pcm, uint32_t count)
{
    (void)lease; assert(!critical_depth && count == 320);
    for (unsigned i = 1; i < count; ++i) assert(pcm[i] == pcm[0]);
    ++attempts;
    if (busy_remaining) { --busy_remaining; return DEMO_AI_AUDIO_ERR_PLAY_BUSY; }
    if (terminal_error) return terminal_error;
    accepted_tags[accepted++] = pcm[0];
    return 0;
}
int demo_ai_audio_record_20ms(const demo_ai_audio_lease_t *lease, int16_t *pcm)
{
    (void)lease; assert(!critical_depth);
    ++captures; clock_ms += 20;
    for (unsigned i = 0; i < 320; ++i) pcm[i] = (int16_t)i;
    return 0;
}
static int demo_tirtc_get_send_buffer_used(int owner, unsigned generation, size_t *used)
{
    (void)owner; (void)generation; *used = 0; return 0;
}
static int demo_tirtc_send_audio(int owner, unsigned generation, const TIRTCFRAMEINFO *frame, const void *data)
{
    (void)owner; (void)generation; (void)data; assert(!critical_depth);
    assert(frame->length == 160 && frame->media == TIRTC_AUDIO_ALAW &&
           frame->flags == TIRTC_AUDIOSAMPLE_8K16B1C);
    ++sends; return 0;
}
#include "backpressure_functions.inc"

static void reset(void)
{
    assert(!critical_depth);
    clock_ms = 1000; captures = sends = attempts = accepted = busy_remaining = 0;
    terminal_error = 0; playback_done = false;
    s_generation = s_epoch = s_tirtc_session_generation = s_tirtc_generation = 1;
    s_conn = (void *)1; s_state = DEMO_WECHAT_IN_CALL;
    s_tirtc_owned = s_audio_owned = s_home_allowed = s_active = s_remote_audio_enabled = true;
    s_tirtc_claimed = true;
    s_request_hangup = s_remote_hangup_pending = s_conn_error_pending = s_disconnected_pending = false;
    s_stop_requested = s_remote_disconnected = s_tirtc_release_pending = s_request_conn_error = false;
    s_connection_error = 0;
#if TEST_KIND != 3
    memset(s_audio_pool, 0, sizeof(s_audio_pool)); memset(s_audio_pool_used, 0, sizeof(s_audio_pool_used));
    s_audio_queued = s_audio_queued_samples = s_audio_producers = queue_count = 0;
    s_rx_dropped = s_rx_frames = s_prebuffer_start_ms = 0;
    s_playback_error_logged = false;
#if DEMO_AI_AUDIO_AEC_ENABLE
    s_play_pending_slot = -1;
#endif
#if TEST_KIND == 1
    memset(&s_rx_session, 0, sizeof(s_rx_session));
    s_play_continuing = true; s_prebuffering = false;
    s_play_tail_ms = clock_ms + 1000; s_play_tail_fraction = 0;
#endif
#else
    memset(s_rx, 0, sizeof(s_rx)); s_rx_read = s_rx_write = s_rx_count = 0;
    s_rx_bytes = s_producers = s_mic_capture_generation = 0;
    s_mic_gate = s_accept_audio = s_enabled = s_foreground = true;
    s_call = DEMO_GROUP_CALL_NONE; memset(&s_snapshot, 0, sizeof(s_snapshot));
#endif
}

/* Populate the already-copied RX queue boundary, with independent payload tags. */
static unsigned enqueue(uint8_t code)
{
    unsigned slot;
#if TEST_KIND != 3
    for (slot = 0; slot < RX_DEPTH && s_audio_pool_used[slot]; ++slot) {}
    assert(slot < RX_DEPTH && queue_count < RX_DEPTH);
    s_audio_pool_used[slot] = true;
    s_audio_pool[slot].generation = s_generation;
    s_audio_pool[slot].length = 160; s_audio_pool[slot].flags = TIRTC_AUDIOSAMPLE_8K16B1C;
    memset(s_audio_pool[slot].payload, code, 160);
    queue[queue_count++] = (uint8_t)slot; ++s_audio_queued; s_audio_queued_samples += 160;
#else
    for (slot = 0; slot < ROOM_RX_SLOTS && s_rx[slot].used; ++slot) {}
    assert(slot < ROOM_RX_SLOTS && s_rx_count < ROOM_RX_SLOTS);
    s_rx[slot].used = true; s_rx[slot].epoch = s_epoch;
    s_rx[slot].length = 160; s_rx[slot].received_ms = clock_ms;
    memset(s_rx[slot].bytes, code, 160);
    s_rx_ring[s_rx_write] = (uint8_t)slot; s_rx_write = (s_rx_write + 1U) % ROOM_RX_SLOTS;
    ++s_rx_count; s_rx_bytes += 160;
#endif
    return slot;
}
static unsigned dropped(void)
{
#if TEST_KIND == 3
    return s_snapshot.rx_dropped;
#else
    return s_rx_dropped;
#endif
}
static void tick(void)
{
    unsigned before = captures;
#if TEST_KIND == 0
    wx_service_downlink(); assert(wx_send_uplink() == 0);
#elif TEST_KIND == 1
    dev_service_downlink(); assert(dev_send_uplink() == 0);
#elif TEST_KIND == 2
    live_service_downlink(s_generation); assert(live_send_uplink_20ms(s_conn) == 0);
#else
    room_audio_tick();
#endif
    /* A busy playback must return to the real capture/send functions each tick. */
    assert(captures == before + 1 && sends == captures && !critical_depth);
}
static void drain(void)
{
#if TEST_KIND == 0
    wx_discard_audio();
#elif TEST_KIND == 1
    dev_drain_audio();
#elif TEST_KIND == 2
    live_discard_rx_queue();
#else
    room_drain_pools();
#endif
}
static void empty(void)
{
#if TEST_KIND != 3
    assert(queue_count == 0 && s_audio_queued == 0);
#if TEST_KIND == 1
    assert(s_audio_queued_samples == 0);
#endif
    for (unsigned i = 0; i < RX_DEPTH; ++i) assert(!s_audio_pool_used[i]);
#else
    assert(s_rx_count == 0 && s_rx_bytes == 0);
    for (unsigned i = 0; i < ROOM_RX_SLOTS; ++i) assert(!s_rx[i].used);
#endif
}
static void expect_tag(unsigned index, uint8_t code)
{
    assert(accepted_tags[index] == demo_g711_alaw_decode_sample(code));
}
static void retry_order(void)
{
    reset(); unsigned first = enqueue(0xD5); enqueue(0xD6);
    busy_remaining = 2; tick();
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(attempts == 1 && accepted == 0 && dropped() == 0);
    /* Arrivals during retry must not reuse the held slot or overtake it. */
    assert(enqueue(0xD7) != first); tick();
    assert(attempts == 2 && accepted == 0 && dropped() == 0);
    tick(); assert(accepted == 3 && attempts == 5 && dropped() == 0);
    expect_tag(0, 0xD5); expect_tag(1, 0xD6); expect_tag(2, 0xD7); empty();
    tick(); assert(accepted == 3); /* Never replay an accepted frame. */
#else
    /* The ordinary backend deliberately retains its previous drop behavior. */
    for (unsigned i = 0; i < 3; ++i) tick();
    assert(accepted == 0 && attempts == 2 && dropped() == 2); empty();
#endif
}
static void only_pending_and_finish(void)
{
    reset(); enqueue(0xD5); busy_remaining = 1; tick();
    playback_done = true;
#if TEST_KIND == 1
    s_play_tail_ms = clock_ms; s_play_tail_fraction = 0;
#endif
    tick();
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(accepted == 1 && dropped() == 0); expect_tag(0, 0xD5);
#else
    assert(accepted == 0 && dropped() == 1);
#endif
    empty();
}
static void stale_generation(void)
{
    reset(); enqueue(0xD5); busy_remaining = 1; tick();
    ++s_generation; ++s_epoch; enqueue(0xD6); tick();
    assert(accepted == 1); expect_tag(0, 0xD6); empty();
}
static void stop_and_new_session(void)
{
    reset(); enqueue(0xD5); enqueue(0xD6); busy_remaining = 1; tick();
    drain(); empty(); ++s_generation; ++s_epoch;
    unsigned old = accepted; enqueue(0xD7); tick();
    assert(accepted == old + 1); expect_tag(old, 0xD7); empty();
}
static void real_error(void)
{
    reset(); enqueue(0xD5); terminal_error = -41; tick();
    assert(accepted == 0 && attempts == 1 && dropped() == 1); empty();
    terminal_error = 0; tick(); assert(attempts == 1);
}
static void cleanup_with_rx_producer(void)
{
#if TEST_KIND != 3
    reset(); enqueue(0xD5); busy_remaining = 1; tick();
    unsigned reserved = RX_DEPTH - 1; s_audio_pool_used[reserved] = true; s_audio_producers = 1;
    drain(); assert(s_audio_pool_used[reserved]);
    for (unsigned i = 0; i < RX_DEPTH - 1; ++i) assert(!s_audio_pool_used[i]);
    s_audio_producers = 0; s_audio_pool_used[reserved] = false; drain(); empty();
#endif
}
static void group_deadline(void)
{
#if TEST_KIND == 3
    reset(); enqueue(0xD5); busy_remaining = 1; tick();
    clock_ms = 1161; enqueue(0xD6); tick();
    assert(dropped() == 1 && accepted == 1); expect_tag(0, 0xD6); empty();
    /* At the exact boundary, the pending frame remains eligible. */
    reset(); enqueue(0xD5); busy_remaining = 1; tick(); clock_ms = 1160; tick();
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(dropped() == 0 && accepted == 1);
#else
    assert(dropped() == 1 && accepted == 0);
#endif
    empty();
#endif
}
int main(void)
{
    retry_order(); only_pending_and_finish(); stale_generation(); stop_and_new_session();
    real_error(); cleanup_with_rx_producer(); group_deadline();
    printf("PASS kind=%d AEC=%d: retry/order/no-duplicate, lone pending, generation, cleanup, error, uplink%s\n",
           TEST_KIND, DEMO_AI_AUDIO_AEC_ENABLE, TEST_KIND == 3 ? ", 160ms expiry" : ", active RX producer");
    return 0;
}
