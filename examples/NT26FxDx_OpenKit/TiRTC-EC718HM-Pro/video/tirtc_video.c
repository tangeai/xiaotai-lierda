/* V04 GC032A -> bounded MJPEG -> TiRTC, and MJPEG -> RGB565 display.
 * Original sensor/codec SDK is unchanged. Network callbacks only copy a frame;
 * a camera producer overlaps DMA with one codec worker, with no UI-thread I/O. */
#include "tirtc_video.h"
#if TIRTC_F9_CALL_AUDIO_QOS
#include "../calls/calls_audio.h"
#endif
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
#include "../video_isolation/jpeg_codec.h"
#include "../video_isolation/video_iso_call.h"
#if !TIRTC_VIDEO_VPU_ISOLATION
#error "Armed fixture calls require passive VPU ownership tracing"
#endif
#endif
#if TIRTC_VIDEO_TASK_RUNTIME_DIAGNOSTICS
#include "../port/tirtc_task_runtime.h"
#endif
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
#include "../port/tirtc_video_perf.h"
#endif
#include "../platform/device_metadata.h"
#include "tirtc_camera_fence.h"
#include "tirtc_capture_pipeline.h"
#include "tirtc_video_format.h"
#include "tirtc_video_vpu_diag.h"
#include "tirtc_video_rx_sched.h"
#include "tirtc_rx_lcd_metrics.h"
#include "tirtc_jpeg_stage.h"
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
#include "tirtc_jpeg_sched.h"
#include "tirtc_jpeg_tasks.h"
#endif
#include "tirtc_ui.h"
#include "liot_camera.h"
#include "liot_gpio2.h"
#include "liot_os.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include "liot_power.h"
#include "mm_jpeg_if.h"
#include <string.h>
#include <stdio.h>

#define CAP_W TIRTC_VIDEO_CAP_W
#define CAP_H TIRTC_VIDEO_CAP_H
#define ENC_W TIRTC_VIDEO_ENC_W
#define ENC_H TIRTC_VIDEO_ENC_H
#define JPEG_MAX (64U*1024U)
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
/* Tolerate one frame period plus half a period of codec contention; only
 * skip an old frame at dequeue when a newer frame can be displayed. */
#define RX_PERIOD_MS ((1000U+TIRTC_VIDEO_DUPLEX_FPS-1U)/TIRTC_VIDEO_DUPLEX_FPS)
#define RX_OLD_MAX_MS ((1500U+TIRTC_VIDEO_DUPLEX_FPS-1U)/TIRTC_VIDEO_DUPLEX_FPS)
#define RX_QUEUE_SLOTS 3U
#else
#define RX_QUEUE_SLOTS 2U
#endif
#define RX_MAX_W 640U
#define RX_MAX_H 640U
#define RX_MAX_PIXELS (640U*480U)
#define OUT_W 192U
#define OUT_H 144U
#if TIRTC_F9_WX_SQUARE_VIDEO
#define DECODE_MAP_W TIRTC_F9_WX_SQUARE_SIZE
#define DECODE_MAP_H TIRTC_F9_WX_SQUARE_SIZE
#else
#define DECODE_MAP_W OUT_W
#define DECODE_MAP_H OUT_H
#endif
#if TIRTC_VIDEO_DIRECT_VIEW_ENABLE
#define DIRECT_W 256U
#define DIRECT_H 240U
#define OUTPUT_CAPACITY_BYTES (DIRECT_W*DIRECT_H*2U)
#elif TIRTC_F9_WX_SQUARE_VIDEO
#define OUTPUT_CAPACITY_BYTES (TIRTC_F9_WX_SQUARE_SIZE*TIRTC_F9_WX_SQUARE_SIZE*2U)
#else
#define OUTPUT_CAPACITY_BYTES OUT_BYTES
#endif
#if TIRTC_F9_WX_SQUARE_VIDEO
#define VOIP_SCREEN_WIDTH TIRTC_F9_WX_SQUARE_SIZE_STR
#define VOIP_SCREEN_HEIGHT TIRTC_F9_WX_SQUARE_SIZE_STR
#define VOIP_ASPECT_RATIO "1"
#elif TIRTC_BOARD_NT26F9D0
#define VOIP_SCREEN_WIDTH "320"
#define VOIP_SCREEN_HEIGHT "240"
#define VOIP_ASPECT_RATIO "1.3333333333333333"
#else
#define VOIP_SCREEN_WIDTH "144"
#define VOIP_SCREEN_HEIGHT "192"
#define VOIP_ASPECT_RATIO "1.3333333333333333"
#endif
#define RAW_BYTES (CAP_W*CAP_H*2U)
#define LINE_BYTES (RX_MAX_W*16U*2U)
#define OUT_BYTES (OUT_W*OUT_H*2U)
#define VIDEO_BYTES (RAW_BYTES*2U+JPEG_MAX*3U+LINE_BYTES+OUTPUT_CAPACITY_BYTES)
#define LIVE_VIDEO_BYTES (RAW_BYTES*2U+JPEG_MAX)
/* Keep headroom for incoming frame assembly and audio/control allocations.
 * These are admission samples, not a reservation against every SDK task. */
#define HEAP_PAUSE_BYTES (192U*1024U)
#define HEAP_RESUME_BYTES (256U*1024U)
#define BLOCK_PAUSE_BYTES (32U*1024U)
#define BLOCK_RESUME_BYTES (64U*1024U)
#define SEND_QUEUE_TARGET (48U*1024U)
#define VIDEO_ERROR_MEMORY (-1701)
#define VIDEO_ERROR_CAMERA (-1702)
#define VIDEO_ERROR_CODEC (-1703)
#define VIDEO_ERROR_FRAME (-1704)
#define VIDEO_ERROR_CLOSE (-1705)
LIOT_ADD_CAMERA(liot_gc032a_2ddr);

static liot_task_t s_task;
static liot_mutex_t s_rx_lock;
static bool s_starting, s_wanted, s_wechat, s_enabled=true;
static bool s_incoming, s_rx_clockwise;
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
static bool s_test_fixture;
static unsigned s_test_flags, s_fixture_jpeg_length;
static uint8_t *s_fixture_pixels;
_Static_assert(ENC_W == 320U && ENC_H == 240U && TIRTC_VIDEO_JPEG_QUALITY == 50U,
               "Live isolation requires the frozen QVGA quality setting");
#endif

static bool uses_camera(void)
{
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    return !s_test_fixture || (s_test_flags & ISO_CALL_CAMERA);
#else
    return true;
#endif
}

static bool decode_video(void)
{
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    return !(s_test_flags & (ISO_CALL_NO_RX | ISO_CALL_NO_DECODE));
#else
    return true;
#endif
}

static bool publish_video(void)
{
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    return !(s_test_flags & (ISO_CALL_NO_RX | ISO_CALL_NO_LCD));
#else
    return true;
#endif
}

static void sample_camera_pipeline(tirtc_capture_pipeline_stats_t *pipeline)
{
    if (uses_camera()) tirtc_capture_pipeline_get_stats(pipeline);
    else memset(pipeline,0,sizeof(*pipeline));
}
static demo_tirtc_owner_e s_owner;
static uint32_t s_generation;
static tirtc_video_snapshot_t s_status;
/* The worker owns these pointers. The callback may access rx slots only with
 * rx_lock held while s_accept is true. Teardown first closes that admission. */
static uint8_t *s_memory, *s_raw, *s_raw_next, *s_jpeg, *s_rx[RX_QUEUE_SLOTS];
static uint16_t *s_line, *s_output;
static int s_pending=-1, s_decoding=-1;
static uint32_t s_rx_len[RX_QUEUE_SLOTS];
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
/* With an optional third JPEG buffer, s_pending is the older waiting slot.
 * The current decoder lease remains separate and cannot be evicted. */
static int s_pending_newer=-1;
static uint32_t s_rx_arrival[RX_QUEUE_SLOTS];
#endif
static bool s_accept;
static bool s_memory_paused; /* Video worker only. */
static liot_camera_handle_t s_camera;
static void *s_encoder, *s_decoder;
#if TIRTC_VIDEO_LOCAL_UI_BENCH
_Static_assert(ENC_W==320U && DIRECT_W==256U && DIRECT_H==240U,
               "Local benchmark uses the product QVGA direct viewport");
static liot_task_t s_local_decoder_task;
static bool s_local_pixels_valid;
#endif
/* Per-call counters separate network delivery from local decode/drop work.
 * Snapshot only every five seconds; never print from media callbacks. */
typedef struct {
    uint32_t incoming, incoming_bytes, oversized, format_drop, lock_drop;
    uint32_t replaced, decode_drop, memory_drop, send_zero, send_error;
    uint32_t send_ms, width, height, input_jpeg, out_width, out_height;
    uint32_t crop_x, crop_y, crop_width, crop_height;
    uint32_t tx_bytes, tx_not_accepted, encode_defer;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE && TIRTC_VIDEO12_DIAGNOSTICS
    uint32_t rxq_slots, rxq_peak, rxq_overflow, rxq_stale;
    uint32_t rxq_wait_max, rxq_stop_pending;
#endif
} video_diag_t;
static video_diag_t s_diag;

#if TIRTC_CALL_MEDIA_DIAGNOSTICS
#define MEDIA_REPORT_MS 10000U
typedef struct {
    uint32_t started, last_report;
    uint32_t forwarded, bad_arg, stream_drop, admission_closed, stop_pending;
    uint32_t rx_open_ms, rx_ready_ms, warm_queued, queue_age_drop;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    uint32_t queue_takes, wait_sum_ms, wait_peak_ms, kept_over_period, rx_first;
    uint32_t age_drop_peak_ms, full_drop_peak_ms, gui_wait_polls, arrived_during_tx;
    bool worker_encoding;
#endif
#if TIRTC_F9_CALL_AUDIO_QOS
    uint32_t audio_hold_polls, audio_hold_late_frames;
#endif
    uint32_t decoded, decode_jobs, decode_cancelled, decode_ms, decode_peak_ms;
    uint32_t published, publish_failed, publish_ms;
    uint32_t ui[TIRTC_VIDEO_UI_EVENT_COUNT];
    uint32_t encoded, encode_failed, encode_ms, encode_peak_ms;
    uint32_t tx_gate_hits;
    size_t tx_queue_peak;
    bool closed;
} video_media_diag_t;
static video_media_diag_t s_media_diag;

/* A delayed SDK/GUI event from the previous generation cannot affect the
 * next call. All shared counter changes run inside the RTOS critical section. */
static bool media_diag_current(uint32_t generation)
{
    return generation && s_status.active && s_generation==generation &&
           s_owner!=DEMO_TIRTC_OWNER_LIVE && !s_media_diag.closed;
}
void tirtc_video_note_ui(uint32_t generation,tirtc_video_ui_event_t event)
{
    if (!generation) return;
    liot_rtos_enter_critical();
    if (media_diag_current(generation)) ++s_media_diag.ui[event];
    liot_rtos_exit_critical();
}
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
/* Preserve accepted frame timestamps even when the codec stalls several seconds. */
#define VIDEO12_TRACE_CAPACITY 1024U
#else
#define VIDEO12_TRACE_CAPACITY 32U
#endif
#define VIDEO12_TRACE_BATCH 16U
typedef struct {
    uint32_t timestamps[VIDEO12_TRACE_CAPACITY];
    unsigned head, count;
    uint32_t skipped;
} video12_frame_trace_t;
static video12_frame_trace_t s_trace[2];
static uint32_t s_receive_callbacks;
#if TIRTC_VIDEO_LOCAL_UI_BENCH
static tirtc_video_local_decode_stats_t s_live_decode_phases;
static uint32_t s_live_decode_frames;
#endif
typedef struct {
    uint32_t count, sum, max;
} video12_span_t;
static struct {
    video12_span_t encode, decode;
    uint32_t encode_queue_ms, camera_selected_ms;
} s_video12_timing;

static void video12_span_add(video12_span_t *span, uint32_t elapsed)
{
    ++span->count;
    span->sum+=elapsed;
    if (elapsed>span->max) span->max=elapsed;
}

/* Store only sender timestamps in the RTOS critical section. The worker
 * drains bounded chunks and performs all formatting outside the callback. */
static void video12_trace_offer(unsigned direction, uint32_t timestamp)
{
    video12_frame_trace_t *trace=&s_trace[direction];
    if (trace->count==VIDEO12_TRACE_CAPACITY) {
        ++trace->skipped;
        return;
    }
    unsigned tail=(trace->head+trace->count)%VIDEO12_TRACE_CAPACITY;
    trace->timestamps[tail]=timestamp;
    ++trace->count;
}

static void video12_trace_drain(uint32_t generation, bool final)
{
    for (unsigned dir=0; dir<2U; ++dir) {
        video12_frame_trace_t *trace=&s_trace[dir];
        for (;;) {
            unsigned count;
            uint32_t values[VIDEO12_TRACE_BATCH];
            liot_rtos_enter_critical();
            count=trace->count;
            if ((!final && count<VIDEO12_TRACE_BATCH) || !count) {
                liot_rtos_exit_critical();
                break;
            }
            if (count>VIDEO12_TRACE_BATCH) count=VIDEO12_TRACE_BATCH;
            for (unsigned i=0; i<count; ++i)
                values[i]=trace->timestamps[(trace->head+i)%VIDEO12_TRACE_CAPACITY];
            trace->head=(trace->head+count)%VIDEO12_TRACE_CAPACITY;
            trace->count-=count;
            liot_rtos_exit_critical();
            char line[VIDEO12_TRACE_BATCH*11U+1U];
            unsigned length=0;
            for (unsigned i=0; i<count; ++i) {
                int size=snprintf(line+length,sizeof(line)-length,
                                  i?",%lu":"%lu",(unsigned long)values[i]);
                if (size<0 || (unsigned)size>=sizeof(line)-length) {
                    liot_rtos_enter_critical(); trace->skipped+=count; liot_rtos_exit_critical();
                    length=0;
                    break;
                }
                length+=(unsigned)size;
            }
            if (length) liot_trace("[V12-FRAME] gen=%lu dir=%u s=%s\r\n",
                                   (unsigned long)generation,dir+1U,line);
            if (!final) break;
        }
    }
    if (final) liot_trace("[V12-TRACE] gen=%lu tx_missing=%lu rx_missing=%lu\r\n",
                          (unsigned long)generation,(unsigned long)s_trace[0].skipped,
                          (unsigned long)s_trace[1].skipped);
}
#endif

