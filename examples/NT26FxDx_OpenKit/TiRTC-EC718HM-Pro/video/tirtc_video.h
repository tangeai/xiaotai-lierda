#ifndef TIRTC_VIDEO_H
#define TIRTC_VIDEO_H
#include <stdbool.h>
#include <stdint.h>
#include "tirtc_runtime.h"
typedef struct {
    uint32_t generation, tx_frames, rx_frames, dropped_frames;
    uint32_t capture_ms, encode_ms, decode_ms;
    bool active, ready, camera_enabled;
    int error;
} tirtc_video_snapshot_t;
/* One low-priority owner serializes camera and JPEG codec use. Commands are
 * bounded, asynchronous RAM operations. stop/is_stopped fence all in-flight
 * camera DMA, codec work and managed sends before the call releases its lease. */
int tirtc_video_start_service(void);
/* incoming is the device's call direction, not the later WX join notification.
 * Freeze display orientation for this generation; camera upload is unchanged. */
int tirtc_video_start(demo_tirtc_owner_e owner, uint32_t generation, bool wechat,
                     bool incoming);
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
/* For an explicitly armed, owned inbound Windows DEV_CHAT call only.
 * Camera load is independent of the deterministic pixels supplied to JPEG. */
int tirtc_video_start_test_fixture(uint32_t generation, unsigned flags);
/* In receive-count mode only, emit admitted JPEGs as RX progress for the
 * acceptance transport. Production status and codec diagnostics remain separate. */
void tirtc_video_get_test_acceptance_snapshot(tirtc_video_snapshot_t *out);
#endif
/* Remote viewing uploads stream 11 only. Initial camera permission is committed
 * before the worker can run; later subscription changes use set_enabled.
 * Repeating an active generation preserves its current enable state. */
int tirtc_video_start_live(uint32_t generation, bool enabled);
void tirtc_video_stop(uint32_t generation);
bool tirtc_video_is_stopped(uint32_t generation);
int tirtc_video_set_enabled(uint32_t generation, bool enabled);
void tirtc_video_receive(uint32_t generation, const TIRTCFRAMEINFO *frame, const void *data);
void tirtc_video_get_snapshot(tirtc_video_snapshot_t *out);
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
typedef enum {
    TIRTC_VIDEO_UI_REPLACED,
    TIRTC_VIDEO_UI_CONSUMED,
    TIRTC_VIDEO_UI_DIRECT_OK,
    TIRTC_VIDEO_UI_DIRECT_FAIL,
    TIRTC_VIDEO_UI_LVGL_OK,
    TIRTC_VIDEO_UI_LVGL_FAIL,
    TIRTC_VIDEO_UI_EVENT_COUNT
} tirtc_video_ui_event_t;
/* GUI mailbox/transfer boundaries only. The generation travels with pixels. */
void tirtc_video_note_ui(uint32_t generation, tirtc_video_ui_event_t event);
#endif
/* Stage wall times for the opt-in local JPEG/UI benchmark. Each named phase
 * is a subset of total_ms and can include RTOS preemption while in that phase. */
typedef struct {
    uint32_t total_ms, geometry_ms, info_ms, rows_ms, copy_ms, publish_ms;
    uint32_t slices, width, height;
} tirtc_video_local_decode_stats_t;
#if TIRTC_VIDEO_LOCAL_UI_BENCH
/* Same-task, idle-only lease of the product decoder. Input JPEG must reside
 * in a caller-owned RAM buffer valid for the whole step. Output stays valid
 * until the next step/end. A failed step never yields pixels. */
int tirtc_video_local_decode_begin(void);
int tirtc_video_local_decode_step(uint8_t *jpeg,uint32_t length,bool publish,
                                  tirtc_video_local_decode_stats_t *stats);
const uint16_t *tirtc_video_local_decode_pixels(void);
void tirtc_video_local_decode_end(void);
#endif
/* Exact, complete /v1/device/profile payload: MJPEG, never simulated H.264. */
const char *tirtc_video_profile_json(void);
#endif
