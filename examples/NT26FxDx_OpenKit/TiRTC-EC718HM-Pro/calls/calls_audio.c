/* SPDX-License-Identifier: MIT
 * Shared call/LIVE audio: Lite-compatible 16 kHz local PCM with 8 kHz
 * A-law network frames and bounded 8/16 kHz A-law downlink.
 * The control task submits playback then captures; only network TX is separate. */
#include "calls_audio.h"
#include "g711_codec.h"
#if TIRTC_DEV_OPUS_ENABLE
#include "dev_opus_codec.h"
#endif
#if TIRTC_DEV_OPUS_ENABLE || TIRTC_F9_WX_ADAPTIVE_PLAYOUT
#include "dev_playout.h"
#endif
#include "liot_os.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include <string.h>
#define RX_BYTES 15360U
#define RX_SEGMENTS 32U
#define PLAY_AHEAD 480U
#define PREBUFFER_BYTES 1920U
#define PREBUFFER_MS 120U
/* Match Lite05 DEV startup/rebuffering without changing WX/LIVE pacing. */
#define DEV_PREBUFFER_BYTES (200U * 8U)
#define DEV_PREBUFFER_TIMEOUT_MS 300U
#define DEV_PLAY_BATCH_MAX 8U
#define AUDIO_ERROR (-6208)
#define CALL_NETWORK_SAMPLES_8K (DEMO_AI_AUDIO_FRAME_SAMPLES / 2U)
#define CALL_TX_QUEUE_DEPTH 8U
#define WX_PLAY_RETRY_CACHE (TIRTC_F9_WX_PLAY_RETRY_CACHE && DEMO_AI_AUDIO_AEC_ENABLE)
#if TIRTC_DEV_OPUS_ENABLE
#define DEV_RX_SLOTS 32U
#define DEV_ALAW_MAX_BYTES 640U
#define DEV_TX_MAX_AGE_MS 300U
#define DEV_CAPTURE_STACK (64U*1024U)
#define CALL_TX_STACK (12U*1024U)
#if DEMO_AI_AUDIO_AEC_ENABLE && TIRTC_F9_WX_CAPTURE_RING
#define CONTROL_CAPTURE_ONCE() do { \
    if (owner!=DEMO_TIRTC_OWNER_WECHAT) capture_once(DEMO_TIRTC_OWNER_NONE); \
} while (0)
#else
#define CONTROL_CAPTURE_ONCE() capture_once(DEMO_TIRTC_OWNER_NONE)
#endif
#else
#define CALL_TX_STACK (6U*1024U)
#define CONTROL_CAPTURE_ONCE() capture_once()
#endif
static liot_task_t s_task;
static liot_task_t s_tx_task;
static liot_queue_t s_tx_queue;
static bool s_creating, s_running, s_owned, s_record_busy;
#if TIRTC_DEV_OPUS_ENABLE
static bool s_tx_busy;
#else
static bool s_tx_creating, s_tx_busy;
#endif
static demo_ai_audio_lease_t s_lease=DEMO_AI_AUDIO_LEASE_INIT;
static demo_tirtc_owner_e s_owner;
static uint32_t s_generation;
static uint8_t s_volume=DEMO_AI_AUDIO_DEFAULT_SPK_LEVEL, s_mic=DEMO_AI_AUDIO_DEFAULT_MIC_LEVEL;
static bool s_speaker=true, s_microphone=true;
static bool s_session_speaker=true, s_session_microphone=true, s_uplink_subscribed=true;
/* Adjacent equal-rate input coalesces, so normal 20-ms packets use one entry.
 * Callback metadata is bounded even if a peer alternates rate every packet. */
typedef struct { uint16_t bytes; uint8_t rate; } audio_segment_t;
static audio_segment_t s_segments[RX_SEGMENTS];
static uint32_t s_segment_head, s_segment_count;
static uint32_t s_received, s_received_bytes, s_sent_bytes;
static bool s_first_rx_pending, s_first_rx_logged, s_first_tx_logged;
static uint8_t s_first_rx_stream, s_first_rx_rate;
static uint32_t s_first_rx_length;
static uint32_t s_config_revision=1, s_applied_revision;
static bool s_mute_pending, s_draining;
static uint32_t s_drain_since;
static uint8_t s_rx[RX_BYTES], s_alaw[CALL_NETWORK_SAMPLES_8K], s_capture_alaw[CALL_NETWORK_SAMPLES_8K];
static int16_t s_pcm[DEMO_AI_AUDIO_FRAME_SAMPLES], s_capture[DEMO_AI_AUDIO_FRAME_SAMPLES];
static uint32_t s_read, s_count, s_peak, s_drops, s_played, s_sent;
#if WX_PLAY_RETRY_CACHE
/* One prepared block while the AEC playback ring is briefly full. The
 * control worker owns s_pcm; the RX critical section guards its cache key. */
static uint32_t s_wx_pcm_generation,s_wx_pcm_read,s_wx_pcm_source;
#endif
#if TIRTC_BOARD_NT26F9D0
static int16_t s_wx_previous_sample;
static bool s_wx_previous_valid;

/* Expand backwards to reuse the decode buffer. The midpoint precedes the
 * current sample so consecutive blocks need no lookahead. */
static void wx_upsample_8k_to_16k(int16_t *pcm,uint32_t count,bool continuous,int16_t previous)
{
    while(count){
        const uint32_t index=--count;
        const int16_t current=pcm[index];
        const int16_t before=index?pcm[index-1U]:(continuous?previous:current);
        pcm[2U*index]=(int16_t)(((int32_t)before+(int32_t)current)/2);
        pcm[2U*index+1U]=current;
    }
}

static void wx_reset_upsample_locked(void)
{
    s_wx_previous_valid=false;
#if WX_PLAY_RETRY_CACHE
    s_wx_pcm_generation=0;
#endif
}
#endif
static uint32_t s_tail, s_prebuffer_at, s_partial_at;
static bool s_guard, s_continuing, s_prebuffer, s_partial;
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
static dev_playout_t s_wx_playout;
/* Keep an earned start across HAL BUSY retries without rebuffering. */
static bool s_wx_play_ready;
#endif
static uint8_t s_fraction;
static uint32_t s_tx_timestamp, s_pause_at;
static bool s_pause_seen;
static int s_error;
static bool s_tx_failing, s_record_failing;
static uint32_t s_tx_failure_at, s_record_failure_at;
static uint32_t s_last_log, s_play_failure_at;
static bool s_play_failing;
#if TIRTC_DEV_OPUS_ENABLE
/* One writer reserves a slot, copies outside critical, then commits only to
 * the same generation/epoch. Stop keeps ownership until all writers return. */
typedef struct {
    uint16_t length, samples;
    uint8_t media, flags, state;
    uint32_t generation, epoch, timestamp;
    uint8_t payload[CALLS_DEV_OPUS_MAX_PACKET];
} dev_rx_packet_t;
static dev_rx_packet_t s_dev_rx[DEV_RX_SLOTS];
static uint8_t s_dev_fifo[DEV_RX_SLOTS];
static uint32_t s_dev_head, s_dev_count, s_dev_samples, s_dev_producers, s_dev_epoch;
static int s_dev_pending_slot=-1;
static uint32_t s_dev_pending_samples;
static int16_t s_dev_pcm[CALLS_DEV_OPUS_MAX_DECODE_SAMPLES];
static bool s_dev_prebuffering=true;
static dev_playout_t s_dev_playout;
static calls_dev_opus_context_t s_dev_opus=CALLS_DEV_OPUS_CONTEXT_INIT;
static liot_task_t s_dev_capture_task;
static liot_sem_t s_dev_capture_wake;
#endif
/* DEV-only local counters; PCM is inspected without changing it.
 * Preserve bounded window resets without periodic serial output. */
typedef struct {
    uint32_t generation, started_at, reported_at;
    uint32_t record_try, record_ok, record_busy, record_error, record_stale;
    uint32_t record_max_ms, queue_ok, queue_fail;
    uint32_t tx_ok, tx_busy, tx_error, tx_stale, tx_max_ms;
    uint32_t pcm_samples, pcm_peak;
    uint64_t pcm_absolute;
    int record_ret, queue_ret, tx_ret;
} dev_audio_diagnostic_t;
static dev_audio_diagnostic_t s_dev_diagnostic;
enum failure_kind { FAIL_RECORD, FAIL_SEND, FAIL_PLAY, FAIL_CONTROL, FAIL_STOP, FAIL_START, FAIL_COUNT };
typedef struct {
    demo_tirtc_owner_e owner;
    uint32_t generation;
    uint32_t revision;
    TIRTCFRAMEINFO frame;
#if TIRTC_DEV_OPUS_ENABLE
    uint32_t enqueue_ms;
    uint8_t payload[CALLS_DEV_OPUS_MAX_PACKET];
#else
    uint8_t payload[CALL_NETWORK_SAMPLES_8K];
#endif
} call_tx_job_t;
/* Each slot has one task owner. Reset only before capture admission opens.
 * No diagnostic heap allocation and no heap query/printing under critical. */
static struct { uint32_t at; bool seen, terminal; } s_failure_log[FAIL_COUNT];
#ifndef TIRTC_CALL_MEDIA_DIAGNOSTICS
#define TIRTC_CALL_MEDIA_DIAGNOSTICS 0
#endif
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
#define AUDIO_MEDIA_REPORT_MS 10000U
#define AUDIO_FAILURE_REPEAT_MS AUDIO_MEDIA_REPORT_MS
#define AUDIO_MEDIA_ONLY(...) do { __VA_ARGS__ } while (0)
typedef struct {
    uint32_t generation, started_at, reported_at, lease_generation;
    uint32_t cap_try, cap_ok, cap_timeout, cap_busy, cap_fail, cap_stale, cap_max_ms;
    uint32_t encode_fail, txq_ok, txq_drop, tx_ok, tx_busy, tx_fail, tx_stale, tx_max_ms;
    uint32_t wx_tx_packets, wx_first_send_ms;
    uint32_t wx_last_pts, wx_pts_gap_count, wx_pts_gap_ms, wx_pts_max_ms, wx_pts_nonmono;
    uint32_t rx_in, rx_ok, rx_format, rx_muted, rx_full, rx_stale;
    uint32_t rx_pcm, full_pcm, discard_pcm, drop_pcm;
    uint32_t play_ok, play_busy, play_fail, play_pcm, rebuffer, starve;
#if WX_PLAY_RETRY_CACHE
    uint32_t wx_pcm_retries_saved;
#endif
} call_audio_media_t;
static call_audio_media_t s_audio_media;
#else
#define AUDIO_FAILURE_REPEAT_MS 1000U
#define AUDIO_MEDIA_ONLY(...) do { } while (0)
#endif
#if TIRTC_F9_CALL_AUDIO_QOS
/* One PCM frame is 20 ms. Two consecutive slow submissions indicate
 * sustained pressure rather than pausing video for an isolated slow call. */