static uint32_t now_ms(void) { return liot_rtos_get_running_time(); }
static bool duplex_12fps(demo_tirtc_owner_e owner)
{
#if TIRTC_BOARD_NT26F9D0
    return owner == DEMO_TIRTC_OWNER_WECHAT || owner == DEMO_TIRTC_OWNER_DEV_CHAT;
#else
    (void)owner;
    return false;
#endif
}
static unsigned call_jpeg_quality(void)
{
#if TIRTC_F9_AV_FOCUS
    if (duplex_12fps(s_owner)) return 30U;
#endif
    return TIRTC_VIDEO_JPEG_QUALITY;
}

static bool wx_square_video(void)
{
#if TIRTC_F9_WX_SQUARE_VIDEO
    return s_wechat && s_owner == DEMO_TIRTC_OWNER_WECHAT;
#else
    return false;
#endif
}

static unsigned call_tx_fps(bool duplex)
{
    if (wx_square_video()) return TIRTC_F9_WX_TX_FPS;
    return duplex ? TIRTC_VIDEO_DUPLEX_FPS : TIRTC_VIDEO_TARGET_FPS;
}
static bool memory_admit(void)
{
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    tirtc_video_perf_scope_t heap_scope=tirtc_video_perf_begin(TIRTC_VPERF_HEAP);
#endif
    size_t free_bytes=liot_xPortGetFreeHeapSize();
    size_t largest=liot_xPortGetMaximumFreeBlockSize();
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    tirtc_video_perf_end(heap_scope);
#endif
    if (s_memory_paused) {
        if (free_bytes>=HEAP_RESUME_BYTES && largest>=BLOCK_RESUME_BYTES) s_memory_paused=false;
    } else if (free_bytes<HEAP_PAUSE_BYTES || largest<BLOCK_PAUSE_BYTES) s_memory_paused=true;
    return !s_memory_paused;
}
static bool requested(uint32_t generation)
{
    bool wanted;
    liot_rtos_enter_critical(); wanted=s_wanted && s_generation==generation; liot_rtos_exit_critical();
    return wanted;
}
/* Caller holds s_rx_lock. This transfers exactly one slot to the worker. */
static int take_pending_locked(void)
{
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    if (s_rx[2] && s_pending>=0) {
        const uint32_t tick=now_ms();
        const uint32_t oldest_ms=tick-s_rx_arrival[s_pending];
        if (s_pending_newer>=0 && oldest_ms>=RX_OLD_MAX_MS) {
            s_pending=s_pending_newer;
            s_pending_newer=-1;
            liot_rtos_enter_critical();
            ++s_status.dropped_frames;
            ++s_diag.replaced;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
            ++s_media_diag.queue_age_drop;
            if (oldest_ms>s_media_diag.age_drop_peak_ms) s_media_diag.age_drop_peak_ms=oldest_ms;
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
            ++s_diag.rxq_stale;
#endif
            liot_rtos_exit_critical();
        }
#if TIRTC_VIDEO12_DIAGNOSTICS || TIRTC_CALL_MEDIA_DIAGNOSTICS
        const uint32_t waited=tick-s_rx_arrival[s_pending];
        liot_rtos_enter_critical();
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        ++s_media_diag.queue_takes;
        s_media_diag.wait_sum_ms+=waited;
        if (waited>s_media_diag.wait_peak_ms) s_media_diag.wait_peak_ms=waited;
        if (s_pending_newer>=0 && waited>=RX_PERIOD_MS) ++s_media_diag.kept_over_period;
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
        if (waited>s_diag.rxq_wait_max) s_diag.rxq_wait_max=waited;
#endif
        liot_rtos_exit_critical();
#endif
        s_decoding=s_pending;
        s_pending=s_pending_newer;
        s_pending_newer=-1;
        return s_decoding;
    }
#endif
    s_decoding=s_pending;
    s_pending=-1;
    return s_decoding;
}
#if TIRTC_VIDEO_GUI_BACKPRESSURE_ENABLE
static int take_pending_for_gui(void)
{
    int slot;
    /* The GUI mutex is released before acquiring the RX mutex. Keep the
     * newest compressed JPEG pending until the previous RGB was consumed. */
    if (publish_video() && tirtc_ui_video_frame_waiting()) {
#if TIRTC_CALL_MEDIA_DIAGNOSTICS && TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
        ++s_media_diag.gui_wait_polls;
#endif
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
        tirtc_video_perf_note_gui_wait();
#endif
        return -1;
    }
    (void)liot_rtos_mutex_lock(s_rx_lock,LIOT_WAIT_FOREVER);
    slot=take_pending_locked();
    (void)liot_rtos_mutex_unlock(s_rx_lock);
    return slot;
}
#endif
#if TIRTC_F9_AV_FOCUS && TIRTC_VIDEO_GUI_BACKPRESSURE_ENABLE && TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
/* Only the worker dequeues frames. The callback can add or replace a waiting
 * frame after this advisory snapshot; take_pending_for_gui leases it under
 * the RX mutex. If the TX deadline is already late, retain TX-first service. */
static int take_backlog_before_tx(uint32_t clock,const tirtc_video_rate_t *rate)
{
    if ((int32_t)(clock-rate->next)<0 || clock-rate->next>=RX_PERIOD_MS/2U) return -1;
    liot_rtos_enter_critical();
    const bool two_waiting=s_pending_newer>=0;
    liot_rtos_exit_critical();
    return two_waiting ? take_pending_for_gui() : -1;
}
#endif
static void set_error(int error)
{
    liot_rtos_enter_critical(); s_status.error=error; liot_rtos_exit_critical();
}
void tirtc_video_get_snapshot(tirtc_video_snapshot_t *out)
{
    if (!out) return;
    liot_rtos_enter_critical(); *out=s_status; liot_rtos_exit_critical();
}
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
void tirtc_video_get_test_acceptance_snapshot(tirtc_video_snapshot_t *out)
{
    if (!out) return;
    liot_rtos_enter_critical();
    *out=s_status;
    if (s_test_flags & ISO_CALL_NO_DECODE) out->rx_frames=s_diag.incoming;
    liot_rtos_exit_critical();
}
#endif
const char *tirtc_video_profile_json(void)
{
    return "{\"hardware\":{\"chip_model\":\"" TIRTC_DEVICE_CHIP_MODEL
      "\",\"board_model\":\"" TIRTC_DEVICE_BOARD_MODEL "\"},"
      "\"firmware_version\":\"" TIRTC_DEVICE_FIRMWARE_VERSION "\","
#if TIRTC_DEV_OPUS_ENABLE
      "\"profiles\":{\"call\":{\"up_audio_mt\":[\"opus\"],\"down_audio_mt\":[\"opus\",\"alaw\"],"
      "\"up_video_mt\":[\"mjpeg\"],\"down_video_mt\":[\"mjpeg\"],\"audio_rate\":16000,\"audio_channels\":1,"
#else
      "\"profiles\":{\"call\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
      "\"up_video_mt\":[\"mjpeg\"],\"down_video_mt\":[\"mjpeg\"],\"audio_rate\":8000,\"audio_channels\":1,"
#endif
      "\"camera_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,\"no_video\":false,\"aspect_ratio\":1.3333333333333333,\"object_fit\":\"contain\"},"
      "\"stream\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
      "\"up_audio_streamid\":10,\"up_video_streamid\":11,\"down_audio_streamid\":14,"
      "\"up_video_mt\":[\"mjpeg\"],\"down_video_mt\":[],\"audio_rate\":8000,\"audio_channels\":1,"
      "\"camera_rotation\":90,\"hor_mirror\":false,\"vert_mirror\":false,\"no_video\":false,\"aspect_ratio\":1.3333333333333333,\"object_fit\":\"contain\"},"
      "\"voip\":{\"screen_width\":" VOIP_SCREEN_WIDTH ",\"screen_height\":" VOIP_SCREEN_HEIGHT ",\"up_video_mt\":\"mjpeg\",\"down_video_mt\":\"mjpeg\","
      "\"down_audio_mt\":\"alaw\",\"audio_rate\":8000,\"audio_channels\":1,\"camera_rotation\":0,\"down_video_rotation\":1,"
      "\"hor_mirror\":false,\"vert_mirror\":false,\"no_video\":false,\"aspect_ratio\":" VOIP_ASPECT_RATIO ",\"object_fit\":\"contain\","
      "\"video_res_mode\":\"fill_screen\",\"calling_timeout_sec\":45}}}";
}
static int start_session(demo_tirtc_owner_e owner,uint32_t generation,bool wechat,
                         bool incoming,bool enabled,bool test_fixture,unsigned test_flags)
{
#if !TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    (void)test_fixture; (void)test_flags;
#endif
    int result=0;
    if (!generation) return -2;
    liot_rtos_enter_critical();
    if (!s_task || !s_rx_lock) result=-1;
#if TIRTC_VIDEO_LOCAL_UI_BENCH
    else if (s_local_decoder_task) result=-3;
#endif
    else if (s_wanted || s_status.active)
        result=s_wanted && s_generation==generation && s_owner==owner &&
               s_wechat==wechat && s_incoming==incoming
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
               && s_test_fixture==test_fixture && s_test_flags==test_flags
#endif
               ? 0 : -3;
    else {
        memset(&s_status,0,sizeof(s_status));
        memset(&s_diag,0,sizeof(s_diag));
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        memset(&s_media_diag,0,sizeof(s_media_diag));
        s_media_diag.last_report=s_media_diag.started=now_ms();
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
        memset(s_trace,0,sizeof(s_trace));
        s_receive_callbacks=0;
#if TIRTC_VIDEO_LOCAL_UI_BENCH
        memset(&s_live_decode_phases,0,sizeof(s_live_decode_phases));
        s_live_decode_frames=0;
#endif
        memset(&s_video12_timing,0,sizeof(s_video12_timing));
#endif
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
        s_test_fixture=test_fixture; s_test_flags=test_flags;
#endif
        s_owner=owner; s_generation=generation; s_wechat=wechat; s_enabled=enabled; s_wanted=true;
        /* The deployed WX incoming flow is upright; outgoing retains its
         * verified CW90 compatibility transform. Do not infer orientation
         * from JPEG dimensions or unused SDK frame-header fields. */
        s_incoming=incoming; s_rx_clockwise=wechat && !incoming;
        s_status.active=true; s_status.generation=generation; s_status.camera_enabled=enabled;
    }
    liot_rtos_exit_critical(); return result;
}
int tirtc_video_start(demo_tirtc_owner_e owner,uint32_t generation,bool wechat,bool incoming)
{
    if (owner!=DEMO_TIRTC_OWNER_WECHAT && owner!=DEMO_TIRTC_OWNER_DEV_CHAT) return -2;
    return start_session(owner,generation,wechat,incoming,true,false,0U);
}
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
int tirtc_video_start_test_fixture(uint32_t generation,unsigned flags)
{
    return start_session(DEMO_TIRTC_OWNER_DEV_CHAT,generation,false,true,
                         !(flags & ISO_CALL_NO_TX),true,flags);
}
#endif
int tirtc_video_start_live(uint32_t generation,bool enabled)
{
    return start_session(DEMO_TIRTC_OWNER_LIVE,generation,false,false,enabled,false,0U);
}
void tirtc_video_stop(uint32_t generation)
{
    liot_rtos_enter_critical();
    bool stop_camera=s_generation==generation && uses_camera();
    if (s_generation==generation) s_wanted=false;
    liot_rtos_exit_critical();
    if (stop_camera) tirtc_capture_pipeline_request_stop(generation);
}
bool tirtc_video_is_stopped(uint32_t generation)
{
    bool stopped;
    liot_rtos_enter_critical(); stopped=s_generation!=generation || (!s_wanted && !s_status.active); liot_rtos_exit_critical();
    return stopped;
}
int tirtc_video_set_enabled(uint32_t generation,bool enabled)
{
    int result=0;
    liot_rtos_enter_critical();
    if (!s_wanted || s_generation!=generation) result=-1;
    else { s_enabled=enabled; s_status.camera_enabled=enabled; }
    liot_rtos_exit_critical();
    if (!result && uses_camera()) (void)tirtc_capture_pipeline_set_enabled(generation,enabled);
    return result;
}
void tirtc_video_receive(uint32_t generation,const TIRTCFRAMEINFO *frame,const void *data)
{
    const bool valid_args=frame && data && s_rx_lock;
    if (!valid_args) {
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        liot_rtos_enter_critical();
        if (media_diag_current(generation)) {
            ++s_media_diag.forwarded;
            ++s_media_diag.bad_arg;
        }
        liot_rtos_exit_critical();
#endif
        return;
    }
    liot_rtos_enter_critical();
    bool current=s_wanted && s_generation==generation && s_owner!=DEMO_TIRTC_OWNER_LIVE;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    if (media_diag_current(generation)) {
        ++s_media_diag.forwarded;
        if (!current) ++s_media_diag.admission_closed;
    }
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
    if (current && s_status.ready && duplex_12fps(s_owner) &&
        frame->stream_id==(s_wechat?1U:11U)) {
        ++s_receive_callbacks;
        video12_trace_offer(1U,frame->ts);
    }
#endif
    liot_rtos_exit_critical();
    if (!current) return;
    if (frame->media!=TIRTC_VIDEO_JPEG || frame->length<4 || frame->length>JPEG_MAX) {
        liot_rtos_enter_critical(); s_status.dropped_frames++;
        if (frame->length>JPEG_MAX) s_diag.oversized++; else s_diag.format_drop++;
        liot_rtos_exit_critical(); return;
    }
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (!decode_video()) {
        liot_rtos_enter_critical();
        if (s_wanted && s_generation==generation && s_status.ready &&
            !(s_test_flags & ISO_CALL_NO_RX) && frame->stream_id==11U) {
            ++s_diag.incoming;
            s_diag.incoming_bytes+=frame->length;
            s_diag.input_jpeg=frame->length;
        }
        liot_rtos_exit_critical();
        return;
    }
#endif
    if (liot_rtos_mutex_lock(s_rx_lock,LIOT_NO_WAIT)!=LIOT_OSI_SUCCESS) {
        liot_rtos_enter_critical(); s_status.dropped_frames++; s_diag.lock_drop++; liot_rtos_exit_critical(); return;
    }
    if (s_accept && requested(generation)) {
        if (frame->stream_id!=(s_wechat ? 1 : 11)) {
            liot_rtos_enter_critical(); s_status.dropped_frames++;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
            if (media_diag_current(generation)) ++s_media_diag.stream_drop;
#endif
            liot_rtos_exit_critical();
            (void)liot_rtos_mutex_unlock(s_rx_lock); return;
        }
        int slot=s_decoding==0 ? 1 : 0;
        bool replaced=s_pending>=0;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
        if (s_rx[2]) {
            replaced=s_pending_newer>=0;
            const uint32_t arrival=now_ms();
            if (replaced) {
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
                const uint32_t age=arrival-s_rx_arrival[s_pending];
                liot_rtos_enter_critical();
                if (age>s_media_diag.full_drop_peak_ms) s_media_diag.full_drop_peak_ms=age;
                liot_rtos_exit_critical();
#endif
                /* Reuse the oldest waiting slot while preserving the newer
                 * pending frame and the decoder's immutable slot. */
                slot=s_pending;
                s_pending=s_pending_newer;
                s_pending_newer=-1;
            } else {
                for (slot=0; slot==s_pending || slot==s_decoding; ++slot) { }
            }
            s_rx_arrival[slot]=arrival;
        }
#endif
        liot_rtos_enter_critical();
#if TIRTC_CALL_MEDIA_DIAGNOSTICS && TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
        if (s_media_diag.worker_encoding) ++s_media_diag.arrived_during_tx;
#endif
        s_diag.incoming++; s_diag.incoming_bytes+=frame->length; s_diag.input_jpeg=frame->length;
        if (replaced) { s_status.dropped_frames++; s_diag.replaced++; }
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        if (media_diag_current(generation) && !s_status.ready) ++s_media_diag.warm_queued;
#endif
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE && TIRTC_VIDEO12_DIAGNOSTICS
        const uint32_t depth=s_rx[2] && s_pending>=0 ? 2U : 1U;
        if (depth>s_diag.rxq_peak) s_diag.rxq_peak=depth;
        if (s_rx[2] && replaced) ++s_diag.rxq_overflow;
#endif
        liot_rtos_exit_critical();
        memcpy(s_rx[slot],data,frame->length);
        s_rx_len[slot]=frame->length;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
        if (s_rx[2] && s_pending>=0) s_pending_newer=slot;
        else
#endif
            s_pending=slot;
        tirtc_rx_lcd_record(generation,TIRTC_RX_LCD_RX);
    }
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    else {
        liot_rtos_enter_critical();
        if (media_diag_current(generation)) ++s_media_diag.admission_closed;
        liot_rtos_exit_critical();
    }
#endif
    (void)liot_rtos_mutex_unlock(s_rx_lock);
}
/* Worker-only ownership. A successful DMA stop is required before calling
 * this for active capture; partial-open rollback has never started DMA. */
