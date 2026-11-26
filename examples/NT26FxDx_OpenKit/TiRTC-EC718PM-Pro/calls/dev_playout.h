/* SPDX-License-Identifier: MIT */
#ifndef DEMO_DEV_PLAYOUT_H
#define DEMO_DEV_PLAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define DEV_PLAYOUT_INITIAL_MS 100U
#define DEV_PLAYOUT_MIN_MS      60U
#define DEV_PLAYOUT_MAX_MS     240U

/* One instance per call; caller serializes accesses with its RX queue lock.
 * This controller adapts start/rebuffer thresholds. It neither discards media
 * nor changes the sample rate, and cannot repair loss before the SDK callback.
 */
typedef struct {
    /* Public diagnostics: target plus smallest/largest target this session;
     * confirmed media-contiguous underruns and timing-reference resets.
     */
    uint32_t target_ms;
    uint32_t min_target_ms;
    uint32_t peak_target_ms;
    uint32_t jitter_q8;
    uint32_t last_media_ms;
    uint32_t last_duration_ms;
    uint32_t last_arrival_ms;
    uint32_t decay_since_ms;
    uint32_t wait_since_ms;
    uint32_t underruns;
    uint32_t timing_resets;
    bool have_arrival;
    bool waiting;
    bool pending_underrun;
} dev_playout_t;

void dev_playout_init(dev_playout_t *playout);
/* Observe accepted media in FIFO arrival order; timestamps/durations are ms.
 * Use media duration, not compressed packet byte count. Timestamp gaps over
 * 200 ms beyond the previous packet are treated as a pause/discontinuity.
 */
void dev_playout_observe(dev_playout_t *playout, uint32_t media_ts_ms,
                         uint32_t duration_ms, uint32_t arrival_ms);
/* Mark once on playback-empty transition after playback has started. The next
 * packet establishes whether it was a media-contiguous underrun or a pause.
 */
void dev_playout_underrun(dev_playout_t *playout);
uint32_t dev_playout_target_ms(const dev_playout_t *playout);
/* Call while playback is empty/prebuffering. Includes pending decoded media.
 * Starts short tails after max(200,target) ms or under queue-slot pressure.
 */
bool dev_playout_should_start(dev_playout_t *playout, uint32_t queued_ms,
                              uint32_t queued_slots, uint32_t capacity,
                              uint32_t now_ms);
void dev_playout_started(dev_playout_t *playout);

#endif