#define AUDIO_TX_SLOW_MS 20U
#define AUDIO_TX_SLOW_PAIR_MS 80U
#define AUDIO_VIDEO_HOLD_MS 120U
static struct {
    bool holding, slow_seen;
    uint32_t hold_at, slow_at;
} s_audio_qos;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
static struct {
    uint32_t sdk_busy, txq_reject, slow_pairs;
} s_audio_qos_counts;
#endif
#endif
static uint32_t now(void){return liot_rtos_get_running_time();}
static bool before(uint32_t end){return (int32_t)(end-now())>0;}
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
/* Called only with the RTOS critical section held. PCM counters use mono
 * 16 kHz samples, excluding warmup and padding. HAL acceptance is not DMA
 * completion. Rebuffer/starve count playback state transitions. */
static uint32_t audio_media_pending_pcm_locked(void)
{
#if TIRTC_DEV_OPUS_ENABLE
    if(s_owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        uint32_t pending=s_dev_samples;
        if(s_dev_pending_slot>=0)pending+=s_dev_rx[s_dev_pending_slot].samples;
        return pending;
    }
#endif
    uint32_t pending=0;
    for(uint32_t i=0;i<s_segment_count;++i){
        const audio_segment_t *segment=&s_segments[(s_segment_head+i)%RX_SEGMENTS];
        pending+=segment->bytes*(segment->rate==8U?2U:1U);
    }
    return pending;
}

static void audio_media_report(demo_tirtc_owner_e owner,uint32_t generation,bool final)
{
    uint32_t stamp=now(),pending;
    call_audio_media_t report;
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
    dev_playout_t wx_playout;
#endif
#if TIRTC_F9_CALL_AUDIO_QOS
    uint32_t qos_sdk_busy,qos_txq_reject,qos_slow_pairs;
#endif
    liot_rtos_enter_critical();
    if(s_audio_media.generation!=generation||s_owner!=owner||
       (!final&&(uint32_t)(stamp-s_audio_media.reported_at)<AUDIO_MEDIA_REPORT_MS)){
        liot_rtos_exit_critical();return;
    }
    s_audio_media.reported_at=stamp;
    report=s_audio_media;
    pending=audio_media_pending_pcm_locked();
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
    wx_playout=s_wx_playout;
#endif
#if TIRTC_F9_CALL_AUDIO_QOS
    qos_sdk_busy=s_audio_qos_counts.sdk_busy;
    qos_txq_reject=s_audio_qos_counts.txq_reject;
    qos_slow_pairs=s_audio_qos_counts.slow_pairs;
#endif
    if(final)s_audio_media.generation=0;
    liot_rtos_exit_critical();
    liot_trace("[AV-AUDIO-TX] owner=%u gen=%lu final=%u ms=%lu cap=%lu ok=%lu timeout=%lu busy=%lu fail=%lu stale=%lu cap_max_ms=%lu enc_fail=%lu txq_ok=%lu txq_drop=%lu sdk_ok=%lu sdk_busy=%lu sdk_fail=%lu tx_stale=%lu sdk_max_ms=%lu\r\n",
        (unsigned)owner,(unsigned long)generation,final?1U:0U,(unsigned long)(stamp-report.started_at),
        (unsigned long)report.cap_try,(unsigned long)report.cap_ok,(unsigned long)report.cap_timeout,
        (unsigned long)report.cap_busy,(unsigned long)report.cap_fail,(unsigned long)report.cap_stale,
        (unsigned long)report.cap_max_ms,(unsigned long)report.encode_fail,(unsigned long)report.txq_ok,
        (unsigned long)report.txq_drop,(unsigned long)report.tx_ok,(unsigned long)report.tx_busy,
        (unsigned long)report.tx_fail,(unsigned long)report.tx_stale,(unsigned long)report.tx_max_ms);
    if(owner==DEMO_TIRTC_OWNER_WECHAT)
        liot_trace("[CALL-WX-TX] gen=%lu final=%u packets=%lu first_ms=%lu pts_gaps=%lu pts_missing_ms=%lu pts_max_ms=%lu pts_nonmono=%lu\r\n",
            (unsigned long)generation,(unsigned)final,(unsigned long)report.wx_tx_packets,
            (unsigned long)report.wx_first_send_ms,(unsigned long)report.wx_pts_gap_count,
            (unsigned long)report.wx_pts_gap_ms,(unsigned long)report.wx_pts_max_ms,
            (unsigned long)report.wx_pts_nonmono);
    liot_trace("[AV-AUDIO-RX] owner=%u gen=%lu final=%u in=%lu queued=%lu format=%lu muted=%lu full=%lu stale=%lu pcm16=%lu full_pcm16=%lu discard_pcm16=%lu drop_pcm16=%lu pending_pcm16=%lu\r\n",
        (unsigned)owner,(unsigned long)generation,final?1U:0U,
        (unsigned long)report.rx_in,(unsigned long)report.rx_ok,(unsigned long)report.rx_format,
        (unsigned long)report.rx_muted,(unsigned long)report.rx_full,(unsigned long)report.rx_stale,
        (unsigned long)report.rx_pcm,(unsigned long)report.full_pcm,(unsigned long)report.discard_pcm,
        (unsigned long)report.drop_pcm,(unsigned long)pending);
    liot_trace("[AV-AUDIO-PLAY] owner=%u gen=%lu final=%u hal_ok=%lu hal_busy=%lu hal_fail=%lu hal_pcm16=%lu rebuffer=%lu starve=%lu\r\n",
        (unsigned)owner,(unsigned long)generation,final?1U:0U,
        (unsigned long)report.play_ok,(unsigned long)report.play_busy,(unsigned long)report.play_fail,
        (unsigned long)report.play_pcm,(unsigned long)report.rebuffer,(unsigned long)report.starve);
#if WX_PLAY_RETRY_CACHE
    if(owner==DEMO_TIRTC_OWNER_WECHAT)
        liot_trace("[WX-AUDIO-PCM] gen=%lu final=%u retries_reused=%lu\r\n",
            (unsigned long)generation,final?1U:0U,(unsigned long)report.wx_pcm_retries_saved);
#endif
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
    if(owner==DEMO_TIRTC_OWNER_WECHAT)
        liot_trace("[WX-AUDIO-PLAYOUT] gen=%lu final=%u lease_gen=%lu target_ms=%lu min_ms=%lu peak_ms=%lu underrun=%lu timing_reset=%lu\r\n",
            (unsigned long)generation,final?1U:0U,(unsigned long)report.lease_generation,
            (unsigned long)wx_playout.target_ms,
            (unsigned long)wx_playout.min_target_ms,(unsigned long)wx_playout.peak_target_ms,
            (unsigned long)wx_playout.underruns,(unsigned long)wx_playout.timing_resets);
#endif
#if TIRTC_F9_CALL_AUDIO_QOS
    if(owner==DEMO_TIRTC_OWNER_WECHAT||owner==DEMO_TIRTC_OWNER_DEV_CHAT)
        liot_trace("[AV-AUDIO-QOS] owner=%u gen=%lu final=%u sdk_busy=%lu txq_reject=%lu slow_pairs=%lu\r\n",
            (unsigned)owner,(unsigned long)generation,final?1U:0U,
            (unsigned long)qos_sdk_busy,(unsigned long)qos_txq_reject,(unsigned long)qos_slow_pairs);
#endif
}
#endif
static void audio_error(void){liot_rtos_enter_critical();s_error=AUDIO_ERROR;liot_rtos_exit_critical();}
static void failure_log(enum failure_kind kind,const char *reason,int result,uint32_t duration,bool terminal)
{
    uint32_t stamp=now(),generation;size_t free_bytes,max_block;
    if(s_failure_log[kind].terminal)return;
    if(!terminal&&s_failure_log[kind].seen&&(uint32_t)(stamp-s_failure_log[kind].at)<AUDIO_FAILURE_REPEAT_MS)return;
    s_failure_log[kind].seen=true;s_failure_log[kind].terminal=terminal;s_failure_log[kind].at=stamp;
    liot_rtos_enter_critical();generation=s_generation;liot_rtos_exit_critical();
    free_bytes=liot_xPortGetFreeHeapSize();max_block=liot_xPortGetMaximumFreeBlockSize();
    liot_trace("[CALL24-AUDIO] failure reason=%s ret=%d duration_ms=%lu terminal=%u gen=%lu heap=%lu max_block=%lu\r\n",
        reason,result,(unsigned long)duration,terminal?1U:0U,(unsigned long)generation,
        (unsigned long)free_bytes,(unsigned long)max_block);
}
static int fatal_error(enum failure_kind kind,const char *reason,int result,uint32_t duration)
{
    failure_log(kind,reason,result,duration,true);audio_error();return AUDIO_ERROR;
}
static bool audio_session_locked(demo_tirtc_owner_e owner,uint32_t generation)
{return s_owned&&generation&&s_owner==owner&&s_generation==generation;}
static bool speaker_allowed_locked(void)
{return s_speaker&&s_volume&&s_session_speaker;}
static bool microphone_allowed_locked(void)
{return s_microphone&&s_mic&&s_session_microphone&&s_uplink_subscribed;}
#if TIRTC_F9_CALL_AUDIO_QOS
/* All state access uses the audio session critical section. */
static bool audio_qos_current_locked(demo_tirtc_owner_e owner,uint32_t generation)
{
    return (owner==DEMO_TIRTC_OWNER_WECHAT||owner==DEMO_TIRTC_OWNER_DEV_CHAT)&&
           s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&
           s_applied_revision==s_config_revision;
}