static void buffers_close(void)
{
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (s_fixture_pixels) liot_rtos_free(s_fixture_pixels);
    s_fixture_pixels=NULL;
    s_fixture_jpeg_length=0;
#endif
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    /* The optional slot is independently allocated in both base layouts. */
    if (s_rx[2]) liot_rtos_free(s_rx[2]);
    s_rx[2]=NULL;
#endif
    if (s_memory) {
        liot_rtos_free(s_memory);
    } else {
        if (s_raw) liot_rtos_free(s_raw);
        if (s_raw_next) liot_rtos_free(s_raw_next);
        if (s_jpeg) liot_rtos_free(s_jpeg);
        if (s_rx[0]) liot_rtos_free(s_rx[0]);
        if (s_rx[1]) liot_rtos_free(s_rx[1]);
        if (s_output) liot_rtos_free(s_output);
        if (s_line) liot_rtos_free(s_line);
    }
    s_memory=s_raw=s_raw_next=s_jpeg=s_rx[0]=s_rx[1]=NULL;
    s_line=s_output=NULL;
}
static bool buffers_open(void)
{
    const bool live=s_owner==DEMO_TIRTC_OWNER_LIVE;
    const size_t bytes=live ? LIVE_VIDEO_BYTES : VIDEO_BYTES;
    size_t free_bytes=liot_xPortGetFreeHeapSize();
    size_t largest=liot_xPortGetMaximumFreeBlockSize();
    const char *part="reserve";
    if (free_bytes<bytes+HEAP_RESUME_BYTES) {
        liot_trace("[VIDEO41] buffers denied reason=reserve need=%lu free=%lu max_block=%lu\r\n",
            (unsigned long)bytes,(unsigned long)free_bytes,(unsigned long)largest);
        return false;
    }
    /* Preserve the existing layout when one large block is available. Each
     * consumer only needs its own contiguous buffer, so a fragmented heap
     * need not prevent a call when it can supply those buffers separately. */
    if (largest>=bytes+64U) s_memory=liot_rtos_malloc(bytes);
    if (s_memory) {
        s_raw=s_memory; s_raw_next=s_raw+RAW_BYTES; s_jpeg=s_raw_next+RAW_BYTES;
        s_rx[0]=s_rx[1]=NULL; s_line=s_output=NULL;
        if (!live) {
            s_rx[0]=s_jpeg+JPEG_MAX; s_rx[1]=s_rx[0]+JPEG_MAX;
            s_line=(uint16_t *)(s_rx[1]+JPEG_MAX); s_output=(uint16_t *)((uint8_t *)s_line+LINE_BYTES);
        }
        liot_trace("[VIDEO41] buffers mode=contiguous need=%lu free_before=%lu max_before=%lu\r\n",
            (unsigned long)bytes,(unsigned long)free_bytes,(unsigned long)largest);
        return true;
    }
    liot_trace("[VIDEO41] buffers try=split need=%lu free_before=%lu max_before=%lu\r\n",
        (unsigned long)bytes,(unsigned long)free_bytes,(unsigned long)largest);
    /* Same allocator and DMA domain; allocate largest first. RAW frames are
     * independently contiguous and retain the SDK allocator's alignment. */
    part="raw0"; s_raw=liot_rtos_malloc(RAW_BYTES); if (!s_raw) goto failed;
    part="raw1"; s_raw_next=liot_rtos_malloc(RAW_BYTES); if (!s_raw_next) goto failed;
    part="jpeg"; s_jpeg=liot_rtos_malloc(JPEG_MAX); if (!s_jpeg) goto failed;
    if (!live) {
        part="rx0"; s_rx[0]=liot_rtos_malloc(JPEG_MAX); if (!s_rx[0]) goto failed;
        part="rx1"; s_rx[1]=liot_rtos_malloc(JPEG_MAX); if (!s_rx[1]) goto failed;
        part="output"; s_output=liot_rtos_malloc(OUTPUT_CAPACITY_BYTES); if (!s_output) goto failed;
        part="line"; s_line=liot_rtos_malloc(LINE_BYTES); if (!s_line) goto failed;
    }
    /* Extra allocator headers and concurrent SDK allocations must not use
     * the audio/network reserve retained by the original admission rule. */
    part="reserve_after";
    if (liot_xPortGetFreeHeapSize()<HEAP_RESUME_BYTES) goto failed;
    liot_trace("[VIDEO41] buffers mode=split need=%lu free=%lu max_block=%lu\r\n",
        (unsigned long)bytes,(unsigned long)liot_xPortGetFreeHeapSize(),
        (unsigned long)liot_xPortGetMaximumFreeBlockSize());
    return true;
failed:
    liot_trace("[VIDEO41] buffers failed part=%s need=%lu free=%lu max_block=%lu\r\n",
        part,(unsigned long)bytes,(unsigned long)liot_xPortGetFreeHeapSize(),
        (unsigned long)liot_xPortGetMaximumFreeBlockSize());
    buffers_close();
    return false;
}
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
static void try_extra_receive_slot(void)
{
    if (!duplex_12fps(s_owner)) return;
    /* The normal allocation already passed. Any optional allocation failure
     * leaves the original two slots and their reserve intact. */
    if (liot_xPortGetFreeHeapSize()>=JPEG_MAX+HEAP_RESUME_BYTES+64U &&
        liot_xPortGetMaximumFreeBlockSize()>=JPEG_MAX+64U) {
        uint8_t *extra=liot_rtos_malloc(JPEG_MAX);
        if (extra) {
            if (liot_xPortGetFreeHeapSize()>=HEAP_RESUME_BYTES &&
                liot_xPortGetMaximumFreeBlockSize()>=BLOCK_RESUME_BYTES)
                s_rx[2]=extra;
            else
                liot_rtos_free(extra);
        }
    }
#if TIRTC_VIDEO12_DIAGNOSTICS
    liot_rtos_enter_critical();
    s_diag.rxq_slots=s_rx[2]?3U:2U;
    liot_rtos_exit_critical();
#endif
}
#endif
/* Only the worker opens admission, after the bounded receive buffers exist.
 * The callback holds rx_lock for the entire copy. The same lock fences
 * hardware_close before any slot can be freed, including on open failure. */
static void open_receive_admission(uint32_t generation)
{
    if (s_owner==DEMO_TIRTC_OWNER_LIVE || !decode_video()) return;
    (void)liot_rtos_mutex_lock(s_rx_lock,LIOT_WAIT_FOREVER);
    liot_rtos_enter_critical();
    if (s_wanted && s_generation==generation && !s_accept) {
        s_accept=true;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        s_media_diag.rx_open_ms=now_ms()-s_media_diag.started;
#endif
    }
    liot_rtos_exit_critical();
    (void)liot_rtos_mutex_unlock(s_rx_lock);
}
#if TIRTC_F9_WX_NATIVE_CAPTURE
#define WX240_DMA_BYTES (15U * 7680U)
#define WX240_SENSOR_SIZE (TIRTC_F9_WX_SQUARE_SIZE * (TIRTC_VIDEO_SCALE + 1U))
#define WX240_SENSOR_X ((TIRTC_VIDEO_SENSOR_W - WX240_SENSOR_SIZE) / 2U)
#define WX240_TAIL_PATTERN 0x5aU
#if TIRTC_F9_WX_SQUARE_SIZE != 240 || WX240_SENSOR_SIZE != 480U || WX240_SENSOR_X != 80U
#error WX240 native camera requires the verified VGA sensor and 2:1 decimation
#endif
#if WX240_DMA_BYTES != (TIRTC_F9_WX_SQUARE_SIZE*TIRTC_F9_WX_SQUARE_SIZE*2U) || RAW_BYTES < WX240_DMA_BYTES + 7680U
#error WX240 native path must retain at least one full spare DMA descriptor per slot
#endif
static bool s_native_wx240;
/* If the native shape fails, the next camera open restores the VGA path. */
static bool s_native_disabled;
static bool wx240_guard_ok(const uint8_t *raw, unsigned bytes)
{
    for (unsigned i=0; i<bytes; ++i)
        if (raw[WX240_DMA_BYTES+i] != WX240_TAIL_PATTERN) return false;
    return true;
}
#endif
static int hardware_open(void)
{
    liot_camera_config_t cfg;
    const bool square=wx_square_video();
    const unsigned enc_w=square?TIRTC_F9_WX_SQUARE_SIZE:ENC_W;
    const unsigned enc_h=square?TIRTC_F9_WX_SQUARE_SIZE:ENC_H;
    JPEG_ENC_PARAM enc={JPEG_COLOR_FMT_YUYV,enc_w,enc_h,call_jpeg_quality()};
    unsigned suggested=0;
    UINT8 powerup_reason=0xff;
    int powerup_result=liot_get_powerup_reason(&powerup_reason);
    liot_trace("[VIDEO40] start powerup_reason=%u ret=%d; tx=%ux%u target_fps=%u wx=%u incoming=%u rx_cw90=%u wx_req=" VOIP_SCREEN_WIDTH "x" VOIP_SCREEN_HEIGHT " ui_canvas=%ux%u\r\n",
               powerup_reason,powerup_result,enc_w,enc_h,
               call_tx_fps(duplex_12fps(s_owner)),s_wechat?1U:0U,s_incoming?1U:0U,
               s_rx_clockwise?1U:0U,square?TIRTC_F9_WX_SQUARE_SIZE:OUT_W,
               square?TIRTC_F9_WX_SQUARE_SIZE:OUT_H);
    memset(&cfg,0,sizeof(cfg));
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (s_test_fixture && !uses_camera()) {
        s_fixture_pixels=liot_rtos_malloc(ENC_W*ENC_H*2U);
        if (!s_fixture_pixels || ((uintptr_t)s_fixture_pixels & 3U)) return VIDEO_ERROR_MEMORY;
        jpeg_codec_fill_vga_test_fixture(s_fixture_pixels);
    }
#endif
    if (!buffers_open()) return VIDEO_ERROR_MEMORY;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    try_extra_receive_slot();
#endif
    s_encoder=JpegE_Create();
    s_decoder=s_owner==DEMO_TIRTC_OWNER_LIVE ? NULL : JpegD_Create();
    if (!s_encoder || (s_owner!=DEMO_TIRTC_OWNER_LIVE && !s_decoder) ||
        JpegE_SetParam(s_encoder,&enc,&suggested)<0) return VIDEO_ERROR_CODEC;
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (s_test_flags & ISO_CALL_CACHED_TX) {
        JPEG_IMAGE_BUF fixture={JPEG_COLOR_FMT_YUYV,ENC_W,ENC_H,{s_fixture_pixels,NULL,NULL}};
        unsigned length=JPEG_MAX;
        if (JpegE_Encode(s_encoder,&fixture,s_jpeg,&length)<0 || length<4 || length>JPEG_MAX)
            return VIDEO_ERROR_CODEC;
        s_fixture_jpeg_length=length;
    }
#endif
#if TIRTC_F9_AV_FOCUS
    /* F9 receive buffers and codecs are ready even while the local sensor
     * warms up. Begin admitting peer JPEGs into the existing bounded queue.
     * Decoding still starts in the ordinary worker loop after setup. */
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (!s_test_fixture)
#endif
        open_receive_admission(s_generation);
#endif
    if (!uses_camera()) return 0;
    /* GPIO25 is shared with LCD; its existing rail configuration is retained. */
    if (Liot_GpioInit(L_GPIO_27,L_IO_OUTPUT,L_IO_HIGH,NULL)!=L_GPIO_ERR_SUCCESS) return VIDEO_ERROR_CAMERA;
    cfg.sensor=&liot_gc032a_2ddr;
    cfg.cspi.num=LIOT_CSPI_PORT1; cfg.cspi.speed=LIOT_CAM_24_M;
    cfg.i2c.num=0; cfg.i2c.scl=(int8_t)255; cfg.i2c.sda=(int8_t)255;
    cfg.info.format=LIOT_CAMERA_OUTPUT_YUYV;
    cfg.info.resolution.width.size=TIRTC_VIDEO_SENSOR_W;
    cfg.info.resolution.height.size=TIRTC_VIDEO_SENSOR_H;
    cfg.info.resolution.width.offset=TIRTC_VIDEO_SENSOR_X;
    cfg.info.resolution.height.offset=TIRTC_VIDEO_SENSOR_Y;
    cfg.info.resolution.width.scale=TIRTC_VIDEO_SCALE;
    cfg.info.resolution.height.scale=TIRTC_VIDEO_SCALE;
#if TIRTC_F9_WX_NATIVE_CAPTURE
    const bool native_requested=square && !s_native_disabled;
    if (native_requested) {
        cfg.info.resolution.width.size=WX240_SENSOR_SIZE;
        cfg.info.resolution.height.size=WX240_SENSOR_SIZE;
        cfg.info.resolution.width.offset=WX240_SENSOR_X;
        cfg.info.resolution.height.offset=0U;
    }
#endif
    /* The vendor ignores its callback-init return code. Allocate our wait
     * object first, before it can enter hardware initialization. */
    if (!tirtc_camera_fence_prepare()) return VIDEO_ERROR_CAMERA;
    s_camera=Liot_CameraInit(&cfg);
    if (!s_camera || !tirtc_camera_fence_ready()) {
#if TIRTC_F9_WX_NATIVE_CAPTURE
        if (native_requested) s_native_disabled=true;
#endif
        return VIDEO_ERROR_CAMERA;
    }
#if TIRTC_F9_WX_NATIVE_CAPTURE
    if (native_requested) {
        if (!tirtc_camera_fence_set_wx240_dma()) {
            s_native_disabled=true;
            return VIDEO_ERROR_CAMERA;
        }
        memset(s_raw+WX240_DMA_BYTES,WX240_TAIL_PATTERN,RAW_BYTES-WX240_DMA_BYTES);
        memset(s_raw_next+WX240_DMA_BYTES,WX240_TAIL_PATTERN,RAW_BYTES-WX240_DMA_BYTES);
        s_native_wx240=true;
    }
#endif
#if TIRTC_F9_AV_FOCUS
    if (duplex_12fps(s_owner)) tirtc_camera_fence_enable_rearm_guard();
#endif
    liot_trace("[VIDEO40] camera GC032A window=%ux%u offset=%u,%u scale=%u raw=%ux%u JPEG=%ux%u q%u ready buffer=%lu suggestion=%u heap=%lu max_block=%lu\r\n",
        (unsigned)cfg.info.resolution.width.size,(unsigned)cfg.info.resolution.height.size,
        (unsigned)cfg.info.resolution.width.offset,(unsigned)cfg.info.resolution.height.offset,(unsigned)TIRTC_VIDEO_SCALE,
        (unsigned)cfg.info.resolution.width.size/(TIRTC_VIDEO_SCALE+1U),
        (unsigned)cfg.info.resolution.height.size/(TIRTC_VIDEO_SCALE+1U),enc_w,enc_h,
        call_jpeg_quality(),(unsigned long)(s_owner==DEMO_TIRTC_OWNER_LIVE ? LIVE_VIDEO_BYTES : VIDEO_BYTES),suggested,(unsigned long)liot_xPortGetFreeHeapSize(),
        (unsigned long)liot_xPortGetMaximumFreeBlockSize());
    return 0;
}
static bool hardware_close(void)
{
    (void)liot_rtos_mutex_lock(s_rx_lock,LIOT_WAIT_FOREVER);
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE && TIRTC_VIDEO12_DIAGNOSTICS
    liot_rtos_enter_critical();
    if (s_accept) s_diag.rxq_stop_pending=(s_pending>=0)+(s_pending_newer>=0);
    liot_rtos_exit_critical();
#endif
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    if (s_accept) {
        s_media_diag.stop_pending=(s_pending>=0);
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
        s_media_diag.stop_pending+=(s_pending_newer>=0);
#endif
    }
#endif
    s_accept=false; s_pending=s_decoding=-1;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    s_pending_newer=-1;
#endif
    (void)liot_rtos_mutex_unlock(s_rx_lock);
    if (uses_camera()) {
        tirtc_capture_pipeline_request_stop(s_generation);
        if (!tirtc_capture_pipeline_is_idle(s_generation)) return false;
        /* SDK frame-end may wake CaptureImage before DMA_END. A timed-out DMA
         * retains this allocation until its real completion, including on stop. */
        if (!tirtc_camera_quiesce(400U)) { set_error(VIDEO_ERROR_CLOSE); return false; }
        if (s_camera && Liot_CameraDeinit(s_camera)!=LIOT_CAMERA_SUCCESS) {
            set_error(VIDEO_ERROR_CLOSE); return false;
        }
    }
#if TIRTC_F9_WX_NATIVE_CAPTURE
    if (s_native_wx240) {
        const unsigned tail=RAW_BYTES-WX240_DMA_BYTES;
        const bool guard_ok=wx240_guard_ok(s_raw,tail) && wx240_guard_ok(s_raw_next,tail);
        if (!guard_ok) {
            s_native_disabled=true;
            set_error(VIDEO_ERROR_CAMERA);
        }
        s_native_wx240=false;
    }
#endif
    s_camera=NULL;
    if (s_encoder) JpegE_Destroy(s_encoder);
    if (s_decoder) JpegD_Destroy(s_decoder);
    s_encoder=s_decoder=NULL;
    buffers_close();
    if (s_owner!=DEMO_TIRTC_OWNER_LIVE) (void)tirtc_ui_publish_frame(NULL,0,0);
    return true;
}
/* Reject oversized/non-baseline headers before entering the opaque vendor
 * decoder. The SDK still validates Huffman/quantization and entropy data. */
