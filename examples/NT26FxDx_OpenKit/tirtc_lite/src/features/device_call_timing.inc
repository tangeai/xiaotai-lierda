/* SPDX-License-Identifier: MIT
 * Session-scoped, low-volume timing. RX/connection callbacks only save clocks;
 * no allocation or printing on those paths. All times are local monotonic ms.
 * play_submit is acceptance by the audio backend, NOT acoustic sound onset.
 */
#ifndef TIRTC_LITE_DEV_TIMING
#define TIRTC_LITE_DEV_TIMING 1
#endif
#if !TIRTC_LITE_DEV_TIMING
#define dev_timing_begin(...) ((void)0)
#define dev_timing_action_locked(...) ((void)0)
#define dev_timing_note_rx(...) ((void)0)
#define dev_timing_note_p2p(...) ((void)0)
#define dev_timing_log_p2p(...) ((void)0)
#define dev_timing_now(...) ((void)0)
#define dev_timing_first_play(...) ((void)0)
#define dev_timing_end(...) ((void)0)
#else
typedef struct {
    uint32_t generation, session, start_ms, action_ms, rx_ms, p2p_ms, play_ms;
    bool active, caller, action_seen, rx_seen, p2p_seen, play_seen;
} dev_timing_t;
static dev_timing_t s_dev_timing;

static void dev_timing_begin(bool caller, uint32_t generation, uint32_t session,
                              bool action_seen, uint32_t action_ms)
{
    uint32_t now = liot_rtos_get_running_time();
    liot_rtos_enter_critical();
    memset(&s_dev_timing, 0, sizeof(s_dev_timing));
    s_dev_timing.active = true;
    s_dev_timing.caller = caller;
    s_dev_timing.generation = generation;
    s_dev_timing.session = session;
    s_dev_timing.start_ms = action_seen ? action_ms : now;
    s_dev_timing.action_seen = action_seen;
    s_dev_timing.action_ms = action_ms;
    liot_rtos_exit_critical();
}

/* Answer API already holds the critical section. Preserve the first key time. */
static void dev_timing_action_locked(uint32_t generation, uint32_t now)
{
    if (s_dev_timing.active && s_dev_timing.generation == generation &&
        !s_dev_timing.action_seen) {
        s_dev_timing.action_seen = true;
        s_dev_timing.action_ms = now;
    }
}

static bool dev_timing_snapshot(uint32_t generation, dev_timing_t *out)
{
    bool valid;
    liot_rtos_enter_critical();
    valid = s_dev_timing.active && s_dev_timing.generation == generation;
    if (valid) *out = s_dev_timing;
    liot_rtos_exit_critical();
    return valid;
}

static void dev_timing_log_at(const char *stage, uint32_t generation,
                              uint32_t now, int result)
{
    dev_timing_t t;
    if (!dev_timing_snapshot(generation, &t)) return;
    liot_trace("[DEV-TIME] session=%u gen=%u role=%s stage=%s tick_ms=%u "
               "total_ms=%u action_ms=%d result=%d\r\n",
               (unsigned)t.session, (unsigned)t.generation,
               t.caller ? "caller" : "callee", stage, (unsigned)now,
               (unsigned)(now - t.start_ms),
               t.action_seen ? (int)(uint32_t)(now - t.action_ms) : -1, result);
}

static void dev_timing_now(const char *stage, uint32_t generation, int result)
{
    dev_timing_log_at(stage, generation, liot_rtos_get_running_time(), result);
}

static void dev_timing_note_rx(uint32_t generation, uint32_t now)
{
    liot_rtos_enter_critical();
    if (s_dev_timing.active && s_dev_timing.generation == generation &&
        !s_dev_timing.rx_seen) {
        s_dev_timing.rx_seen = true;
        s_dev_timing.rx_ms = now;
    }
    liot_rtos_exit_critical();
}

static void dev_timing_note_p2p(uint32_t generation, uint32_t now)
{
    liot_rtos_enter_critical();
    if (s_dev_timing.active && s_dev_timing.generation == generation &&
        !s_dev_timing.p2p_seen) {
        s_dev_timing.p2p_seen = true;
        s_dev_timing.p2p_ms = now;
    }
    liot_rtos_exit_critical();
}

static void dev_timing_log_p2p(uint32_t generation)
{
    dev_timing_t t;
    if (dev_timing_snapshot(generation, &t) && t.p2p_seen)
        dev_timing_log_at("p2p_callback", generation, t.p2p_ms, 0);
    dev_timing_now("p2p_worker", generation, 0);
}

static void dev_timing_first_play(uint32_t generation)
{
    dev_timing_t t;
    uint32_t now = liot_rtos_get_running_time();
    bool valid;
    liot_rtos_enter_critical();
    valid = s_dev_timing.active && s_dev_timing.generation == generation &&
            !s_dev_timing.play_seen;
    if (valid) {
        s_dev_timing.play_seen = true;
        s_dev_timing.play_ms = now;
        t = s_dev_timing;
    }
    liot_rtos_exit_critical();
    if (!valid) return;
    if (t.rx_seen) dev_timing_log_at("first_rx", generation, t.rx_ms, 0);
    dev_timing_log_at("first_play_submit", generation, now, 0);
    liot_trace("[DEV-TIME] session=%u first_play action_to_submit_ms=%d "
               "rx_to_submit_ms=%d p2p_to_submit_ms=%d\r\n",
               (unsigned)t.session,
               t.action_seen ? (int)(uint32_t)(now - t.action_ms) : -1,
               t.rx_seen ? (int)(uint32_t)(now - t.rx_ms) : -1,
               t.p2p_seen ? (int)(uint32_t)(now - t.p2p_ms) : -1);
}

static void dev_timing_end(uint32_t generation, int result, uint32_t stop_ms)
{
    dev_timing_t t;
    uint32_t now = liot_rtos_get_running_time();
    if (!dev_timing_snapshot(generation, &t)) return;
    liot_rtos_enter_critical();
    if (s_dev_timing.generation == generation) s_dev_timing.active = false;
    liot_rtos_exit_critical();
    liot_trace("[DEV-TIME] session=%u end result=%d total_ms=%u cleanup_ms=%u "
               "rx_seen=%u play_seen=%u action_to_submit_ms=%d rx_to_submit_ms=%d\r\n",
               (unsigned)t.session, result, (unsigned)(now - t.start_ms),
               (unsigned)(now - stop_ms), t.rx_seen ? 1U : 0U, t.play_seen ? 1U : 0U,
               t.action_seen && t.play_seen ? (int)(uint32_t)(t.play_ms - t.action_ms) : -1,
               t.rx_seen && t.play_seen ? (int)(uint32_t)(t.play_ms - t.rx_ms) : -1);
}
#endif