static void audio_qos_hold_locked(uint32_t stamp)
{
    s_audio_qos.hold_at=stamp;
    s_audio_qos.holding=true;
}

static void audio_qos_sent_locked(demo_tirtc_owner_e owner,uint32_t generation,
                                  int result,uint32_t duration,uint32_t stamp)
{
    if(!audio_qos_current_locked(owner,generation))return;
    if(result==TIRTC_E_BUSY){
        audio_qos_hold_locked(stamp);
        AUDIO_MEDIA_ONLY(++s_audio_qos_counts.sdk_busy;);
    }
    const bool slow=result>0&&duration>=AUDIO_TX_SLOW_MS;
    if(slow&&s_audio_qos.slow_seen&&stamp-s_audio_qos.slow_at<=AUDIO_TX_SLOW_PAIR_MS){
        audio_qos_hold_locked(stamp);
        AUDIO_MEDIA_ONLY(++s_audio_qos_counts.slow_pairs;);
    }
    s_audio_qos.slow_seen=slow;
    if(slow)s_audio_qos.slow_at=stamp;
}

bool calls_audio_video_should_defer(demo_tirtc_owner_e owner,uint32_t generation)
{
    bool defer=false;
    liot_rtos_enter_critical();
    if(audio_qos_current_locked(owner,generation)&&s_audio_qos.holding){
        defer=now()-s_audio_qos.hold_at<AUDIO_VIDEO_HOLD_MS;
        if(!defer)s_audio_qos.holding=false;
    }
    liot_rtos_exit_critical();
    return defer;
}
#endif
/* Honour the selected level in DEV too, as Lite does. Mute admission is
 * checked separately; do not replace every nonzero setting with L7. */
static uint8_t effective_mic_level_locked(void)
{return s_mic;}
static void dev_diagnostic_record(demo_tirtc_owner_e owner,uint32_t generation,
                                  int result,uint32_t duration,bool current)
{
    uint32_t i,peak=0;uint64_t absolute=0;
    if(owner!=DEMO_TIRTC_OWNER_DEV_CHAT)return;
    if(!result)for(i=0;i<DEMO_AI_AUDIO_FRAME_SAMPLES;i++){
        int32_t sample=s_capture[i];uint32_t magnitude=(uint32_t)(sample<0?-sample:sample);
        absolute+=magnitude;if(magnitude>peak)peak=magnitude;
    }
    liot_rtos_enter_critical();
    if(s_dev_diagnostic.generation==generation){
        s_dev_diagnostic.record_ret=result;
        if(duration>s_dev_diagnostic.record_max_ms)s_dev_diagnostic.record_max_ms=duration;
        if(!result){
            s_dev_diagnostic.record_ok++;
            if(!current)s_dev_diagnostic.record_stale++;
            s_dev_diagnostic.pcm_samples+=DEMO_AI_AUDIO_FRAME_SAMPLES;
            s_dev_diagnostic.pcm_absolute+=absolute;
            if(peak>s_dev_diagnostic.pcm_peak)s_dev_diagnostic.pcm_peak=peak;
        }else if(result==DEMO_AI_AUDIO_ERR_BUSY)s_dev_diagnostic.record_busy++;
        else s_dev_diagnostic.record_error++;
    }
    liot_rtos_exit_critical();
}
/* Caller holds critical. A stale job cannot contaminate a newer session. */
static void dev_diagnostic_tx_locked(const call_tx_job_t *job,bool attempted,
                                     bool current,int result,uint32_t duration)
{
    if(job->owner!=DEMO_TIRTC_OWNER_DEV_CHAT||s_dev_diagnostic.generation!=job->generation)return;
    if(!current)s_dev_diagnostic.tx_stale++;
    if(!attempted)return;
    s_dev_diagnostic.tx_ret=result;
    if(duration>s_dev_diagnostic.tx_max_ms)s_dev_diagnostic.tx_max_ms=duration;
    if(result>0)s_dev_diagnostic.tx_ok++;
    else if(result==TIRTC_E_BUSY)s_dev_diagnostic.tx_busy++;
    else s_dev_diagnostic.tx_error++;
}
static void dev_diagnostic_report(demo_tirtc_owner_e owner,uint32_t generation,bool final)
{
    uint32_t stamp=now();
    if(owner!=DEMO_TIRTC_OWNER_DEV_CHAT)return;
    liot_rtos_enter_critical();
    if(s_dev_diagnostic.generation!=generation||
       (!final&&(uint32_t)(stamp-s_dev_diagnostic.reported_at)<5000U)){
        liot_rtos_exit_critical();return;
    }
    s_dev_diagnostic.reported_at=stamp;
    s_dev_diagnostic.pcm_samples=s_dev_diagnostic.pcm_peak=0;s_dev_diagnostic.pcm_absolute=0;
    liot_rtos_exit_critical();
}
static void revise_config_locked(void)
{
    if(!++s_config_revision)++s_config_revision;
#if TIRTC_F9_CALL_AUDIO_QOS
    s_audio_qos.holding=s_audio_qos.slow_seen=false;
#endif
}
#if TIRTC_DEV_OPUS_ENABLE
/* Caller holds critical; reserved/copying slots are never reused here. */
static void dev_discard_rx_locked(void)
{
    uint32_t i;
    if(!++s_dev_epoch)++s_dev_epoch;
    for(i=0;i<DEV_RX_SLOTS;i++)if(s_dev_rx[i].state!=1U)s_dev_rx[i].state=0;
    s_dev_head=s_dev_count=s_dev_samples=0;
    s_dev_pending_slot=-1;s_dev_pending_samples=0;s_dev_prebuffering=true;
    dev_playout_init(&s_dev_playout);
}
static void dev_receive(uint32_t generation,const TIRTCFRAMEINFO *frame,const void *data)
{
    int samples=0;uint32_t i,epoch,revision,arrival=now();int slot=-1;
    dev_rx_packet_t *packet;
    /* Inspect packet framing only; no codec state, allocation or HAL work. */
    if(frame&&data&&frame->stream_id==10U&&frame->length&&frame->length<=CALLS_DEV_OPUS_MAX_PACKET){
        if(frame->media==TIRTC_AUDIO_OPUS&&frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C)
            samples=calls_dev_opus_packet_samples(data,(int)frame->length);
        else if(frame->media==TIRTC_AUDIO_ALAW&&frame->length<=DEV_ALAW_MAX_BYTES&&
                (frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C||frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C))
            samples=(int)frame->length*(frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C?2:1);
    }
    liot_rtos_enter_critical();
    if(!audio_session_locked(DEMO_TIRTC_OWNER_DEV_CHAT,generation)||!s_running){
        liot_rtos_exit_critical();return;
    }
    AUDIO_MEDIA_ONLY(++s_audio_media.rx_in;);
    if(!speaker_allowed_locked()||s_mute_pending||s_draining){
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_muted;);
        liot_rtos_exit_critical();return;
    }
    if(samples<=0||samples>CALLS_DEV_OPUS_MAX_DECODE_SAMPLES){
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_format;);
        ++s_drops;liot_rtos_exit_critical();return;
    }
    for(i=0;i<DEV_RX_SLOTS;i++)if(!s_dev_rx[i].state){slot=(int)i;break;}
    if(slot<0){
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_full;s_audio_media.full_pcm+=(uint32_t)samples;);
        ++s_drops;liot_rtos_exit_critical();return;
    }
    packet=&s_dev_rx[slot];packet->state=1U;++s_dev_producers;
    epoch=s_dev_epoch;revision=s_config_revision;
    liot_rtos_exit_critical();
    packet->length=(uint16_t)frame->length;packet->samples=(uint16_t)samples;
    packet->media=frame->media;packet->flags=frame->flags;
    packet->generation=generation;packet->epoch=epoch;packet->timestamp=frame->ts;
    memcpy(packet->payload,data,frame->length);
    liot_rtos_enter_critical();
    if(audio_session_locked(DEMO_TIRTC_OWNER_DEV_CHAT,generation)&&s_running&&
       speaker_allowed_locked()&&!s_mute_pending&&!s_draining&&
       s_dev_epoch==epoch&&s_config_revision==revision){
        packet->state=2U;s_dev_fifo[(s_dev_head+s_dev_count)%DEV_RX_SLOTS]=(uint8_t)slot;
        ++s_dev_count;s_dev_samples+=(uint32_t)samples;
        if(s_dev_count>s_peak)s_peak=s_dev_count;
        ++s_received;s_received_bytes+=frame->length;
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_ok;s_audio_media.rx_pcm+=(uint32_t)samples;);
        dev_playout_observe(&s_dev_playout,packet->timestamp,((uint32_t)samples+15U)/16U,arrival);
    }else {
        packet->state=0;
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_stale;);
    }
    --s_dev_producers;
    liot_rtos_exit_critical();
}
static int dev_play_one(uint32_t generation)
{
    dev_rx_packet_t *packet;uint32_t i,samples,duration,queued;int slot,result;bool current;
#if !DEMO_AI_AUDIO_AEC_ENABLE
    if(!demo_ai_audio_play_done(&s_lease))return 0;
#endif
    liot_rtos_enter_critical();slot=s_dev_pending_slot;
    if(slot<0&&s_dev_count){
        slot=(int)s_dev_fifo[s_dev_head];s_dev_head=(s_dev_head+1U)%DEV_RX_SLOTS;--s_dev_count;
        s_dev_samples-=s_dev_rx[slot].samples;s_dev_rx[slot].state=3U;s_dev_pending_slot=slot;
    }
    if(slot<0){liot_rtos_exit_critical();return 0;}
    packet=&s_dev_rx[slot];
    current=audio_session_locked(DEMO_TIRTC_OWNER_DEV_CHAT,generation)&&s_running&&
            speaker_allowed_locked()&&!s_mute_pending&&!s_draining&&
            packet->generation==generation&&packet->epoch==s_dev_epoch;
    liot_rtos_exit_critical();
    if(!current)goto dropped;
    samples=packet->samples;
    duration=(samples+15U)/16U;
    queued=s_guard&&before(s_tail)?s_tail-now():0;
    if(queued&&queued+duration+1U>PLAY_AHEAD)return 0;
    if(!s_dev_pending_samples){
        if(packet->media==TIRTC_AUDIO_OPUS){
            result=calls_dev_opus_decode(&s_dev_opus,packet->payload,packet->length,
                                         s_dev_pcm,CALLS_DEV_OPUS_MAX_DECODE_SAMPLES);
            if(result!=(int)samples)goto dropped;
        }else if(packet->flags==TIRTC_AUDIOSAMPLE_8K16B1C){
            for(i=0;i<packet->length;i++){
                int16_t value=demo_g711_alaw_decode_sample(packet->payload[i]);
                s_dev_pcm[i*2U]=value;s_dev_pcm[i*2U+1U]=value;
            }
        }else if(demo_g711_alaw_decode(packet->payload,s_dev_pcm,packet->length)!=packet->length)goto dropped;
        /* Preserve the last sample if a legacy 16k packet has an odd length;
         * the I2S HAL consumes whole pairs. No silent preamble is inserted. */
        if(samples&1U)s_dev_pcm[samples]=s_dev_pcm[samples-1U];
        s_dev_pending_samples=samples;
    }
    liot_rtos_enter_critical();
    current=audio_session_locked(DEMO_TIRTC_OWNER_DEV_CHAT,generation)&&s_running&&
            speaker_allowed_locked()&&!s_mute_pending&&!s_draining&&packet->epoch==s_dev_epoch;
    liot_rtos_exit_critical();
    if(!current)goto dropped;
    result=demo_ai_audio_play(&s_lease,s_dev_pcm,samples+(samples&1U));
    /* Stateful Opus decode runs once; a BUSY retry owns the identical PCM. */
    AUDIO_MEDIA_ONLY(
        liot_rtos_enter_critical();
        if(result==DEMO_AI_AUDIO_ERR_PLAY_BUSY)++s_audio_media.play_busy;
        else if(result)++s_audio_media.play_fail;
        else {++s_audio_media.play_ok;s_audio_media.play_pcm+=samples;}
        liot_rtos_exit_critical();
    );
    if(result==DEMO_AI_AUDIO_ERR_PLAY_BUSY)return 0;
    if(result){
        if(!s_play_failing){s_play_failing=true;s_play_failure_at=now();}
        if((uint32_t)(now()-s_play_failure_at)>=200U)
            (void)fatal_error(FAIL_PLAY,"dev_play",result,now()-s_play_failure_at);
        else failure_log(FAIL_PLAY,"dev_play",result,now()-s_play_failure_at,false);
        goto dropped;
    }
    if(!s_guard||!before(s_tail))s_tail=now();
    s_tail+=duration;s_guard=s_continuing=true;s_play_failing=false;++s_played;
    liot_rtos_enter_critical();packet->state=0;s_dev_pending_slot=-1;s_dev_pending_samples=0;liot_rtos_exit_critical();
    return 1;
dropped:
    liot_rtos_enter_critical();
    AUDIO_MEDIA_ONLY(s_audio_media.drop_pcm+=packet->samples;);
    ++s_drops;packet->state=0;s_dev_pending_slot=-1;s_dev_pending_samples=0;liot_rtos_exit_critical();
    return 1;
}
static void dev_service_downlink(uint32_t generation)
{
    uint32_t queued,samples,started,i;bool ready=true;
    bool done=demo_ai_audio_play_done(&s_lease);
    liot_rtos_enter_critical();queued=s_dev_count;samples=s_dev_samples;
    if(s_dev_pending_slot>=0){++queued;samples+=s_dev_rx[s_dev_pending_slot].samples;}
    if(done&&!s_dev_prebuffering&&!s_dev_pending_samples
#if !DEMO_AI_AUDIO_AEC_ENABLE
       &&(queued==0U||s_played==0U)
#endif
      ){
        s_dev_prebuffering=true;
        if(s_played){
            dev_playout_underrun(&s_dev_playout);
            AUDIO_MEDIA_ONLY(++s_audio_media.rebuffer;if(!queued)++s_audio_media.starve;);
        }
    }
    if(s_dev_prebuffering)ready=dev_playout_should_start(&s_dev_playout,(samples+15U)/16U,queued,DEV_RX_SLOTS,now());
    if(ready&&queued){s_dev_prebuffering=false;dev_playout_started(&s_dev_playout);}
    liot_rtos_exit_critical();
    if(!ready||!queued)return;
    started=now();for(i=0;i<DEV_PLAY_BATCH_MAX;i++){
        if(dev_play_one(generation)<=0||(uint32_t)(now()-started)>=20U)break;
    }
}
#endif
static void discard_rx(void)
{
    liot_rtos_enter_critical();
    AUDIO_MEDIA_ONLY(if(s_audio_media.generation)s_audio_media.discard_pcm+=audio_media_pending_pcm_locked(););
    s_read=s_count=0;s_segment_head=s_segment_count=0;
#if TIRTC_BOARD_NT26F9D0
    wx_reset_upsample_locked();
#elif WX_PLAY_RETRY_CACHE
    s_wx_pcm_generation=0;
#endif
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
    if(s_owner==DEMO_TIRTC_OWNER_WECHAT){
        dev_playout_init(&s_wx_playout);
        s_wx_playout.recover_from_arrival_gap=true;
        s_wx_play_ready=false;
    }
#endif
#if TIRTC_DEV_OPUS_ENABLE
    dev_discard_rx_locked();
#endif
    liot_rtos_exit_critical();s_prebuffer=s_partial=s_continuing=false;
}
/* Caller holds the critical section. Used only after copied audio was accepted
 * by playback, or after an incomplete source sample has timed out. */