static bool jpeg_geometry(const uint8_t *data,uint32_t length,unsigned *width,unsigned *height)
{
    uint32_t at=2; bool found=false;
    if (!data || length<4 || length>JPEG_MAX || data[0]!=0xff || data[1]!=0xd8) return false;
    while (at<length) {
        if (data[at++]!=0xff) return false;
        while (at<length && data[at]==0xff) at++;
        if (at>=length) return false;
        unsigned marker=data[at++];
        if (!marker || marker==0xd8 || marker==0xd9 || (marker>=0xd0 && marker<=0xd7)) return false;
        if (at+2U>length) return false;
        uint32_t size=((uint32_t)data[at]<<8)|data[at+1];
        if (size<2 || size>length-at) return false;
        if (marker>=0xc0 && marker<=0xcf && marker!=0xc4 && marker!=0xc8 && marker!=0xcc) {
            if (marker!=0xc0 || found || size<8 || data[at+2]!=8) return false;
            unsigned h=((unsigned)data[at+3]<<8)|data[at+4];
            unsigned w=((unsigned)data[at+5]<<8)|data[at+6];
            unsigned components=data[at+7];
            if (!w || !h || w>RX_MAX_W || h>RX_MAX_H || w*h>RX_MAX_PIXELS ||
                (components!=1 && components!=3) || size!=8U+3U*components) return false;
            *width=w; *height=h; found=true;
        }
        if (marker==0xda) return found;
        at+=size;
    }
    return false;
}
static bool decode_current(uint32_t generation)
{
#if TIRTC_VIDEO_LOCAL_UI_BENCH
    /* Generation zero is only used by the same-task local lease wrapper.
     * Keep the production per-slice check cost in the local path as well. */
    if (!generation) {
        liot_rtos_enter_critical();
        bool owned=s_local_decoder_task!=NULL;
        liot_rtos_exit_critical();
        return owned;
    }
#endif
    return requested(generation);
}

static inline uint32_t decode_stamp(tirtc_video_local_decode_stats_t *timing)
{
#if TIRTC_VIDEO_LOCAL_UI_BENCH
    if (timing) return now_ms();
#endif
    (void)timing;
    return 0;
}

static int publish_decoded_frame(uint32_t generation,uint16_t width,uint16_t height,
                                 bool publish,tirtc_video_local_decode_stats_t *timing)
{
    uint32_t began=decode_stamp(timing);
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    if (generation) ++s_media_diag.decoded;
#endif
    if (!decode_current(generation)) return -1;
    int result=0;
    if (publish) {
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        const uint32_t ui_began=now_ms();
        result=tirtc_ui_publish_call_frame(s_output,width,height,generation);
        if (generation) {
            s_media_diag.publish_ms+=now_ms()-ui_began;
            if (result) ++s_media_diag.publish_failed;
            else ++s_media_diag.published;
        }
#else
        result=tirtc_ui_publish_frame(s_output,width,height);
#endif
        if (!result) tirtc_rx_lcd_record(generation,TIRTC_RX_LCD_PUB);
    }
    if (timing) {
        timing->width=width;
        timing->height=height;
        if (publish) timing->publish_ms=decode_stamp(timing)-began;
    }
    return result;
}

#if TIRTC_JPEG_STAGE_DIAGNOSTICS
/* The local lease and the fixture video worker are mutually exclusive. */
static bool jpeg_probe_active;
static uint32_t jpeg_probe_rows, jpeg_probe_valid, jpeg_probe_wall, jpeg_probe_run;

static void jpeg_probe_begin(void)
{
    jpeg_probe_active=tirtc_jpeg_stage_begin(false);
    jpeg_probe_rows=0;
    jpeg_probe_valid=0;
    jpeg_probe_wall=0;
    jpeg_probe_run=0;
}

static void jpeg_probe_end(uint32_t generation)
{
    if (!jpeg_probe_active) return;
    jpeg_probe_active=false;
    tirtc_jpeg_stage_stats_t stats;
    if (!tirtc_jpeg_stage_end(&stats)) return;
    liot_trace("[V12-JPEG-SCHED] gen=%lu rows=%lu valid=%lu row_wall=%lu row_run=%lu span_ms=%lu coeff_n=%lu\r\n",
        (unsigned long)generation,(unsigned long)jpeg_probe_rows,(unsigned long)jpeg_probe_valid,
        (unsigned long)jpeg_probe_wall,(unsigned long)jpeg_probe_run,
        (unsigned long)(stats.end_ms-stats.begin_ms),(unsigned long)stats.coeff.count);
    for (unsigned i=0;i<2U;++i) {
        const tirtc_jpeg_stage_span_t *wait=&stats.wait[i];
        liot_trace("[V12-JPEG-WAIT] gen=%lu kind=%u n=%lu wall=%lu max=%lu valid=%lu run_wall=%lu run=%lu\r\n",
            (unsigned long)generation,i,(unsigned long)wait->count,(unsigned long)wait->wall_ms,
            (unsigned long)wait->max_ms,(unsigned long)wait->run_count,
            (unsigned long)wait->run_wall_ms,(unsigned long)wait->run_ms);
    }
}
#endif

#if TIRTC_JPEG_STAGE_DIAGNOSTICS
typedef struct {
    uint32_t start, rows, valid, row_wall, row_run, dec, accepted;
} jpeg_prio_sample_t;

static jpeg_prio_sample_t jpeg_prio_sample(void)
{
    jpeg_prio_sample_t sample={now_ms(),jpeg_probe_rows,jpeg_probe_valid,jpeg_probe_wall,jpeg_probe_run,
        s_live_decode_frames,0U};
    liot_rtos_enter_critical();
    sample.accepted=s_receive_callbacks;
    liot_rtos_exit_critical();
    return sample;
}

static void jpeg_prio_report(uint32_t generation, unsigned block, unsigned priority,
                             const jpeg_prio_sample_t *before,const jpeg_prio_sample_t *after)
{
    liot_trace("[V12-JPEG-PRIO] gen=%lu block=%u priority=%u ms=%lu rows=%lu valid=%lu row_wall=%lu row_run=%lu dec=%lu rx=%lu\r\n",
        (unsigned long)generation,block,priority,(unsigned long)(after->start-before->start),
        (unsigned long)(after->rows-before->rows),(unsigned long)(after->valid-before->valid),
        (unsigned long)(after->row_wall-before->row_wall),(unsigned long)(after->row_run-before->row_run),
        (unsigned long)(after->dec-before->dec),(unsigned long)(after->accepted-before->accepted));
}
#endif

static int decode_line(JPEG_IMAGE_BUF *row,unsigned *y)
{
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
    if (jpeg_probe_active) {
        tirtc_jpeg_sched_stamp_t before,after;
        bool valid=tirtc_jpeg_sched_stamp(&before);
        int result=JpegD_DecodeLine(s_decoder,row,y);
        ++jpeg_probe_rows;
        if (valid && tirtc_jpeg_sched_stamp(&after)) {
            const uint32_t wall=after.wall_ms-before.wall_ms;
            const uint32_t run=after.run_ms-before.run_ms;
            if (run<=wall) {
                ++jpeg_probe_valid;
                jpeg_probe_wall+=wall;
                jpeg_probe_run+=run;
            }
        }
        return result;
    }
#endif
    return JpegD_DecodeLine(s_decoder,row,y);
}

/* Decode one strip at a time into the native viewport when its geometry
 * matches. Other frames use bounded row/column maps to crop and scale. */
