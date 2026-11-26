#ifndef TIRTC_VIDEO_RX_SCHED_H
#define TIRTC_VIDEO_RX_SCHED_H

#include <stdbool.h>
#include "liot_os.h"

#if TIRTC_VIDEO_RX_PRIORITY
/* The call-video worker is the sole writer. For audio calls, release the
 * boost after at most four strips and reacquire only in a guarded strip. */
static bool tirtc_video_rx_priority_active;
static uint8_t tirtc_video_rx_priority_saved;
#endif

static inline void tirtc_video_rx_priority_begin(liot_task_t task,bool allow)
{
#if TIRTC_VIDEO_RX_PRIORITY
    if (allow && !tirtc_video_rx_priority_active)
        tirtc_video_rx_priority_active=
            liot_rtos_task_change_priority(task,TIRTC_VIDEO_RX_PRIORITY,
                                          &tirtc_video_rx_priority_saved)==LIOT_OSI_SUCCESS;
#else
    (void)task;
    (void)allow;
#endif
}

static inline bool tirtc_video_rx_priority_end(liot_task_t task)
{
#if TIRTC_VIDEO_RX_PRIORITY
    if (tirtc_video_rx_priority_active) {
        uint8_t previous;
        if (liot_rtos_task_change_priority(task,tirtc_video_rx_priority_saved,&previous)!=LIOT_OSI_SUCCESS)
            return false;
        tirtc_video_rx_priority_active=false;
    }
#else
    (void)task;
#endif
    return true;
}

static inline bool tirtc_video_rx_pause(liot_task_t task,unsigned completed_strips,bool protect_audio)
{
#if TIRTC_VIDEO_RX_PRIORITY
    if (tirtc_video_rx_priority_active && completed_strips && !(completed_strips%4U)) {
        if (protect_audio && !tirtc_video_rx_priority_end(task)) return false;
        liot_rtos_task_sleep_ms(1U);
    }
#else
    (void)task;
    (void)completed_strips;
    (void)protect_audio;
#endif
    return true;
}
#endif