static void consume_rx_locked(uint32_t bytes)
{
#if WX_PLAY_RETRY_CACHE
    s_wx_pcm_generation=0;
#endif
    s_read=(s_read+bytes)%RX_BYTES;s_count-=bytes;
    s_segments[s_segment_head].bytes=(uint16_t)(s_segments[s_segment_head].bytes-bytes);
    if(!s_segments[s_segment_head].bytes){s_segment_head=(s_segment_head+1U)%RX_SEGMENTS;s_segment_count--;}
}
void calls_audio_set_config(uint8_t volume,uint8_t mic,bool speaker,bool microphone)
{
    bool was_speaker;
    if(volume>10)volume=10;
    if(mic>10)mic=10;
    liot_rtos_enter_critical();was_speaker=speaker_allowed_locked();
    if(s_volume!=volume||s_mic!=mic||s_speaker!=speaker||s_microphone!=microphone){
        s_volume=volume;s_mic=mic;s_speaker=speaker;s_microphone=microphone;
        revise_config_locked();
    }
    if(was_speaker&&!speaker_allowed_locked())s_mute_pending=true;
    liot_rtos_exit_critical();
}
int calls_audio_set_session_controls(demo_tirtc_owner_e owner,uint32_t generation,
                                     bool speaker,bool microphone,bool uplink_subscribed)
{
    bool was_speaker;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)){liot_rtos_exit_critical();return DEMO_AI_AUDIO_ERR_INVALID_LEASE;}
    was_speaker=speaker_allowed_locked();
    if(s_session_speaker!=speaker||s_session_microphone!=microphone||s_uplink_subscribed!=uplink_subscribed){
        s_session_speaker=speaker;s_session_microphone=microphone;s_uplink_subscribed=uplink_subscribed;
        revise_config_locked();
    }
    if(was_speaker&&!speaker_allowed_locked())s_mute_pending=true;
    liot_rtos_exit_critical();return 0;
}
bool calls_audio_busy(void){bool busy;liot_rtos_enter_critical();busy=s_owned;liot_rtos_exit_critical();return busy;}
#if TIRTC_F9_AV_FOCUS
typedef struct {
    bool raised;
    uint8_t saved;
} calls_audio_priority_t;

static calls_audio_priority_t s_tx_priority;
#if TIRTC_DEV_OPUS_ENABLE && DEMO_AI_AUDIO_AEC_ENABLE
static calls_audio_priority_t s_capture_priority;
#endif

static bool set_focus_priority(liot_task_t task, calls_audio_priority_t *priority, bool enabled)
{
    uint8_t previous;
    if (priority->raised == enabled) return true;
    if (!task || liot_rtos_task_change_priority(task,
            enabled ? TIRTC_F9_AV_AUDIO_PRIORITY : priority->saved, &previous) != LIOT_OSI_SUCCESS)
        return false;
    if (enabled) priority->saved = previous;
    priority->raised = enabled;
    return true;
}

static bool set_media_focus(bool enabled)
{
    bool tx = set_focus_priority(s_tx_task, &s_tx_priority, enabled);
#if TIRTC_DEV_OPUS_ENABLE && DEMO_AI_AUDIO_AEC_ENABLE
    bool capture = set_focus_priority(s_dev_capture_task, &s_capture_priority,
        enabled && (s_owner==DEMO_TIRTC_OWNER_DEV_CHAT ||
                    (TIRTC_F9_WX_CAPTURE_RING && s_owner==DEMO_TIRTC_OWNER_WECHAT)));
    return tx && capture;
#else
    return tx;
#endif
}