static int decode_present(uint32_t generation,uint8_t *jpeg,uint32_t length,
                          bool publish,tirtc_video_local_decode_stats_t *timing)
{
    /* The sole codec worker owns these maps, including on size changes. */
    static uint16_t map_x[DECODE_MAP_W], map_y[DECODE_MAP_H];
    JPEG_INFO info;
    JPEG_IMAGE_BUF row;
    const bool square=generation && wx_square_video();
    const unsigned canvas_w=square?TIRTC_F9_WX_SQUARE_SIZE:OUT_W;
    const unsigned canvas_h=square?TIRTC_F9_WX_SQUARE_SIZE:OUT_H;
    unsigned last_y=0, width=0, height=0, out_w=canvas_w, out_h=canvas_h;
    uint32_t began=decode_stamp(timing);
    bool clockwise=false,fill=false;
    if (!jpeg_geometry(jpeg,length,&width,&height)) return VIDEO_ERROR_FRAME;
    bool boost=generation && ((width<=320U && height<=240U) || (width<=240U && height<=320U));
    bool protect_audio=true;
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (s_test_fixture) {
        protect_audio=(s_test_flags & ISO_CALL_AUDIO)!=0U;
        boost=boost && !(s_test_flags & ISO_CALL_PRIO_AB);
    }
#endif
#if TIRTC_F9_AV_FOCUS
    /* The entire call worker has bounded waits at its media priority. */
    if (duplex_12fps(s_owner)) boost=false;
#endif
    if (generation) {
        liot_rtos_enter_critical();
        s_diag.width=width; s_diag.height=height;
        clockwise=s_rx_clockwise; fill=square || (s_wechat && s_incoming);
        liot_rtos_exit_critical();
    }
    if (timing) {
        timing->geometry_ms=decode_stamp(timing)-began;
        timing->width=width;
        timing->height=height;
    }
    memset(&info,0,sizeof(info));
    began=decode_stamp(timing);
    int info_result=JpegD_DecodeInfo(s_decoder,jpeg,length,&info);
    if (timing) timing->info_ms=decode_stamp(timing)-began;
    if (info_result<0) return VIDEO_ERROR_FRAME;
    if (info.uWidth!=width || info.uHeight!=height ||
        info.uEdgedWidth<width || info.uEdgedWidth>RX_MAX_W || info.uEdgedHeight<height || info.uEdgedHeight>RX_MAX_H) return VIDEO_ERROR_FRAME;
#if TIRTC_VIDEO_DIRECT_VIEW_ENABLE
    const bool native_square=square && width==canvas_w && height==canvas_h;
    const bool native_landscape=!square && width==ENC_W && height==DIRECT_H &&
        info.uEdgedWidth==width;
    if (!clockwise && (native_square || native_landscape)) {
        const unsigned view_w=native_square?canvas_w:DIRECT_W;
        const unsigned view_h=height;
        memset(&row,0,sizeof(row));
        row.eFmt=JPEG_COLOR_FMT_RGB565;
        row.uWidth=info.uEdgedWidth; row.uHeight=16; row.pData[0]=s_line;
        const unsigned left=(width-view_w)/2U;
        /* Native, MCU-aligned strips can decode into the private frame.
         * The driver writes exactly 16 rows at the buffer passed for this call. */
        const bool packed_output=native_square && info.uEdgedWidth==view_w &&
            info.uEdgedHeight==view_h && view_h%16U==0U && ((uintptr_t)s_output&3U)==0U;
        int result=0;
        for (unsigned offset=0; offset<view_h; offset+=16U) {
            unsigned y=0;
            if (!decode_current(generation)) {result=-1;break;}
            tirtc_video_rx_priority_begin(s_task,boost);
            began=decode_stamp(timing);
            if (packed_output) row.pData[0]=s_output+offset*view_w;
            const int rc=decode_line(&row,&y);
            if (timing) { timing->rows_ms+=decode_stamp(timing)-began; ++timing->slices; }
            if (rc<0 || y!=offset) {result=VIDEO_ERROR_FRAME;break;}
            began=decode_stamp(timing);
            if (!packed_output) {
                const unsigned rows=view_h-y<16U?view_h-y:16U;
                for (unsigned r=0; r<rows; ++r)
                    memcpy(s_output+(y+r)*view_w,s_line+r*row.uWidth+left,
                           view_w*sizeof(*s_output));
            }
            if (timing) timing->copy_ms+=decode_stamp(timing)-began;
#if TIRTC_F9_AV_FOCUS
            if (generation && (offset/16U & 3U)==3U) liot_rtos_task_sleep_ms(1U);
#endif
            if (!tirtc_video_rx_pause(s_task,offset/16U+1U,protect_audio)) {
                result=VIDEO_ERROR_FRAME;
                break;
            }
        }
        if (!tirtc_video_rx_priority_end(s_task)) return VIDEO_ERROR_FRAME;
        if (result) return result;
        if (generation) {
            liot_rtos_enter_critical();
            s_diag.out_width=view_w; s_diag.out_height=view_h;
            s_diag.crop_x=left; s_diag.crop_y=0; s_diag.crop_width=view_w; s_diag.crop_height=view_h;
            liot_rtos_exit_critical();
        }
        return publish_decoded_frame(generation,view_w,view_h,publish,timing);
    }
#endif
    /* The local experiment deliberately matches only the exact direct-view
     * QVGA format. Never pass a different output shape to its UI-only run. */
    if (!generation) return VIDEO_ERROR_FRAME;
    unsigned display_w=clockwise?height:width, display_h=clockwise?width:height;
    unsigned crop_w=width, crop_h=height, crop_x=0, crop_y=0;
    if (fill) {
        /* Crop in the source coordinate space. Validated JPEG dimensions
         * cap these products at 640*192; no full-frame resize is needed.
         * Keep at least one source pixel for extreme valid aspect ratios. */
        if (width*canvas_h>height*canvas_w) {
            crop_w=height*canvas_w/canvas_h;
            if (!crop_w) crop_w=1U;
        } else {
            crop_h=width*canvas_h/canvas_w;
            if (!crop_h) crop_h=1U;
        }
        crop_x=(width-crop_w)/2U; crop_y=(height-crop_h)/2U;
    } else {
        if (display_w*canvas_h>display_h*canvas_w) out_h=display_h*canvas_w/display_w;
        else out_w=display_w*canvas_h/display_h;
    }
    if (!out_w || !out_h) return VIDEO_ERROR_FRAME;
    liot_rtos_enter_critical();
    s_diag.out_width=out_w; s_diag.out_height=out_h;
    s_diag.crop_x=crop_x; s_diag.crop_y=crop_y;
    s_diag.crop_width=crop_w; s_diag.crop_height=crop_h;
    liot_rtos_exit_critical();
    const unsigned pad_x=(canvas_w-out_w)/2U, pad_y=(canvas_h-out_h)/2U;
    if (clockwise) {
        /* Output x selects a reversed source row; output y a source column. */
        for (unsigned dx=0; dx<out_w; dx++)
            map_x[dx]=(uint16_t)(crop_y+crop_h-1U-dx*crop_h/out_w);
        for (unsigned dy=0; dy<out_h; dy++)
            map_y[dy]=(uint16_t)(crop_x+dy*crop_w/out_h);
    } else {
        for (unsigned dx=0; dx<out_w; dx++) map_x[dx]=(uint16_t)(crop_x+dx*crop_w/out_w);
        for (unsigned dy=0; dy<out_h; dy++) map_y[dy]=(uint16_t)(crop_y+dy*crop_h/out_h);
    }
    if (out_w!=canvas_w || out_h!=canvas_h)
        memset(s_output,0,canvas_w*canvas_h*sizeof(*s_output));
    memset(&row,0,sizeof(row));
    row.eFmt=JPEG_COLOR_FMT_RGB565; row.uWidth=info.uEdgedWidth; row.uHeight=16; row.pData[0]=s_line;
    unsigned next_row=0, next_col=out_w;
    int decode_result=0;
    for (unsigned i=0; i<(height+15U)/16U; i++) {
        unsigned y=0;
        if (!decode_current(generation)) {decode_result=-1;break;}
        tirtc_video_rx_priority_begin(s_task,boost);
        began=decode_stamp(timing);
        int result=decode_line(&row,&y);
        if (timing) { timing->rows_ms+=decode_stamp(timing)-began; ++timing->slices; }
        if (result<0 || y>=height || (i && y<=last_y)) {decode_result=VIDEO_ERROR_FRAME;break;}
        began=decode_stamp(timing);
        last_y=y;
        unsigned end=y+16U<height ? y+16U : height;
        if (clockwise) {
            /* A source row becomes one output column. Only columns whose
             * source row is present in this MCU slice touch its pixels. */
            while (next_col && map_x[next_col-1U]<y) --next_col;
            while (next_col && map_x[next_col-1U]<end) {
                unsigned dx=--next_col, sy=map_x[dx];
                const uint16_t *src=s_line+(sy-y)*row.uWidth;
                uint16_t *dst=s_output+pad_y*canvas_w+pad_x+dx;
                for (unsigned dy=0; dy<out_h; dy++) dst[dy*canvas_w]=src[map_y[dy]];
            }
        } else {
            while (next_row<out_h && map_y[next_row]<y) ++next_row;
            while (next_row<out_h && map_y[next_row]<end) {
                unsigned dy=next_row++, sy=map_y[dy];
                uint16_t *dst=s_output+(dy+pad_y)*canvas_w+pad_x;
                const uint16_t *src=s_line+(sy-y)*row.uWidth;
                for (unsigned dx=0; dx<out_w; dx++) dst[dx]=src[map_x[dx]];
            }
        }
        if (timing) timing->copy_ms+=decode_stamp(timing)-began;
#if TIRTC_VIDEO_RX_PRIORITY
        if (tirtc_video_rx_priority_active) {
            if (!tirtc_video_rx_pause(s_task,i+1U,protect_audio)) {
                decode_result=VIDEO_ERROR_FRAME;
                break;
            }
        } else
#endif
        if ((i&3U)==3U && (!duplex_12fps(s_owner) || TIRTC_F9_AV_FOCUS))
            liot_rtos_task_sleep_ms(1);
    }
    if (!tirtc_video_rx_priority_end(s_task)) return VIDEO_ERROR_FRAME;
    if (decode_result) return decode_result;
    if (clockwise?next_col!=0U:next_row!=out_h) return VIDEO_ERROR_FRAME;
    return publish_decoded_frame(generation,canvas_w,canvas_h,publish,timing);
}

#if TIRTC_VIDEO_LOCAL_UI_BENCH
static bool local_decoder_owned(void)
{
    liot_task_t self=NULL;
    return liot_rtos_task_get_current_ref(&self)==LIOT_OSI_SUCCESS &&
           self && self==s_local_decoder_task;
}

int tirtc_video_local_decode_begin(void)
{
    liot_task_t self=NULL;
    if (liot_rtos_task_get_current_ref(&self)!=LIOT_OSI_SUCCESS || !self) return -1;
    liot_rtos_enter_critical();
    bool idle=s_task && s_rx_lock && !s_wanted && !s_status.active &&
              !s_local_decoder_task && !s_decoder && !s_line && !s_output;
    if (idle) s_local_decoder_task=self;
    liot_rtos_exit_critical();
    if (!idle) return -3;

    s_line=liot_rtos_malloc(LINE_BYTES);
    s_output=liot_rtos_malloc(OUTPUT_CAPACITY_BYTES);
    if (s_line && s_output) s_decoder=JpegD_Create();
    if (!s_line || !s_output || !s_decoder) {
        int error=(!s_line || !s_output) ? VIDEO_ERROR_MEMORY : VIDEO_ERROR_CODEC;
        tirtc_video_local_decode_end();
        return error;
    }
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
    jpeg_probe_begin();
#endif
    return 0;
}

int tirtc_video_local_decode_step(uint8_t *jpeg,uint32_t length,bool publish,
                                  tirtc_video_local_decode_stats_t *stats)
{
    if (!local_decoder_owned() || !s_decoder) return -3;
    s_local_pixels_valid=false;
    if (stats) memset(stats,0,sizeof(*stats));
    uint32_t began=decode_stamp(stats);
    int result=decode_present(0U,jpeg,length,publish,stats);
    if (stats) stats->total_ms=decode_stamp(stats)-began;
    if (!result) s_local_pixels_valid=true;
    return result;
}

const uint16_t *tirtc_video_local_decode_pixels(void)
{
    return local_decoder_owned() && s_local_pixels_valid ? s_output : NULL;
}

void tirtc_video_local_decode_end(void)
{
    if (!local_decoder_owned()) return;
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
    jpeg_probe_end(0U);
#endif
    s_local_pixels_valid=false;
    if (s_decoder) JpegD_Destroy(s_decoder);
    if (s_output) liot_rtos_free(s_output);
    if (s_line) liot_rtos_free(s_line);
    liot_rtos_enter_critical();
    s_decoder=NULL; s_output=NULL; s_line=NULL; s_local_decoder_task=NULL;
    liot_rtos_exit_critical();
}
#endif
/* Recheck after encoding: another task may have consumed memory meanwhile.
 * Avoid adding a frame to an already delayed queue. SDK result zero means
 * not accepted; it must not be reported as a transmitted frame. */
static bool send_frame(demo_tirtc_owner_e owner,uint32_t generation,bool wechat,
                       unsigned length,uint32_t captured_at,size_t *queued)
{
    bool enabled;
    size_t allocation_budget=length+(length/1000U+1U)*64U+1024U;
    size_t largest_needed=length+2048U;
    if (largest_needed<BLOCK_PAUSE_BYTES) largest_needed=BLOCK_PAUSE_BYTES;
    if (!memory_admit() || liot_xPortGetFreeHeapSize()<HEAP_PAUSE_BYTES+allocation_budget ||
        liot_xPortGetMaximumFreeBlockSize()<largest_needed ||
        demo_tirtc_get_send_buffer_used(owner,generation,queued)!=0 ||
        *queued>=SEND_QUEUE_TARGET || length>SEND_QUEUE_TARGET-*queued) return false;
    liot_rtos_enter_critical();
    enabled=s_enabled && s_wanted && s_generation==generation;
    liot_rtos_exit_critical();
    if (!enabled) return false;
    TIRTCFRAMEINFO frame={0};
    frame.stream_id=wechat ? 1 : 11; frame.media=TIRTC_VIDEO_JPEG; frame.flags=TIRTC_FRAME_FLAG_KEY_FRAME;
    frame.ts=captured_at; frame.length=length;
#if TIRTC_F9_CALL_AUDIO_QOS
    if (calls_audio_video_should_defer(owner,generation)) {
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        ++s_media_diag.audio_hold_late_frames;
#endif
        return false;
    }
#endif
    uint32_t began=now_ms();
    int ret=demo_tirtc_send_video(owner,generation,&frame,s_jpeg);
    uint32_t elapsed=now_ms()-began;
    liot_rtos_enter_critical();
    s_diag.send_ms=elapsed;
    if (!ret) s_diag.send_zero++; else if (ret<0) s_diag.send_error++;
    liot_rtos_exit_critical();
    return ret>0;
}
static bool send_counted_frame(demo_tirtc_owner_e owner,uint32_t generation,bool wechat,
                               unsigned length,uint32_t captured_at,size_t *queued)
{
    bool sent=send_frame(owner,generation,wechat,length,captured_at,queued);
    liot_rtos_enter_critical();
    if (sent) {
        s_status.tx_frames++; s_diag.tx_bytes+=length;
#if TIRTC_VIDEO12_DIAGNOSTICS
        if (duplex_12fps(owner)) video12_trace_offer(0U,captured_at);
#endif
    } else { s_status.dropped_frames++; s_diag.tx_not_accepted++; }
    liot_rtos_exit_critical();
    return sent;
}
/* Called only by the codec worker. Release the raw lease on every exit;
 * the camera producer can reuse it as soon as the encoder has returned. */
