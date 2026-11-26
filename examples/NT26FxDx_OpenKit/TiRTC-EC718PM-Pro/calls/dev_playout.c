/* SPDX-License-Identifier: MIT */
#include "dev_playout.h"
#include <string.h>

#define DEV_PLAYOUT_DECAY_MS       5000U
#define DEV_PLAYOUT_PAUSE_GAP_MS    200U
#define DEV_PLAYOUT_MAX_DELTA_MS  10000U
#define DEV_PLAYOUT_WAIT_MIN_MS    200U

static void set_target(dev_playout_t *p, uint32_t target)
{
    if (target < DEV_PLAYOUT_MIN_MS) target = DEV_PLAYOUT_MIN_MS;
    if (target > DEV_PLAYOUT_MAX_MS) target = DEV_PLAYOUT_MAX_MS;
    p->target_ms = target;
    if (target < p->min_target_ms) p->min_target_ms = target;
    if (target > p->peak_target_ms) p->peak_target_ms = target;
}

void dev_playout_init(dev_playout_t *p)
{
    memset(p, 0, sizeof(*p));
    p->target_ms = DEV_PLAYOUT_INITIAL_MS;
    p->min_target_ms = p->target_ms;
    p->peak_target_ms = p->target_ms;
}

void dev_playout_observe(dev_playout_t *p, uint32_t media_ts_ms,
                         uint32_t duration_ms, uint32_t arrival_ms)
{
    uint32_t media_delta, arrival_delta, variation, desired;
    bool continuous;

    if (duration_ms == 0U || duration_ms > 120U) return;
    if (!p->have_arrival)
    {
        p->have_arrival = true;
        p->decay_since_ms = arrival_ms;
    }
    else
    {
        /* Unsigned subtraction also handles the 32-bit millisecond wrap. */
        media_delta = media_ts_ms - p->last_media_ms;
        arrival_delta = arrival_ms - p->last_arrival_ms;
        continuous = media_delta != 0U &&
            media_delta <= p->last_duration_ms + DEV_PLAYOUT_PAUSE_GAP_MS &&
            arrival_delta <= DEV_PLAYOUT_MAX_DELTA_MS;
        if (!continuous)
        {
            /* DTX/pause, duplicate/backward timestamp or a clock reset is not
             * evidence of arrival jitter. Keep the learned target, rebase.
             */
            p->jitter_q8 = 0U;
            p->decay_since_ms = arrival_ms;
            ++p->timing_resets;
        }
        else
        {
            variation = arrival_delta > media_delta ?
                arrival_delta - media_delta : media_delta - arrival_delta;
            /* Bound arithmetic and the influence of one extreme stall. */
            if (variation > 2000U) variation = 2000U;
            if (variation * 256U >= p->jitter_q8)
                p->jitter_q8 += (variation * 256U - p->jitter_q8) / 16U;
            else
                p->jitter_q8 -= (p->jitter_q8 - variation * 256U + 15U) / 16U;
            desired = DEV_PLAYOUT_MIN_MS + (p->jitter_q8 * 4U + 255U) / 256U;
            desired = ((desired + 19U) / 20U) * 20U;
            if (p->pending_underrun)
            {
                ++p->underruns;
                set_target(p, p->target_ms + 20U);
                p->decay_since_ms = arrival_ms;
            }
            if (desired > p->target_ms)
            {
                set_target(p, desired);
                p->decay_since_ms = arrival_ms;
            }
            else if ((uint32_t)(arrival_ms - p->decay_since_ms) >=
                     DEV_PLAYOUT_DECAY_MS)
            {
                /* Slow downward steps avoid oscillation after a burst. */
                if (desired + 20U <= p->target_ms)
                    set_target(p, p->target_ms - 20U);
                p->decay_since_ms = arrival_ms;
            }
        }
    }
    p->pending_underrun = false;
    p->last_media_ms = media_ts_ms;
    p->last_duration_ms = duration_ms;
    p->last_arrival_ms = arrival_ms;
}

void dev_playout_underrun(dev_playout_t *p)
{
    if (p->have_arrival) p->pending_underrun = true;
}

uint32_t dev_playout_target_ms(const dev_playout_t *p)
{
    return p->target_ms;
}

bool dev_playout_should_start(dev_playout_t *p, uint32_t queued_ms,
                              uint32_t queued_slots, uint32_t capacity,
                              uint32_t now_ms)
{
    uint32_t wait_limit, reserve;

    if (queued_slots == 0U || queued_ms == 0U)
    {
        p->waiting = false;
        return false;
    }
    if (!p->waiting)
    {
        p->waiting = true;
        p->wait_since_ms = now_ms;
    }
    wait_limit = p->target_ms > DEV_PLAYOUT_WAIT_MIN_MS ?
        p->target_ms : DEV_PLAYOUT_WAIT_MIN_MS;
    reserve = capacity / 4U;
    if (reserve == 0U) reserve = 1U;
    if (queued_ms >= p->target_ms ||
        (capacity != 0U && queued_slots >= capacity - reserve) ||
        (uint32_t)(now_ms - p->wait_since_ms) >= wait_limit)
    {
        p->waiting = false;
        return true;
    }
    return false;
}

void dev_playout_started(dev_playout_t *p)
{
    p->waiting = false;
}