bool calls_audio_set_av_focus(bool enabled)
{
    if (!enabled) return set_media_focus(false);
    if (!s_owned || !s_running ||
        (s_owner != DEMO_TIRTC_OWNER_DEV_CHAT && s_owner != DEMO_TIRTC_OWNER_WECHAT))
        return false;
    bool ready = set_media_focus(true);
#if DEMO_AI_AUDIO_AEC_ENABLE
    if (ready) ready = demo_ai_audio_set_av_focus(&s_lease, true) == 0;
#endif
    if (!ready) (void)set_media_focus(false);
    return ready;
}
#endif
static bool tx_job_current_locked(const call_tx_job_t *job)
{
    return s_running && audio_session_locked(job->owner,job->generation) &&
           microphone_allowed_locked() && s_config_revision==job->revision;
}
static void tx_send_job(const call_tx_job_t *job)
{
    bool current,first=false;
    int result;uint32_t started,duration;
    liot_rtos_enter_critical();
    current=tx_job_current_locked(job);
#if TIRTC_DEV_OPUS_ENABLE
    if(current&&job->owner==DEMO_TIRTC_OWNER_DEV_CHAT&&(uint32_t)(now()-job->enqueue_ms)>DEV_TX_MAX_AGE_MS){
        current=false;
    }
#endif
    s_tx_busy=current;
    if(!current){
        dev_diagnostic_tx_locked(job,false,false,0,0);
        AUDIO_MEDIA_ONLY(if(s_audio_media.generation==job->generation&&s_owner==job->owner)++s_audio_media.tx_stale;);
    }
    liot_rtos_exit_critical();
    if(!current)return;
    started=now();result=demo_tirtc_send_audio(job->owner,job->generation,&job->frame,job->payload);duration=now()-started;
    liot_rtos_enter_critical();
    current=tx_job_current_locked(job);
#if TIRTC_F9_CALL_AUDIO_QOS
    if(current)audio_qos_sent_locked(job->owner,job->generation,result,duration,started+duration);
#endif
    dev_diagnostic_tx_locked(job,true,current,result,duration);
    AUDIO_MEDIA_ONLY(
        if(s_audio_media.generation==job->generation&&s_owner==job->owner){
            if(result>0){
                ++s_audio_media.tx_ok;
                if(job->owner==DEMO_TIRTC_OWNER_WECHAT){
                    const uint32_t pts=job->frame.ts;
                    if(s_audio_media.wx_tx_packets++){
                        const int32_t elapsed=(int32_t)(pts-s_audio_media.wx_last_pts);
                        if(elapsed<=0)++s_audio_media.wx_pts_nonmono;
                        else {
                            if(elapsed>22){
                                ++s_audio_media.wx_pts_gap_count;
                                s_audio_media.wx_pts_gap_ms+=(uint32_t)elapsed-20U;
                            }
                            if((uint32_t)elapsed>s_audio_media.wx_pts_max_ms) s_audio_media.wx_pts_max_ms=(uint32_t)elapsed;
                        }
                    }else s_audio_media.wx_first_send_ms=started-s_audio_media.started_at;
                    s_audio_media.wx_last_pts=pts;
                }
            }
            else if(result==TIRTC_E_BUSY)++s_audio_media.tx_busy;
            else ++s_audio_media.tx_fail;
            if(duration>s_audio_media.tx_max_ms)s_audio_media.tx_max_ms=duration;
        }
    );
    liot_rtos_exit_critical();
    /* Keep stop/release fenced until this job has finished updating its
     * session state, including a terminal error raised after logging. */
    if(!current)goto finished;
    if(result>0){
        ++s_sent; s_sent_bytes+=job->frame.length; s_tx_failing=false;
        if(!s_first_tx_logged){s_first_tx_logged=true;first=true;}
    }else if(result==TIRTC_E_BUSY){
        s_tx_failing=false;
    }else{
        if(!s_tx_failing){s_tx_failing=true;s_tx_failure_at=now();}
        if((uint32_t)(now()-s_tx_failure_at)>=3000U)
            (void)fatal_error(FAIL_SEND,"send",result,now()-s_tx_failure_at);
        else failure_log(FAIL_SEND,"send",result,now()-s_tx_failure_at,false);
    }
#if TIRTC_DEV_OPUS_ENABLE
    if(first&&job->owner==DEMO_TIRTC_OWNER_DEV_CHAT)
        TIRTC_LOG_DEBUG("[AUDIO42] first_tx owner=%u gen=%lu stream=10 OPUS rate=16000 bytes=%lu\r\n",
            (unsigned)job->owner,(unsigned long)job->generation,(unsigned long)job->frame.length);
    else
#endif
    if(first)TIRTC_LOG_DEBUG("[AUDIO42] first_tx owner=%u gen=%lu stream=%u rate=8000 bytes=%lu\r\n",
        (unsigned)job->owner,(unsigned long)job->generation,(unsigned)job->frame.stream_id,
        (unsigned long)job->frame.length);
finished:
    liot_rtos_enter_critical();s_tx_busy=false;liot_rtos_exit_critical();
}
static void tx_worker(void *arg)
{
    call_tx_job_t job;
    (void)arg;
    for(;;)if(liot_rtos_queue_wait(s_tx_queue,(uint8_t *)&job,sizeof(job),LIOT_WAIT_FOREVER)==LIOT_OSI_SUCCESS)tx_send_job(&job);
}
static void capture_once(
#if TIRTC_DEV_OPUS_ENABLE
    demo_tirtc_owner_e required_owner
#else
    void
#endif
)
{
    demo_ai_audio_lease_t lease; demo_tirtc_owner_e owner=DEMO_TIRTC_OWNER_NONE; uint32_t generation=0, timestamp=0, revision=0;
    bool allowed, current; int result; call_tx_job_t job;uint32_t started,duration;
    liot_rtos_enter_critical();
    allowed=s_running&&s_owned&&!s_error&&!s_record_busy&&microphone_allowed_locked()&&s_config_revision==s_applied_revision;
#if TIRTC_DEV_OPUS_ENABLE
    allowed=allowed&&(required_owner==DEMO_TIRTC_OWNER_NONE||s_owner==required_owner);
#endif
    if(allowed){
        s_record_busy=true;lease=s_lease;owner=s_owner;generation=s_generation;revision=s_config_revision;
        if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)s_dev_diagnostic.record_try++;
        AUDIO_MEDIA_ONLY(++s_audio_media.cap_try;);
        if(s_pause_seen){s_tx_timestamp+=now()-s_pause_at;s_pause_seen=false;}
        timestamp=s_tx_timestamp;s_tx_timestamp+=20U;
    }else if(!s_pause_seen){s_pause_seen=true;s_pause_at=now();}
    liot_rtos_exit_critical();
    if(!allowed){s_tx_failing=s_record_failing=false;liot_rtos_task_sleep_ms(5);return;}
    started=now();
#if DEMO_AI_AUDIO_AEC_ENABLE && TIRTC_F9_WX_CAPTURE_RING
    uint32_t capture_ms=0U;
    const bool wx_capture=owner==DEMO_TIRTC_OWNER_WECHAT;
    if(wx_capture)result=demo_ai_audio_record_wechat_20ms(&lease,s_capture,&capture_ms);
    else
#endif
    result=demo_ai_audio_record_20ms(&lease,s_capture);
    duration=now()-started;
#if DEMO_AI_AUDIO_AEC_ENABLE
    /* Fresh AEC PCM follows wall time; a timeout or scheduling gap must not
     * compress the audio timeline relative to video capture timestamps. */
    if(!result)timestamp=started+duration;
#if TIRTC_F9_WX_CAPTURE_RING
    if(!result && wx_capture)timestamp=capture_ms;