static int encode_next(demo_tirtc_owner_e owner,uint32_t generation,bool wechat,
                       size_t *queued,unsigned *jpeg_length,bool *sent)
{
    unsigned slot=0;
    uint8_t *data=NULL;
    uint32_t captured_at=0,capture_ms=0;
    bool enabled;
    *jpeg_length=0; *sent=false;
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (s_fixture_jpeg_length) {
        unsigned length=s_fixture_jpeg_length;
        captured_at=now_ms();
        *jpeg_length=length;
        *sent=send_counted_frame(owner,generation,wechat,length,captured_at,queued);
        return 1;
    }
#endif
    bool leased_camera_frame=true;
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
    if (s_test_fixture && !uses_camera()) {
        data=s_fixture_pixels;
        captured_at=now_ms();
        leased_camera_frame=false;
    }
#endif
    if (leased_camera_frame &&
        !tirtc_capture_pipeline_take(generation,&slot,&data,&captured_at,&capture_ms)) return 0;
    liot_rtos_enter_critical(); enabled=s_enabled; liot_rtos_exit_critical();
    if (!data || !enabled || !requested(generation) || !memory_admit()) {
        if (leased_camera_frame) tirtc_capture_pipeline_release(generation,slot);
        return 0;
    }
    uint32_t began=now_ms();
    const bool square=wx_square_video();
    if (leased_camera_frame) {
#if TIRTC_F9_WX_NATIVE_CAPTURE
        if (s_native_wx240) {
            if (!wx240_guard_ok(data,16U)) {
                tirtc_capture_pipeline_release(generation,slot);
                s_native_disabled=true;
                return VIDEO_ERROR_CAMERA;
            }
        } else
#endif
        if (square) tirtc_video_prepare_wx_square_yuyv(data);
        else tirtc_video_prepare_yuyv(data);
    }
    JPEG_IMAGE_BUF raw={JPEG_COLOR_FMT_YUYV,
        square?TIRTC_F9_WX_SQUARE_SIZE:ENC_W,square?TIRTC_F9_WX_SQUARE_SIZE:ENC_H,{data,NULL,NULL}};
    unsigned length=JPEG_MAX;
#if TIRTC_VIDEO12_DIAGNOSTICS
    uint32_t sdk_start=now_ms();
#endif
    int ret=JpegE_Encode(s_encoder,&raw,s_jpeg,&length);
    uint32_t elapsed=now_ms()-began;
#if TIRTC_VIDEO12_DIAGNOSTICS
    if (duplex_12fps(owner)) {
        video12_span_add(&s_video12_timing.encode,began+elapsed-sdk_start);
        s_video12_timing.encode_queue_ms+=sdk_start-captured_at;
        s_video12_timing.camera_selected_ms+=capture_ms;
    }
#endif
    if (leased_camera_frame) tirtc_capture_pipeline_release(generation,slot);
    liot_rtos_enter_critical();
    s_status.capture_ms=capture_ms; s_status.encode_ms=elapsed;
    liot_rtos_exit_critical();
    const bool valid=ret>=0 && length>=4 && length<=JPEG_MAX;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    if (valid) ++s_media_diag.encoded; else ++s_media_diag.encode_failed;
    s_media_diag.encode_ms+=elapsed;
    if (elapsed>s_media_diag.encode_peak_ms) s_media_diag.encode_peak_ms=elapsed;
#endif
    if (!valid) return VIDEO_ERROR_CODEC;
    *jpeg_length=length;
    *sent=send_counted_frame(owner,generation,wechat,length,captured_at,queued);
    return 1;
}

/* Counter deltas are bounded to a log interval; 64-bit arithmetic avoids
 * overflow if a stalled worker makes that interval unusually long. */
static uint32_t fps_milli(uint32_t count,uint32_t elapsed)
{
    return elapsed ? (uint32_t)((uint64_t)count*1000000U/elapsed) : 0U;
}

#if TIRTC_CALL_MEDIA_DIAGNOSTICS
/* Each line is a cumulative snapshot. Host tools can difference two samples
 * or use final=1 alone, without merging frame-by-frame serial output. */
static void media_video_report(demo_tirtc_owner_e owner,uint32_t generation,bool final)
{
    if (owner==DEMO_TIRTC_OWNER_LIVE) return;
    const uint32_t tick=now_ms();
    if (!final && tick-s_media_diag.last_report<MEDIA_REPORT_MS) return;
    s_media_diag.last_report=tick;
    video_media_diag_t media;
    video_diag_t diag;
    tirtc_video_snapshot_t status;
    liot_rtos_enter_critical();
    if (final) s_media_diag.closed=true;
    media=s_media_diag;
    diag=s_diag;
    status=s_status;
    liot_rtos_exit_critical();
    const unsigned long gen=(unsigned long)generation;
    const unsigned end=final?1U:0U;
    liot_trace("[MEDIA-VRX] gen=%lu final=%u ms=%lu app_in=%lu queued=%lu bad_arg=%lu oversized=%lu format=%lu stream=%lu lock=%lu closed=%lu replaced=%lu pending_stop=%lu "
               "full=%lu aged=%lu open_ms=%lu ready_ms=%lu warm_queued=%lu\r\n",
               gen,end,(unsigned long)(tick-media.started),(unsigned long)media.forwarded,
               (unsigned long)diag.incoming,(unsigned long)media.bad_arg,(unsigned long)diag.oversized,
               (unsigned long)diag.format_drop,(unsigned long)media.stream_drop,(unsigned long)diag.lock_drop,
               (unsigned long)media.admission_closed,(unsigned long)diag.replaced,(unsigned long)media.stop_pending,
               (unsigned long)(diag.replaced-media.queue_age_drop),(unsigned long)media.queue_age_drop,
               (unsigned long)media.rx_open_ms,(unsigned long)media.rx_ready_ms,(unsigned long)media.warm_queued);
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    liot_trace("[MEDIA-VWAIT] gen=%lu final=%u take=%lu sum_ms=%lu max_ms=%lu kept=%lu rx_first=%lu age_drop_max_ms=%lu full_drop_max_ms=%lu gui_wait_polls=%lu in_tx=%lu\r\n",
               gen,end,(unsigned long)media.queue_takes,(unsigned long)media.wait_sum_ms,
               (unsigned long)media.wait_peak_ms,(unsigned long)media.kept_over_period,
               (unsigned long)media.rx_first,(unsigned long)media.age_drop_peak_ms,
               (unsigned long)media.full_drop_peak_ms,(unsigned long)media.gui_wait_polls,
               (unsigned long)media.arrived_during_tx);
#endif
    liot_trace("[MEDIA-VDEC] gen=%lu final=%u try=%lu ok=%lu fail=%lu cancel=%lu mem=%lu job_sum_ms=%lu job_max_ms=%lu out=%lux%lu in=%lux%lu\r\n",
               gen,end,(unsigned long)media.decode_jobs,(unsigned long)media.decoded,
               (unsigned long)(media.decode_jobs-media.decoded-media.decode_cancelled),
               (unsigned long)media.decode_cancelled,(unsigned long)diag.memory_drop,
               (unsigned long)media.decode_ms,(unsigned long)media.decode_peak_ms,
               (unsigned long)diag.out_width,(unsigned long)diag.out_height,
               (unsigned long)diag.width,(unsigned long)diag.height);
    liot_trace("[MEDIA-VLCD] gen=%lu final=%u pub=%lu pub_fail=%lu pub_sum_ms=%lu consume=%lu replace=%lu lcd_ok=%lu direct=%lu lvgl=%lu direct_fail=%lu lvgl_fail=%lu\r\n",
               gen,end,(unsigned long)media.published,(unsigned long)media.publish_failed,
               (unsigned long)media.publish_ms,(unsigned long)media.ui[TIRTC_VIDEO_UI_CONSUMED],
               (unsigned long)media.ui[TIRTC_VIDEO_UI_REPLACED],
               (unsigned long)(media.ui[TIRTC_VIDEO_UI_DIRECT_OK]+media.ui[TIRTC_VIDEO_UI_LVGL_OK]),
               (unsigned long)media.ui[TIRTC_VIDEO_UI_DIRECT_OK],(unsigned long)media.ui[TIRTC_VIDEO_UI_LVGL_OK],
               (unsigned long)media.ui[TIRTC_VIDEO_UI_DIRECT_FAIL],(unsigned long)media.ui[TIRTC_VIDEO_UI_LVGL_FAIL]);
    tirtc_capture_pipeline_stats_t camera;
    sample_camera_pipeline(&camera);
    if (camera.generation!=generation) memset(&camera,0,sizeof(camera));
    liot_trace("[MEDIA-VTX] gen=%lu final=%u cap_ok=%lu cap_err=%lu cap_replace=%lu cap_discard=%lu enc_ok=%lu enc_fail=%lu sdk_ok=%lu sdk_zero=%lu sdk_err=%lu app_reject=%lu\r\n",
               gen,end,(unsigned long)camera.captured,(unsigned long)camera.errors,
               (unsigned long)camera.replaced,(unsigned long)camera.discarded,
               (unsigned long)media.encoded,(unsigned long)media.encode_failed,
               (unsigned long)status.tx_frames,(unsigned long)diag.send_zero,(unsigned long)diag.send_error,
               (unsigned long)(diag.tx_not_accepted-diag.send_zero-diag.send_error));
#if TIRTC_F9_CALL_AUDIO_QOS
    liot_trace("[MEDIA-VQOS] gen=%lu final=%u audio_hold_polls=%lu after_encode_drop=%lu\r\n",
               gen,end,(unsigned long)media.audio_hold_polls,(unsigned long)media.audio_hold_late_frames);
#endif
    demo_tirtc_video_send_stats_t net={0};
    const int net_rc=demo_tirtc_get_video_send_stats(owner,generation,&net);
    const bool fresh=!net_rc && net.available && !net.stale;
    liot_trace("[MEDIA-VQ] gen=%lu final=%u enc_sum_ms=%lu enc_max_ms=%lu gate_polls=%lu q_peak_B=%lu net_reg=%u net_fresh=%u rtt_ms=%lu sdk_pending=%lu sdk_qdrop=%lu loss_ppm=%ld\r\n",
               gen,end,(unsigned long)media.encode_ms,(unsigned long)media.encode_peak_ms,
               (unsigned long)media.tx_gate_hits,(unsigned long)media.tx_queue_peak,
               net.registered?1U:0U,fresh?1U:0U,fresh?(unsigned long)net.rtt_ms:0UL,
               fresh?(unsigned long)net.pending_frames:0UL,fresh?(unsigned long)net.dropped_pending_frames:0UL,
               fresh && net.loss_available?(long)net.loss_rate_ppm:-1L);
}
#endif

static void log_uplink_feedback(demo_tirtc_owner_e owner,uint32_t generation)
{
    demo_tirtc_video_send_stats_t net;
    char loss[16]="NA";
    int ret=demo_tirtc_get_video_send_stats(owner,generation,&net);
    if (ret || !net.available || net.stale) {
        TIRTC_LOG_DEBUG("[VIDEO40-NET] result=%d registration=%d registered=%u feedback=%s age_ms=%lu loss=NA\r\n",
            ret,net.registration_result,net.registered?1U:0U,
            net.available?"stale":"none",(unsigned long)net.age_ms);
        return;
    }
    if (net.loss_available) (void)snprintf(loss,sizeof(loss),"%lu",(unsigned long)net.loss_rate_ppm);
    TIRTC_LOG_DEBUG("[VIDEO40-NET] event=%lu type=%u reason=%u age_ms=%lu rtt_ms=%lu peer_bps=%lu unacked=%lu pending=%lu wait_ms=%lu sdk_drop=%lu loss_valid=%u loss_ppm=%s\r\n",
        (unsigned long)net.event_seq,net.event_type,net.reason,(unsigned long)net.age_ms,
        (unsigned long)net.rtt_ms,(unsigned long)net.receive_bitrate_bps,
        (unsigned long)net.unacked_segments,(unsigned long)net.pending_frames,
        (unsigned long)net.pending_wait_ms,(unsigned long)net.dropped_pending_frames,
        net.loss_available?1U:0U,loss);
}

#if TIRTC_VIDEO12_DIAGNOSTICS
static void video12_state(demo_tirtc_owner_e owner, uint32_t generation, unsigned blocked, bool final)
{
    uint32_t tick=now_ms();
    tirtc_video_snapshot_t status;
    tirtc_capture_pipeline_stats_t capture;
    video_diag_t diag;
    uint32_t callbacks;
    liot_rtos_enter_critical();
    status=s_status;
    diag=s_diag;
    callbacks=s_receive_callbacks;
    liot_rtos_exit_critical();
    sample_camera_pipeline(&capture);
    liot_trace("[V12-STATE] gen=%lu tick=%lu final=%u tx=%lu cb=%lu accepted=%lu replaced=%lu dec_ok=%lu dec_fail=%lu "
               "lock_drop=%lu format_drop=%lu oversized=%lu memory_drop=%lu tx_reject=%lu tx_defer=%u "
               "cap=%lu cap_replace=%lu in_w=%lu in_h=%lu out_w=%lu out_h=%lu\r\n",
               (unsigned long)generation,(unsigned long)tick,final?1U:0U,
               (unsigned long)status.tx_frames,(unsigned long)callbacks,(unsigned long)diag.incoming,
               (unsigned long)diag.replaced,(unsigned long)status.rx_frames,(unsigned long)diag.decode_drop,
               (unsigned long)diag.lock_drop,(unsigned long)diag.format_drop,
               (unsigned long)diag.oversized,(unsigned long)diag.memory_drop,
               (unsigned long)diag.tx_not_accepted,blocked,(unsigned long)capture.captured,
               (unsigned long)capture.replaced,(unsigned long)diag.width,(unsigned long)diag.height,
               (unsigned long)diag.out_width,(unsigned long)diag.out_height);
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
    liot_trace("[V12-RXQ] gen=%lu tick=%lu final=%u slots=%lu peak=%lu overflow=%lu "
               "stale=%lu wait_max=%lu stop_pending=%lu\r\n",
               (unsigned long)generation,(unsigned long)tick,final?1U:0U,
               (unsigned long)diag.rxq_slots,(unsigned long)diag.rxq_peak,
               (unsigned long)diag.rxq_overflow,(unsigned long)diag.rxq_stale,
               (unsigned long)diag.rxq_wait_max,(unsigned long)diag.rxq_stop_pending);
#endif
    tirtc_camera_fence_stats_t camera={0};
    tirtc_camera_fence_get_stats(&camera);
#if TIRTC_VIDEO_VPU_DIAGNOSTICS || TIRTC_VIDEO_VPU_YIELD_ENABLE || TIRTC_VIDEO_VPU_ISOLATION
    /* The final VPU record is emitted at session end, after the ordinary
     * counters below. Periodic lock observations reuse this 5s cadence. */
    if (!final) tirtc_vpu_report(generation,0U);
#endif
    const video12_span_t enc=s_video12_timing.encode, dec=s_video12_timing.decode;
    liot_trace("[V12-TIME] gen=%lu tick=%lu final=%u enc_n=%lu enc_ms=%lu enc_max=%lu "
               "dec_n=%lu dec_ms=%lu dec_max=%lu enc_wait=%lu cam_n=%lu cam_ms=%lu "
               "cam_last_valid=%u cam_dma_last=%lu cam_resume_last=%lu cam_sdk_last=%lu "
               "cam_attempt=%lu cam_done=%lu cam_timeout=%lu\r\n",
               (unsigned long)generation,(unsigned long)tick,final?1U:0U,
               (unsigned long)enc.count,(unsigned long)enc.sum,(unsigned long)enc.max,
               (unsigned long)dec.count,(unsigned long)dec.sum,(unsigned long)dec.max,
               (unsigned long)s_video12_timing.encode_queue_ms,(unsigned long)enc.count,
               (unsigned long)s_video12_timing.camera_selected_ms,camera.last_timing_valid?1U:0U,
               (unsigned long)(camera.last_timing_valid?camera.last_dma_ms:0U),
               (unsigned long)(camera.last_timing_valid?camera.last_resume_ms:0U),
               (unsigned long)(camera.last_timing_valid?camera.last_sdk_ms:0U),
               (unsigned long)camera.captures,(unsigned long)camera.completions,
               (unsigned long)camera.timeouts);
#if TIRTC_VIDEO_LOCAL_UI_BENCH
    if (s_live_decode_frames) {
        const tirtc_video_local_decode_stats_t *p=&s_live_decode_phases;
        liot_trace("[V12-DECODE-PHASE] gen=%lu tick=%lu final=%u n=%lu total_ms=%lu "
                   "geometry_ms=%lu info_ms=%lu rows_ms=%lu copy_ms=%lu publish_ms=%lu slices=%lu\r\n",
                   (unsigned long)generation,(unsigned long)tick,final?1U:0U,
                   (unsigned long)s_live_decode_frames,(unsigned long)p->total_ms,
                   (unsigned long)p->geometry_ms,(unsigned long)p->info_ms,
                   (unsigned long)p->rows_ms,(unsigned long)p->copy_ms,
                   (unsigned long)p->publish_ms,(unsigned long)p->slices);
    }
#endif
    demo_tirtc_video_send_stats_t net={0};
    int result=demo_tirtc_get_video_send_stats(owner,generation,&net);
    bool available=result==0 && net.available && !net.stale && net.loss_available;
    liot_trace("[V12-NET] gen=%lu tick=%lu event=%lu age=%lu n=%u ppm=%lu\r\n",
               (unsigned long)generation,(unsigned long)tick,(unsigned long)net.event_seq,
               (unsigned long)net.age_ms,available?1U:0U,
               (unsigned long)(available?net.loss_rate_ppm:0U));
}
#endif