#endif
#endif
    liot_rtos_enter_critical();
    current=s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&s_config_revision==revision;
    AUDIO_MEDIA_ONLY(
        if(!result){++s_audio_media.cap_ok;if(!current)++s_audio_media.cap_stale;}
        else if(result==DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT)++s_audio_media.cap_timeout;
        else if(result==DEMO_AI_AUDIO_ERR_BUSY)++s_audio_media.cap_busy;
        else ++s_audio_media.cap_fail;
        if(duration>s_audio_media.cap_max_ms)s_audio_media.cap_max_ms=duration;
    );
    liot_rtos_exit_critical();
    dev_diagnostic_record(owner,generation,result,duration,current);
    if(result && result!=DEMO_AI_AUDIO_ERR_BUSY){
        if(!s_record_failing){s_record_failing=true;s_record_failure_at=now();}
        if((uint32_t)(now()-s_record_failure_at)>=3000U)
            (void)fatal_error(FAIL_RECORD,"record",result,now()-s_record_failure_at);
        else failure_log(FAIL_RECORD,"record",result,now()-s_record_failure_at,false);
    }else if(!result){
        s_record_failing=false;
        if(current){
#if TIRTC_DEV_OPUS_ENABLE
            if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
                int bytes;
                memset(&job,0,sizeof(job));job.owner=owner;job.generation=generation;job.revision=revision;
                bytes=calls_dev_opus_encode_20ms(&s_dev_opus,s_capture,job.payload,sizeof(job.payload));
                if(bytes<=0){
                    (void)fatal_error(FAIL_RECORD,"opus_encode",bytes,0);
                    liot_rtos_enter_critical();
                    AUDIO_MEDIA_ONLY(++s_audio_media.encode_fail;);
                    s_record_busy=false;liot_rtos_exit_critical();return;
                }
                job.frame.stream_id=10U;job.frame.media=TIRTC_AUDIO_OPUS;
                job.frame.flags=TIRTC_AUDIOSAMPLE_16K16B1C;job.frame.ts=timestamp;job.frame.length=(uint32_t)bytes;
                job.enqueue_ms=now();
            }else {
#endif
            {
                uint32_t sample;
                for (sample=0U; sample<CALL_NETWORK_SAMPLES_8K; ++sample) {
                    int32_t mixed=(int32_t)s_capture[sample*2U]+(int32_t)s_capture[sample*2U+1U];
                    s_capture_alaw[sample]=demo_g711_alaw_encode_sample((int16_t)(mixed/2));
                }
            }
            memset(&job,0,sizeof(job));job.owner=owner;job.generation=generation;job.revision=revision;
            memcpy(job.payload,s_capture_alaw,sizeof(s_capture_alaw));
            job.frame.stream_id=owner==DEMO_TIRTC_OWNER_WECHAT?0U:10U;job.frame.media=TIRTC_AUDIO_ALAW;
            job.frame.flags=TIRTC_AUDIOSAMPLE_8K16B1C;job.frame.ts=timestamp;job.frame.length=CALL_NETWORK_SAMPLES_8K;
#if TIRTC_DEV_OPUS_ENABLE
            }
#endif
            liot_rtos_enter_critical();
            current=s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&s_config_revision==revision;
            liot_rtos_exit_critical();
            if(!current){s_tx_failing=false;liot_rtos_enter_critical();
                if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)s_dev_diagnostic.record_stale++;
                AUDIO_MEDIA_ONLY(++s_audio_media.cap_stale;);
                s_record_busy=false;liot_rtos_exit_critical();return;}
            {
                int queue_result=liot_rtos_queue_release(s_tx_queue,sizeof(job),(uint8_t *)&job,LIOT_NO_WAIT);
                if(owner==DEMO_TIRTC_OWNER_DEV_CHAT||TIRTC_CALL_MEDIA_DIAGNOSTICS
#if TIRTC_F9_CALL_AUDIO_QOS
                   ||queue_result!=LIOT_OSI_SUCCESS
#endif
                ){
                    liot_rtos_enter_critical();
#if TIRTC_F9_CALL_AUDIO_QOS
                    if(queue_result!=LIOT_OSI_SUCCESS&&tx_job_current_locked(&job)&&
                       audio_qos_current_locked(owner,generation)){
                        audio_qos_hold_locked(now());
                        AUDIO_MEDIA_ONLY(++s_audio_qos_counts.txq_reject;);
                    }
#endif
                    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
                        s_dev_diagnostic.queue_ret=queue_result;
                        if(queue_result==LIOT_OSI_SUCCESS)s_dev_diagnostic.queue_ok++;else s_dev_diagnostic.queue_fail++;
                    }
                    AUDIO_MEDIA_ONLY(
                        if(queue_result==LIOT_OSI_SUCCESS)++s_audio_media.txq_ok;
                        else ++s_audio_media.txq_drop;
                    );
                    liot_rtos_exit_critical();
                }
                if(queue_result!=LIOT_OSI_SUCCESS)s_tx_failing=false;
                result=queue_result==LIOT_OSI_SUCCESS ? 1 : -1;
            }
        }else s_tx_failing=false;
    }
    liot_rtos_enter_critical();s_record_busy=false;liot_rtos_exit_critical();
    if(result<=0)liot_rtos_task_sleep_ms(5);
}
#if TIRTC_DEV_OPUS_ENABLE
static void dev_capture_worker(void *arg)
{
    (void)arg;
    for(;;){
        demo_tirtc_owner_e owner=DEMO_TIRTC_OWNER_NONE;
#if DEMO_AI_AUDIO_AEC_ENABLE
        liot_rtos_enter_critical();
        if(s_running && s_owned && (s_owner==DEMO_TIRTC_OWNER_DEV_CHAT ||
                    (TIRTC_F9_WX_CAPTURE_RING && s_owner==DEMO_TIRTC_OWNER_WECHAT))) owner=s_owner;
        liot_rtos_exit_critical();
#endif
        /* Ordinary HAL capture remains serialized with playback. */
        if(owner==DEMO_TIRTC_OWNER_NONE){(void)liot_rtos_semaphore_wait(s_dev_capture_wake,LIOT_WAIT_FOREVER);continue;}
        capture_once(owner);liot_rtos_task_sleep_ms(1U);
    }
}
#endif
int calls_audio_start_service(void)
{
#if TIRTC_DEV_OPUS_ENABLE
    liot_task_t created=NULL;int result=0;
    liot_rtos_enter_critical();
    if(s_creating){liot_rtos_exit_critical();return DEMO_AI_AUDIO_ERR_BUSY;}
    if(s_tx_task&&s_dev_capture_task){liot_rtos_exit_critical();return 0;}
    s_creating=true;liot_rtos_exit_critical();
    if(!s_tx_queue)result=(int)liot_rtos_queue_create(&s_tx_queue,sizeof(call_tx_job_t),CALL_TX_QUEUE_DEPTH);
    if(!result&&!s_dev_capture_wake)result=(int)liot_rtos_semaphore_create(&s_dev_capture_wake,0U);
    if(!result&&!s_tx_task){
        result=(int)liot_rtos_task_create(&created,CALL_TX_STACK,11,"call_tx",tx_worker,NULL);
        if(!result&&!created)result=-1;
        if(!result){liot_rtos_enter_critical();s_tx_task=s_task=created;liot_rtos_exit_critical();}
    }
    if(!result&&!s_dev_capture_task){
        created=NULL;result=(int)liot_rtos_task_create(&created,DEV_CAPTURE_STACK,11,"dev_capture",dev_capture_worker,NULL);
        if(!result&&!created)result=-1;
        if(!result){liot_rtos_enter_critical();s_dev_capture_task=created;liot_rtos_exit_critical();}
    }
    liot_rtos_enter_critical();s_creating=false;liot_rtos_exit_critical();
    return result? -1:0;
#else

    liot_task_t tx_task=NULL;int result;
    liot_rtos_enter_critical();if(s_task||s_creating||s_tx_task||s_tx_creating){liot_rtos_exit_critical();return 0;}s_creating=true;s_tx_creating=true;liot_rtos_exit_critical();
    result=(int)liot_rtos_queue_create(&s_tx_queue,sizeof(call_tx_job_t),CALL_TX_QUEUE_DEPTH);
    if(result!=LIOT_OSI_SUCCESS){liot_rtos_enter_critical();s_creating=false;s_tx_creating=false;liot_rtos_exit_critical();return -1;}
    /* Liot_AudioRecord and Liot_AudioPlay share the F6D_A I2S/codec state.
     * DEV capture therefore runs from calls_audio_step(), alongside playback,
     * just like the proven group-room path. Keep only network TX asynchronous;
     * it never touches the audio driver. */
    result=(int)liot_rtos_task_create(&tx_task,6U*1024U,11,"call_tx",tx_worker,NULL);
    if(result||!tx_task){
        /* No worker was created, so this queue has no consumer yet. Keep
         * creation fenced while releasing it, then allow a clean retry. */
        (void)liot_rtos_queue_delete(s_tx_queue);s_tx_queue=NULL;
    }
    liot_rtos_enter_critical();s_creating=false;s_tx_creating=false;if(!result&&tx_task){s_task=tx_task;s_tx_task=tx_task;}liot_rtos_exit_critical();
    return !result&&tx_task ? 0 : -1;
#endif
}
int calls_audio_start(demo_tirtc_owner_e owner,uint32_t generation)
{
    demo_ai_audio_owner_e audio_owner;uint8_t volume,mic;uint32_t revision,extra=0;int result;
    if(!generation||!s_task||s_owned)return DEMO_AI_AUDIO_ERR_BUSY;
#if TIRTC_DEV_OPUS_ENABLE
    if((owner==DEMO_TIRTC_OWNER_DEV_CHAT ||
        (DEMO_AI_AUDIO_AEC_ENABLE && TIRTC_F9_WX_CAPTURE_RING && owner==DEMO_TIRTC_OWNER_WECHAT)) &&
        !s_dev_capture_task)return DEMO_AI_AUDIO_ERR_BUSY;
#endif
    if(owner==DEMO_TIRTC_OWNER_WECHAT)audio_owner=DEMO_AI_AUDIO_OWNER_WECHAT;
    else if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)audio_owner=DEMO_AI_AUDIO_OWNER_DEVICE;
    else if(owner==DEMO_TIRTC_OWNER_LIVE)audio_owner=DEMO_AI_AUDIO_OWNER_LIVE;
    else return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    memset(s_failure_log,0,sizeof(s_failure_log));
    result=demo_ai_audio_acquire(audio_owner,&s_lease);if(result){failure_log(FAIL_START,"acquire",result,0,true);return result;}
    liot_rtos_enter_critical();
    s_owned=true;s_owner=owner;s_generation=generation;s_running=false;s_error=0;
    memset(&s_dev_diagnostic,0,sizeof(s_dev_diagnostic));
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        s_dev_diagnostic.generation=generation;
    }
    s_session_speaker=true;s_session_microphone=owner!=DEMO_TIRTC_OWNER_LIVE;
    s_uplink_subscribed=owner!=DEMO_TIRTC_OWNER_LIVE;revise_config_locked();
    volume=speaker_allowed_locked()?s_volume:0;
    mic=microphone_allowed_locked()?effective_mic_level_locked():0;
    revision=s_config_revision;
    s_received=s_received_bytes=s_sent_bytes=0;
    s_first_rx_pending=s_first_rx_logged=s_first_tx_logged=false;
    liot_rtos_exit_critical();
#if TIRTC_DEV_OPUS_ENABLE
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        result=calls_dev_opus_init(&s_dev_opus);
        if(result){failure_log(FAIL_START,"opus_init",result,0,true);return result;}
        calls_dev_opus_reset(&s_dev_opus);
    }
#endif
    result=demo_ai_audio_prepare_session(&s_lease,volume,mic);if(result){failure_log(FAIL_START,"prepare",result,0,true);return result;}
    /* Lite DEV submits speech directly; do not queue a separate silent
     * preamble while the full-duplex call is connecting. */
    if(owner!=DEMO_TIRTC_OWNER_DEV_CHAT){
        result=demo_ai_audio_prewarm_session(&s_lease,&extra);if(result){failure_log(FAIL_START,"prewarm",result,0,true);return result;}
    }
    discard_rx();s_guard=extra!=0;s_tail=now()+extra;s_fraction=0;
    s_peak=s_drops=s_played=s_sent=0;s_tx_timestamp=now();s_pause_seen=false;s_last_log=now();
    s_tx_failing=s_record_failing=s_play_failing=s_draining=false;
    liot_rtos_enter_critical();s_applied_revision=revision;s_mute_pending=false;s_running=true;
    s_dev_diagnostic.started_at=s_dev_diagnostic.reported_at=now();
#if TIRTC_F9_CALL_AUDIO_QOS && TIRTC_CALL_MEDIA_DIAGNOSTICS
    memset(&s_audio_qos_counts,0,sizeof(s_audio_qos_counts));
#endif
    AUDIO_MEDIA_ONLY(
        memset(&s_audio_media,0,sizeof(s_audio_media));
        s_audio_media.generation=generation;
        s_audio_media.lease_generation=s_lease.generation;
        s_audio_media.started_at=s_audio_media.reported_at=s_dev_diagnostic.started_at;
    );
    liot_rtos_exit_critical();
#if TIRTC_DEV_OPUS_ENABLE
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT ||
       (DEMO_AI_AUDIO_AEC_ENABLE && TIRTC_F9_WX_CAPTURE_RING && owner==DEMO_TIRTC_OWNER_WECHAT))
        (void)liot_rtos_semaphore_release(s_dev_capture_wake);
#endif
    return 0;
}
void calls_audio_receive(demo_tirtc_owner_e owner,uint32_t generation,const TIRTCFRAMEINFO *frame,const void *data)
{
    uint32_t write,first,tail;uint8_t rate;bool new_segment;
#if TIRTC_DEV_OPUS_ENABLE
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){dev_receive(generation,frame,data);return;}
#endif
    if(!frame||!data||!frame->length||frame->length>RX_BYTES){
        AUDIO_MEDIA_ONLY(
            liot_rtos_enter_critical();
            if(audio_session_locked(owner,generation)&&s_running){
                ++s_audio_media.rx_in;++s_audio_media.rx_format;
            }
            liot_rtos_exit_critical();
        );
        return;
    }
    rate=frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C?8:
         frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C?16:0;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)||!s_running){liot_rtos_exit_critical();return;}
    AUDIO_MEDIA_ONLY(++s_audio_media.rx_in;);
    if(!speaker_allowed_locked()||s_mute_pending||s_draining){
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_muted;);
        liot_rtos_exit_critical();return;
    }
    if(frame->media!=TIRTC_AUDIO_ALAW||!rate||
       (owner==DEMO_TIRTC_OWNER_LIVE ? (frame->stream_id!=10U&&frame->stream_id!=14U) :
        (rate!=8U||frame->stream_id!=(owner==DEMO_TIRTC_OWNER_WECHAT?0U:10U)))){
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_format;);
        s_drops++;liot_rtos_exit_critical();return;
    }
    tail=(s_segment_head+s_segment_count+RX_SEGMENTS-1U)%RX_SEGMENTS;
    new_segment=!s_segment_count||s_segments[tail].rate!=rate;
    if(frame->length>RX_BYTES-s_count||(new_segment&&s_segment_count==RX_SEGMENTS)){
        AUDIO_MEDIA_ONLY(++s_audio_media.rx_full;s_audio_media.full_pcm+=frame->length*(rate==8U?2U:1U););
        s_drops++;liot_rtos_exit_critical();return;
    }
    write=(s_read+s_count)%RX_BYTES;first=RX_BYTES-write;if(first>frame->length)first=frame->length;
    memcpy(s_rx+write,data,first);if(first<frame->length)memcpy(s_rx,(const uint8_t*)data+first,frame->length-first);
    if(new_segment){tail=(s_segment_head+s_segment_count)%RX_SEGMENTS;s_segments[tail].bytes=0;s_segments[tail].rate=rate;s_segment_count++;}
    s_segments[tail].bytes=(uint16_t)(s_segments[tail].bytes+frame->length);
    s_count+=frame->length;if(s_count>s_peak)s_peak=s_count;s_received++;s_received_bytes+=frame->length;
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
    /* Validated WX payload is 8 kHz mono A-law, one byte per sample. */
    if(owner==DEMO_TIRTC_OWNER_WECHAT)
        dev_playout_observe(&s_wx_playout,frame->ts,(frame->length+7U)/8U,now());
#endif
    AUDIO_MEDIA_ONLY(++s_audio_media.rx_ok;s_audio_media.rx_pcm+=frame->length*(rate==8U?2U:1U););
    if(!s_first_rx_logged&&!s_first_rx_pending){s_first_rx_pending=true;s_first_rx_stream=frame->stream_id;s_first_rx_rate=rate;s_first_rx_length=frame->length;}
    liot_rtos_exit_critical();
}
static void log_metrics(demo_tirtc_owner_e owner,uint32_t generation)
{
    dev_diagnostic_report(owner,generation,false);
    AUDIO_MEDIA_ONLY(audio_media_report(owner,generation,false););
    if((uint32_t)(now()-s_last_log)>=5000U){s_last_log=now();
        TIRTC_LOG_DEBUG("[AUDIO42] owner=%u gen=%lu tx=%lu tx_bytes=%lu rx=%lu rx_bytes=%lu play=%lu q=%lu peak=%lu drop=%lu capture_busy=%u\r\n",
            (unsigned)owner,(unsigned long)generation,(unsigned long)s_sent,(unsigned long)s_sent_bytes,
            (unsigned long)s_received,(unsigned long)s_received_bytes,(unsigned long)s_played,(unsigned long)s_count,
            (unsigned long)s_peak,(unsigned long)s_drops,s_record_busy?1U:0U);}
}
static int apply_controls(void)
{
    uint8_t volume,mic;uint32_t revision,discarded=0;bool busy,mute;int result;
    liot_rtos_enter_critical();revision=s_config_revision;volume=speaker_allowed_locked()?s_volume:0;
    mic=microphone_allowed_locked()?effective_mic_level_locked():0;
    busy=s_record_busy;mute=s_mute_pending;liot_rtos_exit_critical();
    if(mute&&!s_draining){
        if(busy)return DEMO_AI_AUDIO_ERR_BUSY;
        result=demo_ai_audio_set_levels(&s_lease,0,mic);if(result){failure_log(FAIL_CONTROL,"mute",result,0,result!=DEMO_AI_AUDIO_ERR_BUSY);return result;}
        discard_rx();s_draining=true;s_drain_since=now();
        liot_rtos_enter_critical();s_mute_pending=false;liot_rtos_exit_critical();
    }
    if(s_draining){
        if((s_guard&&before(s_tail+120U))||!demo_ai_audio_play_done(&s_lease)){
            if((uint32_t)(now()-s_drain_since)>=3000U)return fatal_error(FAIL_CONTROL,"mute_drain",DEMO_AI_AUDIO_ERR_BUSY,now()-s_drain_since);
            return DEMO_AI_AUDIO_ERR_BUSY;
        }
        s_draining=false;s_guard=false;s_continuing=false;
    }
    if(revision==s_applied_revision)return 0;
    if(busy)return DEMO_AI_AUDIO_ERR_BUSY;
    result=demo_ai_audio_set_levels(&s_lease,volume,mic);if(result){failure_log(FAIL_CONTROL,"levels",result,0,result!=DEMO_AI_AUDIO_ERR_BUSY);return result;}
    result=demo_ai_audio_discard_capture_timed(&s_lease,&discarded);if(result){failure_log(FAIL_CONTROL,"discard",result,0,result!=DEMO_AI_AUDIO_ERR_BUSY);return result;}
    liot_rtos_enter_critical();s_tx_timestamp+=discarded;s_applied_revision=revision;liot_rtos_exit_critical();
    return 0;
}
int calls_audio_step(demo_tirtc_owner_e owner,uint32_t generation)
{
    uint32_t started=now(),count,samples,source,first,extra,duration,fraction,i,queued;int result;bool playing,first_rx;
    bool direct_play=owner==DEMO_TIRTC_OWNER_DEV_CHAT;
    uint32_t prebuffer_bytes=direct_play?DEV_PREBUFFER_BYTES:PREBUFFER_BYTES;
    uint32_t prebuffer_ms=direct_play?DEV_PREBUFFER_TIMEOUT_MS:PREBUFFER_MS;
    uint32_t play_batch_max=direct_play?DEV_PLAY_BATCH_MAX:24U;
    uint8_t rate,first_stream,first_rate;uint32_t first_length;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)||!s_running){liot_rtos_exit_critical();return 0;}
    first_rx=s_first_rx_pending;s_first_rx_pending=false;
    first_stream=s_first_rx_stream;first_rate=s_first_rx_rate;first_length=s_first_rx_length;
    if(first_rx)s_first_rx_logged=true;
    liot_rtos_exit_critical();
    if(first_rx)TIRTC_LOG_DEBUG("[AUDIO42] first_rx owner=%u gen=%lu stream=%u rate=%u bytes=%lu\r\n",
        (unsigned)owner,(unsigned long)generation,(unsigned)first_stream,(unsigned)first_rate*1000U,(unsigned long)first_length);
    liot_rtos_enter_critical();result=s_error;liot_rtos_exit_critical();
    if(result)return result;
    log_metrics(owner,generation);
    result=apply_controls();if(result&&result!=DEMO_AI_AUDIO_ERR_BUSY)return result;
    liot_rtos_enter_critical();playing=speaker_allowed_locked()&&!s_mute_pending&&!s_draining;count=s_count;liot_rtos_exit_critical();
#if TIRTC_DEV_OPUS_ENABLE
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        if(playing)dev_service_downlink(generation);
#if !DEMO_AI_AUDIO_AEC_ENABLE
        CONTROL_CAPTURE_ONCE();
#endif
        return 0;
    }
#endif
    if(!playing){CONTROL_CAPTURE_ONCE();return 0;}
    if(s_continuing&&!before(s_tail)&&demo_ai_audio_play_done(&s_lease)){
#if TIRTC_BOARD_NT26F9D0
        if(owner==DEMO_TIRTC_OWNER_WECHAT){
            liot_rtos_enter_critical();
            wx_reset_upsample_locked();
            liot_rtos_exit_critical();
        }
#endif
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
        if(owner==DEMO_TIRTC_OWNER_WECHAT){
            liot_rtos_enter_critical();
            count=s_count;
            s_wx_play_ready=count!=0U;
            if(!count){
                dev_playout_underrun(&s_wx_playout);
                AUDIO_MEDIA_ONLY(++s_audio_media.rebuffer;++s_audio_media.starve;);
            }
            liot_rtos_exit_critical();
        }else
#endif
        AUDIO_MEDIA_ONLY(
            liot_rtos_enter_critical();
            ++s_audio_media.rebuffer;if(!s_count)++s_audio_media.starve;
            liot_rtos_exit_critical();
        );
        s_continuing=false;s_prebuffer=false;
    }
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
    if(owner==DEMO_TIRTC_OWNER_WECHAT&&!s_continuing){
        bool ready;
        liot_rtos_enter_critical();
        count=s_count;
        if(!count)s_wx_play_ready=false;
        if(!s_wx_play_ready)
            s_wx_play_ready=dev_playout_should_start(&s_wx_playout,(count+7U)/8U,count,RX_BYTES,now());
        ready=s_wx_play_ready;
        liot_rtos_exit_critical();
        if(!ready){CONTROL_CAPTURE_ONCE();return 0;}
    }else