/* The worker owns both codecs. Run at most one RX job and one due TX job
 * per pass, in either order, and keep per-job memory admission unchanged. */
static void decode_slot(int slot,uint32_t generation,bool duplex,bool pace_receive,
                        tirtc_video_rate_t *receive_rate,unsigned target_fps)
{
    (void)duplex;
    bool memory_ready=memory_admit();
    uint32_t began=now_ms();
    if (pace_receive) tirtc_video_rate_advance_at(receive_rate,began,target_fps);
    tirtc_video_local_decode_stats_t *timing=NULL;
#if TIRTC_VIDEO_LOCAL_UI_BENCH && TIRTC_VIDEO12_DIAGNOSTICS
    tirtc_video_local_decode_stats_t stages={0};
    if (duplex) timing=&stages;
#endif
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    const uint32_t decoded_before=s_media_diag.decoded;
#endif
    int ret=memory_ready ? decode_present(generation,s_rx[slot],s_rx_len[slot],
                                           publish_video(),timing) : VIDEO_ERROR_MEMORY;
    if (!ret) tirtc_rx_lcd_record(generation,TIRTC_RX_LCD_DEC);
    uint32_t elapsed=now_ms()-began;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
    if (memory_ready) {
        ++s_media_diag.decode_jobs;
        if (s_media_diag.decoded==decoded_before && ret==-1)
            ++s_media_diag.decode_cancelled;
        s_media_diag.decode_ms+=elapsed;
        if (elapsed>s_media_diag.decode_peak_ms) s_media_diag.decode_peak_ms=elapsed;
    }
#endif
#if TIRTC_VIDEO_LOCAL_UI_BENCH && TIRTC_VIDEO12_DIAGNOSTICS
    if (!ret && timing) {
        ++s_live_decode_frames;
        s_live_decode_phases.total_ms+=elapsed;
        s_live_decode_phases.geometry_ms+=stages.geometry_ms;
        s_live_decode_phases.info_ms+=stages.info_ms;
        s_live_decode_phases.rows_ms+=stages.rows_ms;
        s_live_decode_phases.copy_ms+=stages.copy_ms;
        s_live_decode_phases.publish_ms+=stages.publish_ms;
        s_live_decode_phases.slices+=stages.slices;
    }
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
    if (duplex && memory_ready) video12_span_add(&s_video12_timing.decode,elapsed);
#endif
    liot_rtos_enter_critical();
    s_status.decode_ms=elapsed;
    if (!ret) s_status.rx_frames++;
    else { s_status.dropped_frames++; if (memory_ready) s_diag.decode_drop++; else s_diag.memory_drop++; }
    liot_rtos_exit_critical();
    (void)liot_rtos_mutex_lock(s_rx_lock,LIOT_WAIT_FOREVER); s_decoding=-1; (void)liot_rtos_mutex_unlock(s_rx_lock);
}