#endif
    {
        if(!count){s_prebuffer=false;CONTROL_CAPTURE_ONCE();return 0;}
        if(!s_continuing){
            if(!s_prebuffer){s_prebuffer=true;s_prebuffer_at=s_partial?s_partial_at:now();}
            if(count<prebuffer_bytes&&(uint32_t)(now()-s_prebuffer_at)<prebuffer_ms){CONTROL_CAPTURE_ONCE();return 0;}
        }
    }
    for(i=0;i<play_batch_max&&(uint32_t)(now()-started)<20U;i++){
        liot_rtos_enter_critical();count=s_count;playing=s_running&&speaker_allowed_locked()&&!s_mute_pending&&!s_draining;
        source=s_segment_count?s_segments[s_segment_head].bytes:0;
        rate=s_segment_count?s_segments[s_segment_head].rate:8;
        if(source>CALL_NETWORK_SAMPLES_8K)source=CALL_NETWORK_SAMPLES_8K;
        samples=rate==16U?source:source*2U;
        if(samples>1U&&(samples&1U))samples--;
        source=rate==16U?samples:samples/2U;
#if TIRTC_BOARD_NT26F9D0
        const bool interpolate=owner==DEMO_TIRTC_OWNER_WECHAT&&rate==8U;
        const bool continuous=s_wx_previous_valid;
        const int16_t previous=s_wx_previous_sample;
#endif
#if WX_PLAY_RETRY_CACHE
        const uint32_t rx_head=s_read;
        const bool reuse_pcm=owner==DEMO_TIRTC_OWNER_WECHAT&&s_wx_pcm_generation==generation&&
                             s_wx_pcm_read==rx_head&&s_wx_pcm_source==source;
        if(!reuse_pcm)
#endif
        {
            first=RX_BYTES-s_read;if(first>source)first=source;
            memcpy(s_alaw,s_rx+s_read,first);if(source>first)memcpy(s_alaw+first,s_rx,source-first);
        }
        liot_rtos_exit_critical();
        if(!playing||!count)break;
        if(!samples){
            /* An empty local PCM block cannot be submitted. */
            if(!s_partial){s_partial=true;s_partial_at=now();}
            if((uint32_t)(now()-s_partial_at)<PREBUFFER_MS)break;
            liot_rtos_enter_critical();consume_rx_locked(1);s_drops++;
#if TIRTC_BOARD_NT26F9D0
            if(owner==DEMO_TIRTC_OWNER_WECHAT)wx_reset_upsample_locked();
#endif
            AUDIO_MEDIA_ONLY(s_audio_media.drop_pcm+=rate==8U?2U:1U;);
            liot_rtos_exit_critical();s_partial=false;continue;
        }
        if(samples==1U){if(!s_partial){s_partial=true;s_partial_at=s_prebuffer?s_prebuffer_at:now();}
            if((uint32_t)(now()-s_partial_at)<PREBUFFER_MS)break;}
        else s_partial=false;
        queued=s_guard&&before(s_tail)?s_tail-now():0;
        duration=(samples+(samples&1U)+(s_continuing?s_fraction:0U))/16U;
        if(queued&&queued+duration+((s_continuing||direct_play)?0U:80U)+1U>PLAY_AHEAD)break;
#if WX_PLAY_RETRY_CACHE
        if(!reuse_pcm)
#endif
        {
            demo_g711_alaw_decode(s_alaw,s_pcm,source);
            if(rate==8U){
#if TIRTC_BOARD_NT26F9D0
                if(interpolate)wx_upsample_8k_to_16k(s_pcm,source,continuous,previous);
                else
#endif
                {
                    uint32_t sample;
                    for(sample=source; sample>0U; --sample){
                        int16_t value=s_pcm[sample-1U];
                        s_pcm[(sample-1U)*2U]=value;
                        s_pcm[(sample-1U)*2U+1U]=value;
                    }
                }
            }
            if(samples&1U)s_pcm[samples]=0;
        }
        extra=0;result=(s_continuing||direct_play)?demo_ai_audio_play(&s_lease,s_pcm,samples+(samples&1U)):
            demo_ai_audio_play_warm(&s_lease,s_pcm,samples+(samples&1U),&extra);
        if(result){
#if WX_PLAY_RETRY_CACHE || TIRTC_CALL_MEDIA_DIAGNOSTICS
            liot_rtos_enter_critical();
            AUDIO_MEDIA_ONLY(if(result==DEMO_AI_AUDIO_ERR_PLAY_BUSY)++s_audio_media.play_busy;
                             else ++s_audio_media.play_fail;);
#if WX_PLAY_RETRY_CACHE
            s_wx_pcm_generation=0;
            if(owner==DEMO_TIRTC_OWNER_WECHAT){
                AUDIO_MEDIA_ONLY(if(reuse_pcm)++s_audio_media.wx_pcm_retries_saved;);
                if(result==DEMO_AI_AUDIO_ERR_PLAY_BUSY&&s_read==rx_head&&s_running&&
                   audio_session_locked(owner,generation)&&speaker_allowed_locked()&&!s_mute_pending&&!s_draining){
                    s_wx_pcm_read=rx_head;
                    s_wx_pcm_source=source;
                    s_wx_pcm_generation=generation;
                }
            }
#endif
            liot_rtos_exit_critical();
#endif
#if DEMO_AI_AUDIO_AEC_ENABLE
            /* The stream ring rejected the whole block. Keep RX at its head
             * and still service capture below; backpressure is not failure. */
            if(result==DEMO_AI_AUDIO_ERR_PLAY_BUSY){s_play_failing=false;break;}
#endif
            if(result==DEMO_AI_AUDIO_ERR_WARM_PARTIAL)return fatal_error(FAIL_PLAY,"warm_partial",result,0);
            if(!s_play_failing){s_play_failing=true;s_play_failure_at=now();}
            if((uint32_t)(now()-s_play_failure_at)>=200U)return fatal_error(FAIL_PLAY,"play",result,now()-s_play_failure_at);
            failure_log(FAIL_PLAY,"play",result,now()-s_play_failure_at,false);
            break;
        }
        fraction=samples+(samples&1U)+(s_continuing?s_fraction:0U);
        if(!s_guard||!before(s_tail))s_tail=now();
        s_tail+=duration+extra;s_fraction=(uint8_t)(fraction%16U);
        s_guard=s_continuing=true;s_prebuffer=s_partial=s_play_failing=false;s_played++;
        liot_rtos_enter_critical();consume_rx_locked(source);
#if TIRTC_BOARD_NT26F9D0
        /* A BUSY result leaves both the input and the previous sample
         * unchanged. Commit the original last sample only after playback
         * accepts the whole block, including a reused retry block. */
        if(owner==DEMO_TIRTC_OWNER_WECHAT&&audio_session_locked(owner,generation)){
            s_wx_previous_valid=interpolate;
            if(interpolate)s_wx_previous_sample=s_pcm[samples-1U];
        }
#endif
#if TIRTC_F9_WX_ADAPTIVE_PLAYOUT
        if(owner==DEMO_TIRTC_OWNER_WECHAT&&s_wx_play_ready){
            dev_playout_started(&s_wx_playout);
            s_wx_play_ready=false;
        }
#endif
        AUDIO_MEDIA_ONLY(++s_audio_media.play_ok;s_audio_media.play_pcm+=samples;);
#if WX_PLAY_RETRY_CACHE
        AUDIO_MEDIA_ONLY(if(reuse_pcm)++s_audio_media.wx_pcm_retries_saved;);
#endif
        liot_rtos_exit_critical();
    }
    /* Capture after the playback burst in this same task. The call TX task
     * only sends already prepared A-law frames and never touches I2S. */
    CONTROL_CAPTURE_ONCE();
    return 0;
}
int calls_audio_stop(demo_tirtc_owner_e owner,uint32_t generation)
{
    int result;bool busy,tx_busy;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)){liot_rtos_exit_critical();return 0;}
    s_running=false;busy=s_record_busy;tx_busy=s_tx_busy;liot_rtos_exit_critical();
#if TIRTC_DEV_OPUS_ENABLE
    liot_rtos_enter_critical();busy=busy||s_dev_producers!=0U;liot_rtos_exit_critical();
#endif
    if(busy||tx_busy)return DEMO_AI_AUDIO_ERR_BUSY;
    result=demo_ai_audio_stop(&s_lease);if(result){failure_log(FAIL_STOP,"stop",result,0,false);return result;}
#if TIRTC_F9_AV_FOCUS && DEMO_AI_AUDIO_AEC_ENABLE
    if (owner==DEMO_TIRTC_OWNER_DEV_CHAT || owner==DEMO_TIRTC_OWNER_WECHAT) {
        result=demo_ai_audio_set_av_focus(&s_lease,false);
        if(result){failure_log(FAIL_STOP,"restore_aec_priority",result,0,false);return result;}
    }
#endif
    result=demo_ai_audio_release(&s_lease);if(result){failure_log(FAIL_STOP,"release",result,0,false);return result;}
    if(s_tx_queue!=NULL){call_tx_job_t stale;while(liot_rtos_queue_wait(s_tx_queue,(uint8_t *)&stale,sizeof(stale),LIOT_NO_WAIT)==LIOT_OSI_SUCCESS){
        liot_rtos_enter_critical();dev_diagnostic_tx_locked(&stale,false,false,0,0);
        AUDIO_MEDIA_ONLY(if(s_audio_media.generation==stale.generation&&s_owner==stale.owner)++s_audio_media.tx_stale;);
        liot_rtos_exit_critical();}}
    dev_diagnostic_report(owner,generation,true);
    AUDIO_MEDIA_ONLY(audio_media_report(owner,generation,true););
#if TIRTC_DEV_OPUS_ENABLE
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)calls_dev_opus_deinit(&s_dev_opus);
#endif
#if TIRTC_DEV_OPUS_ENABLE
    discard_rx();s_guard=s_continuing=false;
    liot_rtos_enter_critical();s_owned=false;s_generation=0;liot_rtos_exit_critical();return 0;
#else
    discard_rx();s_guard=s_continuing=false;s_owned=false;s_generation=0;return 0;
#endif
}