static void task(void *context)
{
    (void)context;
#if TIRTC_VIDEO_VPU_DIAGNOSTICS || TIRTC_VIDEO_VPU_YIELD_ENABLE || TIRTC_VIDEO_VPU_ISOLATION
    tirtc_vpu_worker_start();
#endif
    for (;;) {
        uint32_t generation; demo_tirtc_owner_e owner; bool wechat,wanted,active;
        liot_rtos_enter_critical();
        generation=s_generation; owner=s_owner; wechat=s_wechat; wanted=s_wanted; active=s_status.active;
        liot_rtos_exit_critical();
        if (!wanted) {
            /* A stop can arrive before this worker observes a queued start. */
            if (active) {
                while (!hardware_close()) liot_rtos_task_sleep_ms(25);
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
                media_video_report(owner,generation,true);
#endif
                liot_rtos_enter_critical(); s_status.ready=false; s_status.active=false; liot_rtos_exit_critical();
            }
            liot_rtos_task_sleep_ms(20); continue;
        }
        s_pending=s_decoding=-1;
#if TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
        s_pending_newer=-1;
#endif
        s_memory_paused=false;
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
        bool fixture_vpu_scope=false;
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
        bool jpeg_prio_ab=false;
        unsigned jpeg_prio_block=0U, jpeg_priority=11U;
        jpeg_prio_sample_t jpeg_prio_before={0};
#endif
#endif
        int error=hardware_open();
#if TIRTC_F9_AV_FOCUS
        uint8_t saved_priority=0;
        bool focused=false;
        if (!error && requested(generation) && duplex_12fps(owner)) {
            focused=liot_rtos_task_change_priority(s_task,TIRTC_F9_AV_VIDEO_PRIORITY,
                                                   &saved_priority)==LIOT_OSI_SUCCESS;
            const uint8_t camera_priority=TIRTC_F9_AV_AUDIO_PRIORITY-1U;
            if (!focused || (uses_camera() && !tirtc_capture_pipeline_focus(camera_priority)))
                error=VIDEO_ERROR_CLOSE;
        }
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
        bool trace_started=false;
        unsigned last_blocked=0;
#endif
        if (!error && requested(generation)) {
            /* Serialize initial enable with UI stop/mute. The pipeline start
             * only updates metadata and never blocks or allocates. */
            liot_rtos_enter_critical();
            if (uses_camera() && s_wanted && s_generation==generation &&
                tirtc_capture_pipeline_start(s_camera,generation,s_raw,s_raw_next,s_enabled)!=0)
                error=VIDEO_ERROR_CAMERA;
            liot_rtos_exit_critical();
        }
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
        if (!error && requested(generation) && s_test_fixture) {
            const int scope_result=tirtc_vpu_fixture_call_begin((uint32_t)owner,generation,(s_test_flags & ISO_CALL_TUNED)!=0U);
            fixture_vpu_scope=scope_result==0;
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
            if (fixture_vpu_scope && decode_video() && (s_test_flags & ISO_CALL_NO_TX)) {
                if (!tirtc_jpeg_tasks_begin(generation))
                    liot_trace("[V12-JPEG-TASK-ERROR] gen=%lu rc=-1\r\n",(unsigned long)generation);
                jpeg_probe_begin();
                jpeg_prio_ab=(s_test_flags & ISO_CALL_PRIO_AB)!=0U && jpeg_probe_active;
                if (jpeg_prio_ab) jpeg_prio_before=jpeg_prio_sample();
            }
#endif
            if (!fixture_vpu_scope) {
                liot_trace("[V12-VPU-LEASE] gen=%lu rc=%d\r\n",
                           (unsigned long)generation,scope_result);
                error=scope_result;
            }
        }
#endif
        if (!error && requested(generation)) {
            int monitor_ret=0;
            bool monitor_needed=TIRTC_ENABLE_DEBUG_LOG;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
            monitor_needed=monitor_needed || owner!=DEMO_TIRTC_OWNER_LIVE;
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
            monitor_needed=monitor_needed || duplex_12fps(owner);
#endif
            if (monitor_needed)
                monitor_ret=demo_tirtc_video_send_monitor_start(owner,generation,wechat?1U:11U);
            TIRTC_LOG_DEBUG("[VIDEO40-NET] monitor registration=%d stream=%u (optional diagnostics)\r\n",
                monitor_ret,wechat?1U:11U);
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
            if (s_test_fixture) tirtc_rx_lcd_begin(generation);
#endif
            if (!s_accept) open_receive_admission(generation);
            liot_rtos_enter_critical();
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
            s_media_diag.rx_ready_ms=now_ms()-s_media_diag.started;
#endif
            s_status.ready=true;
            liot_rtos_exit_critical();
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
            if (s_test_fixture) {
                const bool test_tx=!(s_test_flags & ISO_CALL_NO_TX);
                liot_trace("[V12-RXSCHED] gen=%lu priority=%u protect_audio=%u\r\n",
                    (unsigned long)generation,(s_test_flags & ISO_CALL_PRIO_AB)?11U:TIRTC_VIDEO_RX_PRIORITY,
                    (s_test_flags & ISO_CALL_AUDIO)?1U:0U);
                liot_trace("[V12-ISO-CONFIG] gen=%lu camera=%u w=%u h=%u q=%u pattern=1\r\n",
                    (unsigned long)generation,uses_camera()?1U:0U,(unsigned)ENC_W,(unsigned)ENC_H,
                    call_jpeg_quality());
#if TIRTC_F9_AV_FOCUS
                liot_trace("[V12-FOCUS] gen=%lu video=%u gui=27 camera=%u audio=%u\r\n",
                    (unsigned long)generation,TIRTC_F9_AV_VIDEO_PRIORITY,uses_camera()?30U:0U,
                    (s_test_flags&ISO_CALL_AUDIO)?TIRTC_F9_AV_AUDIO_PRIORITY:0U);
#endif
                liot_trace("[V12-PIPE] gen=%lu tx=%u rx=%u enc=%u dec=%u lcd=%u jpeg=%u\r\n",
                    (unsigned long)generation,test_tx?1U:0U,(s_test_flags & ISO_CALL_NO_RX)?0U:1U,
                    test_tx&&!s_fixture_jpeg_length?1U:0U,decode_video()?1U:0U,publish_video()?1U:0U,
                    s_fixture_jpeg_length);
            }
#endif
            const bool duplex=duplex_12fps(owner);
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
            /* An owned unidirectional test is driven by the Windows source rate. */
            const bool pace_receive=wx_square_video() || (duplex && !TIRTC_F9_AV_FOCUS &&
                                    !(s_test_fixture && (s_test_flags & ISO_CALL_NO_TX)));
#else
            const bool pace_receive=wx_square_video() || (duplex && !TIRTC_F9_AV_FOCUS);
#endif
#if TIRTC_VIDEO_VPU_DIAGNOSTICS || TIRTC_VIDEO_VPU_YIELD_ENABLE || TIRTC_VIDEO_VPU_ISOLATION
            if (duplex
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
                && !fixture_vpu_scope
#endif
                ) tirtc_vpu_session_begin((uint32_t)owner,generation);
#endif
            const unsigned target_fps=call_tx_fps(duplex);
            const unsigned rx_fps=wx_square_video()?TIRTC_F9_WX_RX_FPS:target_fps;
            tirtc_video_rate_t rate,receive_rate;
            uint32_t clock=now_ms(), next_memory_poll=clock, next_send_check=clock;
            bool memory_ready=true;
            tirtc_video_rate_reset(&rate,clock);
            tirtc_video_rate_reset(&receive_rate,clock);
            uint32_t last_log=now_ms(),last_tx=0,last_rx=0,last_cap=0,last_bytes=0;
            size_t queued=0;
            size_t heap_min=TIRTC_ENABLE_DEBUG_LOG?liot_xPortGetFreeHeapSize():0U,block_min=TIRTC_ENABLE_DEBUG_LOG?liot_xPortGetMaximumFreeBlockSize():0U,queue_peak=0;
            unsigned last_jpeg=0, blocked=0;
#if TIRTC_VIDEO12_DIAGNOSTICS
            trace_started=duplex;
            uint32_t next_trace_state=clock+5000U;
            if (trace_started) video12_state(owner,generation,0U,false);
#endif
            while (requested(generation)) {
                int slot=-1;
                bool decoded_before_tx=false;
                clock=now_ms();
                /* Recheck immediately at codec admission. The idle loop
                 * samples bounded heap pressure less often while waiting. */
                if (!duplex || (int32_t)(clock-next_memory_poll)>=0) {
                    memory_ready=memory_admit();
                    if (uses_camera()) (void)tirtc_capture_pipeline_set_paused(generation,!memory_ready);
                    next_memory_poll=clock+20U;
                }
                tirtc_capture_pipeline_stats_t pipeline;
                sample_camera_pipeline(&pipeline);
                if (pipeline.error) {
                    liot_trace("[VIDEO40-CAM] capture_failed ret=%d errors=%lu in_flight=%u\r\n",
                        pipeline.error,(unsigned long)pipeline.errors,pipeline.in_flight?1U:0U);
                    error=VIDEO_ERROR_CAMERA; break;
                }
                bool enabled;
                liot_rtos_enter_critical(); enabled=s_enabled; liot_rtos_exit_critical();
                /* Give a ready outgoing frame first use of the shared codec.
                 * Capture continues while this worker decodes remote video. */
                if (enabled && (int32_t)(clock-next_send_check)>=0 &&
                    tirtc_video_rate_due(&rate,clock)) {
#if TIRTC_F9_AV_FOCUS && TIRTC_VIDEO_GUI_BACKPRESSURE_ENABLE && TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
                    if (duplex && decode_video() &&
                        (!pace_receive || tirtc_video_rate_due(&receive_rate,clock))) {
                        int urgent=take_backlog_before_tx(now_ms(),&rate);
                        if (urgent>=0) {
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
                            ++s_media_diag.rx_first;
#endif
                            decode_slot(urgent,generation,duplex,pace_receive,&receive_rate,rx_fps);
                            decoded_before_tx=true;
                            if (!requested(generation)) break;
                        }
                    }
#endif
                    uint32_t admitted_at=now_ms();
                    if (duplex) memory_ready=memory_admit();
                    uint32_t send_retry_ms=5U;
                    bool queue_ready=memory_ready;
#if TIRTC_F9_CALL_AUDIO_QOS
                    if (queue_ready && calls_audio_video_should_defer(owner,generation)) {
                        queue_ready=false;
                        send_retry_ms=10U;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
                        ++s_media_diag.audio_hold_polls;
#endif
                    }
#endif
                    queue_ready=queue_ready &&
                        demo_tirtc_get_send_buffer_used(owner,generation,&queued)==0 &&
                        queued<SEND_QUEUE_TARGET;
                    /* A nearly full queue used to consume codec time for a
                     * JPEG that send_frame() would immediately reject. Use
                     * the previous encoded size as an admission estimate;
                     * the producer still keeps its newest complete raw frame.
                     * An empty queue always allows a fresh size estimate, so
                     * a previous oversized scene cannot stall encoding forever.
                     * send_frame() still checks actual bytes and current state. */
                    if (queue_ready && queued && last_jpeg>SEND_QUEUE_TARGET-queued) {
                        queue_ready=false;
                        liot_rtos_enter_critical(); s_diag.encode_defer++; liot_rtos_exit_critical();
                    }
                    if (!queue_ready) {
                        blocked++;
                        if (duplex && TIRTC_F9_AV_FOCUS)
                            next_send_check=admitted_at+send_retry_ms;
                        else
                            tirtc_video_rate_advance_at(&rate,admitted_at,target_fps);
                    } else {
                        bool sent=false;
                        unsigned length=0;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS && TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
                        liot_rtos_enter_critical(); s_media_diag.worker_encoding=true; liot_rtos_exit_critical();
#endif
                        int ret=encode_next(owner,generation,wechat,&queued,&length,&sent);
#if TIRTC_CALL_MEDIA_DIAGNOSTICS && TIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE
                        liot_rtos_enter_critical(); s_media_diag.worker_encoding=false; liot_rtos_exit_critical();
#endif
                        if (ret<0) { error=ret; break; }
                        if (ret>0) {
                            tirtc_video_rate_advance_at(&rate,admitted_at,target_fps);
                            last_jpeg=length;

                        }
                    }
                }
                if (!decoded_before_tx && owner!=DEMO_TIRTC_OWNER_LIVE && decode_video() &&
                    (!pace_receive || tirtc_video_rate_due(&receive_rate,now_ms()))) {
#if TIRTC_VIDEO_GUI_BACKPRESSURE_ENABLE
                    slot=take_pending_for_gui();
#else
                    (void)liot_rtos_mutex_lock(s_rx_lock,LIOT_WAIT_FOREVER);
                    slot=take_pending_locked();
                    (void)liot_rtos_mutex_unlock(s_rx_lock);
#endif
                }
                if (slot>=0) decode_slot(slot,generation,duplex,pace_receive,&receive_rate,rx_fps);
                /* Min/max are worker samples, not allocator-wide watermarks. */
                size_t free_sample=TIRTC_ENABLE_DEBUG_LOG?liot_xPortGetFreeHeapSize():0U;
                size_t block_sample=TIRTC_ENABLE_DEBUG_LOG?liot_xPortGetMaximumFreeBlockSize():0U;
                if (free_sample<heap_min) heap_min=free_sample;
                if (block_sample<block_min) block_min=block_sample;
                if (queued>queue_peak) queue_peak=queued;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
                s_media_diag.tx_gate_hits=blocked;
                s_media_diag.tx_queue_peak=queue_peak;
                media_video_report(owner,generation,false);
#endif
                if (TIRTC_ENABLE_DEBUG_LOG && now_ms()-last_log>=5000U) {
                    uint32_t log_time=now_ms(),dt=log_time-last_log;
                    tirtc_video_snapshot_t status; tirtc_video_get_snapshot(&status);
                    sample_camera_pipeline(&pipeline);
                    uint32_t cap_rate=fps_milli(pipeline.captured-last_cap,dt);
                    uint32_t tx_rate=fps_milli(status.tx_frames-last_tx,dt);
                    uint32_t rx_rate=fps_milli(status.rx_frames-last_rx,dt);
                    last_log=log_time; last_cap=pipeline.captured; last_tx=status.tx_frames; last_rx=status.rx_frames;
                    TIRTC_LOG_DEBUG("[VIDEO40] gen=%lu tx=%lu rx=%lu drop=%lu cap_ms=%lu enc_ms=%lu dec_ms=%lu camera=%u queue=%lu jpeg=%u blocked=%u heap=%lu max_block=%lu mem_pause=%u\r\n",
                        (unsigned long)generation,(unsigned long)status.tx_frames,(unsigned long)status.rx_frames,(unsigned long)status.dropped_frames,
                        (unsigned long)status.capture_ms,(unsigned long)status.encode_ms,(unsigned long)status.decode_ms,enabled?1U:0U,
                        (unsigned long)queued,last_jpeg,blocked,(unsigned long)liot_xPortGetFreeHeapSize(),
                        (unsigned long)liot_xPortGetMaximumFreeBlockSize(),s_memory_paused?1U:0U);
                    video_diag_t diag;
                    liot_rtos_enter_critical(); diag=s_diag; liot_rtos_exit_critical();
                    TIRTC_LOG_DEBUG("[VIDEO40-FPS] dt_ms=%lu target=%u cap=%lu.%03lu tx=%lu.%03lu rx=%lu.%03lu cap_replaced=%lu cap_errors=%lu\r\n",
                        (unsigned long)dt,target_fps,(unsigned long)(cap_rate/1000U),(unsigned long)(cap_rate%1000U),
                        (unsigned long)(tx_rate/1000U),(unsigned long)(tx_rate%1000U),
                        (unsigned long)(rx_rate/1000U),(unsigned long)(rx_rate%1000U),
                        (unsigned long)pipeline.replaced,(unsigned long)pipeline.errors);
                    uint32_t payload_bps=dt?(uint32_t)((uint64_t)(diag.tx_bytes-last_bytes)*8000U/dt):0U;
                    last_bytes=diag.tx_bytes;
                    TIRTC_LOG_DEBUG("[VIDEO40-UP] jpeg_bps=%lu bytes=%lu not_accepted=%lu blocked=%u queue_sample_peak=%lu heap_sample_min=%lu block_sample_min=%lu encode_defer=%lu\r\n",
                        (unsigned long)payload_bps,(unsigned long)diag.tx_bytes,(unsigned long)diag.tx_not_accepted,
                        blocked,(unsigned long)queue_peak,(unsigned long)heap_min,(unsigned long)block_min,
                        (unsigned long)diag.encode_defer);
                    log_uplink_feedback(owner,generation);
                    TIRTC_LOG_DEBUG("[VIDEO40-RX] in=%lu bytes=%lu size_drop=%lu format_drop=%lu lock_drop=%lu replaced=%lu decode_drop=%lu memory_drop=%lu send_zero=%lu send_error=%lu send_ms=%lu size=%lux%lu jpeg=%lu fit=%lux%lu rx_cw90=%u fill=%u crop=%lu,%lu+%lux%lu\r\n",
                        (unsigned long)diag.incoming,(unsigned long)diag.incoming_bytes,
                        (unsigned long)diag.oversized,(unsigned long)diag.format_drop,
                        (unsigned long)diag.lock_drop,(unsigned long)diag.replaced,
                        (unsigned long)diag.decode_drop,(unsigned long)diag.memory_drop,
                        (unsigned long)diag.send_zero,(unsigned long)diag.send_error,
                        (unsigned long)diag.send_ms,(unsigned long)diag.width,
                        (unsigned long)diag.height,(unsigned long)diag.input_jpeg,
                        (unsigned long)diag.out_width,(unsigned long)diag.out_height,s_rx_clockwise?1U:0U,
                        s_wechat && s_incoming?1U:0U,(unsigned long)diag.crop_x,(unsigned long)diag.crop_y,
                        (unsigned long)diag.crop_width,(unsigned long)diag.crop_height);
                    tirtc_camera_fence_stats_t capture;
                    tirtc_camera_fence_get_stats(&capture);
                    TIRTC_LOG_DEBUG("[VIDEO40-CAM] frames=%lu dma=%lu early=%lu timeout=%lu wait_ms=%lu sdk_ret=%d in_flight=%u timing_valid=%u last_dma_ms=%lu last_resume_ms=%lu last_sdk_ms=%lu rearm=%lu stop_err=%lu flush_err=%lu rearm_ret=%d\r\n",
                        (unsigned long)capture.captures,(unsigned long)capture.completions,
                        (unsigned long)capture.sdk_early_returns,(unsigned long)capture.timeouts,
                        (unsigned long)capture.last_wait_ms,capture.last_sdk_result,capture.in_flight?1U:0U,
                        capture.last_timing_valid?1U:0U,(unsigned long)capture.last_dma_ms,
                        (unsigned long)capture.last_resume_ms,(unsigned long)capture.last_sdk_ms,
                        (unsigned long)capture.rearm_prepares,(unsigned long)capture.rearm_stop_errors,
                        (unsigned long)capture.rearm_flush_errors,capture.last_rearm_result);
                }
#if TIRTC_VIDEO12_DIAGNOSTICS
                if (trace_started) {
                    video12_trace_drain(generation,false);
                    clock=now_ms();
                    if ((int32_t)(clock-next_trace_state)>=0) {
                        video12_state(owner,generation,blocked,false);
                        next_trace_state=clock+5000U;
                    }
                }
#endif
#if TIRTC_VIDEO_TASK_RUNTIME_DIAGNOSTICS
                tirtc_port_task_runtime_sample(s_task,generation,1U);
#endif
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
                if (jpeg_prio_ab && jpeg_prio_block<5U &&
                    now_ms()-jpeg_prio_before.start>=(jpeg_prio_block?8000U:3000U)) {
                    static const uint8_t next_priority[]={11U,28U,11U,28U,11U};
                    const jpeg_prio_sample_t after=jpeg_prio_sample();
                    jpeg_prio_report(generation,jpeg_prio_block,jpeg_priority,&jpeg_prio_before,&after);
                    const unsigned want=next_priority[jpeg_prio_block++];
                    if (want!=jpeg_priority) {
                        uint8_t old_priority;
                        int result=liot_rtos_task_change_priority(s_task,(uint8_t)want,&old_priority);
                        if (result==LIOT_OSI_SUCCESS) jpeg_priority=want;
                        else liot_trace("[V12-JPEG-PRIO-ERROR] gen=%lu want=%u rc=%d\r\n",
                            (unsigned long)generation,want,result);
                    }
                    jpeg_prio_before=jpeg_prio_sample();
                }
#endif
                liot_rtos_task_sleep_ms(2);
            }
#if TIRTC_VIDEO12_DIAGNOSTICS
            last_blocked=blocked;
#endif
        }
#if TIRTC_F9_WX_NATIVE_CAPTURE
        /* Keep all buffers leased until the producer stops and real DMA_END arrives.
         * A later call can then reopen the ordinary VGA camera and software crop. */
        if (error==VIDEO_ERROR_CAMERA && s_native_wx240) s_native_disabled=true;
#endif
        if (error) { set_error(error); liot_trace("[VIDEO40] failed ret=%d generation=%lu\r\n",error,(unsigned long)generation); }
#if TIRTC_VIDEO12_DIAGNOSTICS
        /* Error exits can precede the UI stop. Close trace admission before
         * draining, so no late callback can add to the finished window. */
        liot_rtos_enter_critical(); s_status.ready=false; liot_rtos_exit_critical();
#endif
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
        if (jpeg_prio_ab) {
            const jpeg_prio_sample_t after=jpeg_prio_sample();
            jpeg_prio_report(generation,jpeg_prio_block,jpeg_priority,&jpeg_prio_before,&after);
            if (jpeg_priority!=11U) {
                uint8_t old_priority;
                const int result=liot_rtos_task_change_priority(s_task,11U,&old_priority);
                if (result!=LIOT_OSI_SUCCESS)
                    liot_trace("[V12-JPEG-PRIO-ERROR] gen=%lu want=11 rc=%d\r\n",
                        (unsigned long)generation,result);
            }
        }
#endif
#if TIRTC_F9_AV_FOCUS
        if (focused) {
            uint8_t previous;
            if (uses_camera()) (void)tirtc_capture_pipeline_focus(0U);
            if (liot_rtos_task_change_priority(s_task,saved_priority,&previous)!=LIOT_OSI_SUCCESS)
                liot_trace("[V12-PRIO-RESTORE] gen=%lu rc=-1\r\n",(unsigned long)generation);
        }
#endif
        while (!hardware_close()) liot_rtos_task_sleep_ms(25);
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
        media_video_report(owner,generation,true);
#endif
#if TIRTC_VIDEO12_DIAGNOSTICS
        if (trace_started) {
            video12_trace_drain(generation,true);
            video12_state(owner,generation,last_blocked,true);
            tirtc_rx_lcd_end(generation);
        }
#endif
#if TIRTC_VIDEO_VPU_DIAGNOSTICS || TIRTC_VIDEO_VPU_YIELD_ENABLE || TIRTC_VIDEO_VPU_ISOLATION
#if TIRTC_VIDEO_ISOLATION_CALL_ENABLE
        if (fixture_vpu_scope) {
#if TIRTC_JPEG_STAGE_DIAGNOSTICS
            jpeg_probe_end(generation);
            tirtc_jpeg_tasks_end(generation);
#endif
            tirtc_vpu_isolation_end();
        } else if (!s_test_fixture && duplex_12fps(owner)) {
            tirtc_vpu_session_end(generation);
        }
#else
        if (duplex_12fps(owner)) tirtc_vpu_session_end(generation);
#endif
#endif
        liot_rtos_enter_critical(); s_wanted=false; s_status.ready=false; s_status.active=false; liot_rtos_exit_critical();
        liot_trace("[VIDEO40] stopped generation=%lu\r\n",(unsigned long)generation);
    }
}
int tirtc_video_start_service(void)
{
    liot_task_t created=NULL;
    liot_rtos_enter_critical();
    if (s_task || s_starting) { liot_rtos_exit_critical(); return s_task ? 0 : -1; }
    s_starting=true; liot_rtos_exit_critical();
    if (!s_rx_lock && liot_rtos_mutex_create(&s_rx_lock)!=LIOT_OSI_SUCCESS) {s_rx_lock=NULL;goto failed;}
    if (tirtc_capture_pipeline_service_init()!=0) goto failed;
    /* Resume video ahead of GUI work, below audio/control; keep every yield. */
    if (liot_rtos_task_create(&created,12U*1024U,11,"call_video",task,NULL)!=LIOT_OSI_SUCCESS || !created) goto failed;
    liot_rtos_enter_critical(); s_task=created; s_starting=false; liot_rtos_exit_critical(); return 0;
failed:
    liot_rtos_enter_critical(); s_starting=false; liot_rtos_exit_critical(); return -1;
}
