/* SPDX-License-Identifier: MIT
 * Shared call/LIVE audio: Lite-compatible 16 kHz local PCM with 8 kHz
 * A-law for WX/LIVE and Opus for DEV. DEV uses adaptive playout and a
 * separate AEC capture task. The ordinary hardware backend serializes I/O. */
#include "calls_audio.h"
#include "dev_playout.h"
#include "g711_codec.h"
#include "opus_codec.h"
#include "liot_os.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include <string.h>
#define RX_BYTES 15360U
#define RX_SEGMENTS 32U
#define PLAY_AHEAD 480U
#define PREBUFFER_BYTES 1920U
#define PREBUFFER_MS 120U
#define WX_RX_PACKETS 8U
#define WX_RX_MAX_BYTES 640U
#define WX_PREBUFFER_PACKETS 3U
#define WX_PREBUFFER_MS 80U
#define DEV_RX_DEPTH 32U
#define DEV_ALAW_MAX_BYTES 640U
#define DEV_PLAY_BATCH_MAX 8U
#define DEV_PLAY_BUDGET_MS 20U
#define CALL_TX_MAX_AGE_MS 300U
#define DEV_CAPTURE_TIMEOUT_LIMIT_MS 2000U
#define AUDIO_ERROR (-6208)
#define CALL_NETWORK_SAMPLES_8K (DEMO_AI_AUDIO_FRAME_SAMPLES / 2U)
#define CALL_TX_QUEUE_DEPTH 8U
static liot_task_t s_task;
static liot_task_t s_tx_task;
static liot_queue_t s_tx_queue;
#if DEMO_AI_AUDIO_AEC_ENABLE
static liot_task_t s_dev_capture_task;
static liot_sem_t s_dev_capture_wake;
#endif
static bool s_creating, s_running, s_owned, s_record_busy;
static bool s_tx_creating, s_tx_busy;
static demo_ai_audio_lease_t s_lease=DEMO_AI_AUDIO_LEASE_INIT;
static demo_tirtc_owner_e s_owner;
static uint32_t s_generation;
static uint8_t s_volume=DEMO_AI_AUDIO_DEFAULT_SPK_LEVEL, s_mic=DEMO_AI_AUDIO_DEFAULT_MIC_LEVEL;
static bool s_speaker=true, s_microphone=true;
static bool s_session_speaker=true, s_session_microphone=true, s_uplink_subscribed=true;
/* LIVE coalesces adjacent frames at the same rate. WX preserves one slot
 * per incoming packet to cap latency independently of payload size. */
typedef struct { uint16_t bytes; uint8_t rate; } audio_segment_t;
static audio_segment_t s_segments[RX_SEGMENTS];
static uint32_t s_segment_head, s_segment_count;
static uint32_t s_received, s_received_bytes, s_sent_bytes;
static bool s_first_rx_pending, s_first_rx_logged, s_first_tx_logged;
static uint8_t s_first_rx_stream, s_first_rx_rate;
static uint32_t s_first_rx_length;
static uint32_t s_config_revision=1, s_applied_revision;
static uint32_t s_capture_revision=1, s_applied_capture_revision;
static bool s_mute_pending, s_draining;
static uint32_t s_drain_since;
static uint8_t s_rx[RX_BYTES], s_alaw[DEMO_AI_AUDIO_FRAME_SAMPLES];
typedef struct {
    uint16_t length, samples;
    uint8_t media, flags;
    uint8_t payload[DEMO_AI_OPUS_MAX_PACKET];
} dev_rx_packet_t;
static dev_rx_packet_t s_dev_rx[DEV_RX_DEPTH];
static demo_opus_context_t s_dev_opus = DEMO_OPUS_CONTEXT_INIT;
static dev_playout_t s_dev_playout;
static uint32_t s_dev_read, s_dev_count, s_dev_queued_samples;
static bool s_dev_prebuffering;
static int16_t s_dev_pcm[DEMO_OPUS_MAX_DECODE_SAMPLES];
static uint32_t s_dev_pcm_samples, s_dev_pcm_sent;
static int16_t s_pcm[DEMO_AI_AUDIO_FRAME_SAMPLES], s_capture[DEMO_AI_AUDIO_FRAME_SAMPLES];
static uint32_t s_read, s_count, s_peak, s_drops, s_played, s_sent;
static uint32_t s_tail, s_prebuffer_at, s_partial_at;
static bool s_guard, s_continuing, s_prebuffer, s_partial;
static uint8_t s_fraction;
static uint32_t s_tx_timestamp, s_pause_at;
static bool s_pause_seen;
static int s_error;
static bool s_tx_failing, s_record_failing;
static uint32_t s_tx_failure_at, s_record_failure_at;
static uint32_t s_last_log, s_play_failure_at;
static bool s_play_failing;
static uint32_t s_dev_tx_drops, s_tx_expired, s_dev_sdk_busy;
enum failure_kind { FAIL_RECORD, FAIL_SEND, FAIL_PLAY, FAIL_CONTROL, FAIL_STOP, FAIL_START, FAIL_COUNT };
typedef struct {
    demo_tirtc_owner_e owner;
    uint32_t generation;
    uint32_t revision;
    uint32_t enqueue_ms;
    TIRTCFRAMEINFO frame;
    uint8_t payload[DEMO_AI_OPUS_MAX_PACKET];
} call_tx_job_t;
/* Each slot has one task owner. Reset only before capture admission opens.
 * No diagnostic heap allocation and no heap query/printing under critical. */
static struct { uint32_t at; bool seen, terminal; } s_failure_log[FAIL_COUNT];
static uint32_t now(void){return liot_rtos_get_running_time();}
static bool before(uint32_t end){return (int32_t)(end-now())>0;}
static void audio_error(void){liot_rtos_enter_critical();s_error=AUDIO_ERROR;liot_rtos_exit_critical();}
static void failure_log(enum failure_kind kind,const char *reason,int result,uint32_t duration,bool terminal)
{
    uint32_t stamp=now(),generation;size_t free_bytes,max_block;
    if(s_failure_log[kind].terminal)return;
    if(!terminal&&s_failure_log[kind].seen&&(uint32_t)(stamp-s_failure_log[kind].at)<1000U)return;
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
static uint8_t capture_level_locked(void)
{return microphone_allowed_locked()?s_mic:0;}
static void revise_config_locked(bool capture_changed)
{
    if(!++s_config_revision)++s_config_revision;
    /* DEV playback-only changes leave its independent capture/TX valid. */
    if((capture_changed||s_owner!=DEMO_TIRTC_OWNER_DEV_CHAT) && !++s_capture_revision)
        ++s_capture_revision;
}
static void discard_rx(void)
{
    liot_rtos_enter_critical();s_read=s_count=0;s_segment_head=s_segment_count=0;
    s_dev_read=s_dev_count=s_dev_queued_samples=0U;
    s_dev_pcm_samples=s_dev_pcm_sent=0U;
    s_dev_prebuffering=true;
    dev_playout_init(&s_dev_playout);
    liot_rtos_exit_critical();s_prebuffer=s_partial=s_continuing=false;
}
/* Caller holds the critical section. Used only after copied audio was accepted
 * by playback, or after an incomplete source sample has timed out. */
static void consume_rx_locked(uint32_t bytes)
{
    s_read=(s_read+bytes)%RX_BYTES;s_count-=bytes;
    s_segments[s_segment_head].bytes=(uint16_t)(s_segments[s_segment_head].bytes-bytes);
    if(!s_segments[s_segment_head].bytes){s_segment_head=(s_segment_head+1U)%RX_SEGMENTS;s_segment_count--;}
}
void calls_audio_set_config(uint8_t volume,uint8_t mic,bool speaker,bool microphone)
{
    bool was_speaker;uint8_t previous_mic;
    if(volume>10)volume=10;
    if(mic>10)mic=10;
    liot_rtos_enter_critical();was_speaker=speaker_allowed_locked();
    previous_mic=capture_level_locked();
    if(s_volume!=volume||s_mic!=mic||s_speaker!=speaker||s_microphone!=microphone){
        s_volume=volume;s_mic=mic;s_speaker=speaker;s_microphone=microphone;
        revise_config_locked(previous_mic!=capture_level_locked());
    }
    if(was_speaker&&!speaker_allowed_locked())s_mute_pending=true;
    liot_rtos_exit_critical();
}
int calls_audio_set_session_controls(demo_tirtc_owner_e owner,uint32_t generation,
                                     bool speaker,bool microphone,bool uplink_subscribed)
{
    bool was_speaker;uint8_t previous_mic;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)){liot_rtos_exit_critical();return DEMO_AI_AUDIO_ERR_INVALID_LEASE;}
    was_speaker=speaker_allowed_locked();previous_mic=capture_level_locked();
    if(s_session_speaker!=speaker||s_session_microphone!=microphone||s_uplink_subscribed!=uplink_subscribed){
        s_session_speaker=speaker;s_session_microphone=microphone;s_uplink_subscribed=uplink_subscribed;
        revise_config_locked(previous_mic!=capture_level_locked());
    }
    if(was_speaker&&!speaker_allowed_locked())s_mute_pending=true;
    liot_rtos_exit_critical();return 0;
}
bool calls_audio_busy(void){bool busy;liot_rtos_enter_critical();busy=s_owned;liot_rtos_exit_critical();return busy;}
static bool tx_job_current_locked(const call_tx_job_t *job)
{
    return s_running && audio_session_locked(job->owner,job->generation) &&
           microphone_allowed_locked() && s_capture_revision==job->revision;
}
static void tx_send_job(const call_tx_job_t *job)
{
    bool current,first=false;
    int result;
    liot_rtos_enter_critical();
    current=tx_job_current_locked(job);
    if (current && (uint32_t)(now()-job->enqueue_ms)>CALL_TX_MAX_AGE_MS) {
        ++s_tx_expired;
        current=false;
    }
    s_tx_busy=current;
    liot_rtos_exit_critical();
    if(!current)return;
    result=demo_tirtc_send_audio(job->owner,job->generation,&job->frame,job->payload);
    liot_rtos_enter_critical();
    current=tx_job_current_locked(job);
    if(current && job->owner==DEMO_TIRTC_OWNER_DEV_CHAT && result==TIRTC_E_BUSY)
        ++s_dev_sdk_busy;
    liot_rtos_exit_critical();
    /* Keep stop/release fenced until this job has finished updating its
     * session state, including a terminal error raised after logging. */
    if(!current)goto finished;
    if(result>0 || (result==0 && job->owner!=DEMO_TIRTC_OWNER_LIVE)){
        ++s_sent; s_sent_bytes+=job->frame.length; s_tx_failing=false;
        if(!s_first_tx_logged){s_first_tx_logged=true;first=true;}
    }else if(result==TIRTC_E_BUSY){
        s_tx_failing=false;
    }else{
        if(!s_tx_failing){s_tx_failing=true;s_tx_failure_at=now();}
        if(job->owner==DEMO_TIRTC_OWNER_DEV_CHAT || (uint32_t)(now()-s_tx_failure_at)>=3000U)
            (void)fatal_error(FAIL_SEND,"send",result,now()-s_tx_failure_at);
        else failure_log(FAIL_SEND,"send",result,now()-s_tx_failure_at,false);
    }
    if(first)TIRTC_LOG_DEBUG("[AUDIO42] first_tx owner=%u gen=%lu stream=%u rate=%u bytes=%lu\r\n",
        (unsigned)job->owner,(unsigned long)job->generation,(unsigned)job->frame.stream_id,
        job->owner==DEMO_TIRTC_OWNER_DEV_CHAT?16000U:8000U,(unsigned long)job->frame.length);
finished:
    liot_rtos_enter_critical();s_tx_busy=false;liot_rtos_exit_critical();
}
static void tx_worker(void *arg)
{
    call_tx_job_t job;
    (void)arg;
    for(;;)if(liot_rtos_queue_wait(s_tx_queue,(uint8_t *)&job,sizeof(job),LIOT_WAIT_FOREVER)==LIOT_OSI_SUCCESS)tx_send_job(&job);
}
static void capture_once(demo_tirtc_owner_e owner,uint32_t generation)
{
    demo_ai_audio_lease_t lease; uint32_t timestamp=0, revision=0;
    bool allowed, current; int result; call_tx_job_t job;
    liot_rtos_enter_critical();
    if(!s_running||!audio_session_locked(owner,generation)){
        liot_rtos_exit_critical();return;
    }
    allowed=!s_error&&!s_record_busy&&microphone_allowed_locked()&&
        s_capture_revision==s_applied_capture_revision&&
        (s_config_revision==s_applied_revision||s_draining);
    if(allowed){
        s_record_busy=true;lease=s_lease;revision=s_capture_revision;
        if(s_pause_seen){s_tx_timestamp+=now()-s_pause_at;s_pause_seen=false;}
        timestamp=s_tx_timestamp;s_tx_timestamp+=20U;
    }else if(!s_pause_seen){s_pause_seen=true;s_pause_at=now();}
    liot_rtos_exit_critical();
    if(!allowed){s_tx_failing=s_record_failing=false;liot_rtos_task_sleep_ms(5);return;}
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)
        result=demo_ai_audio_record_20ms_timed(&lease,s_capture,&timestamp);
    else result=demo_ai_audio_record_20ms(&lease,s_capture);
#if DEMO_AI_AUDIO_AEC_ENABLE
    /* Fresh AEC PCM follows wall time; a timeout or scheduling gap must not
     * compress the audio timeline. */
    if(!result&&owner!=DEMO_TIRTC_OWNER_DEV_CHAT)timestamp=now();
#endif
    liot_rtos_enter_critical();
    current=s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&s_capture_revision==revision;
    liot_rtos_exit_critical();
    if(result && result!=DEMO_AI_AUDIO_ERR_BUSY){
        if(current){
            if(!s_record_failing){s_record_failing=true;s_record_failure_at=now();}
            uint32_t elapsed=now()-s_record_failure_at;
            uint32_t limit=owner==DEMO_TIRTC_OWNER_DEV_CHAT?
                DEV_CAPTURE_TIMEOUT_LIMIT_MS:3000U;
            if(elapsed>=limit || (owner==DEMO_TIRTC_OWNER_DEV_CHAT &&
               (!DEMO_AI_AUDIO_AEC_ENABLE || result!=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT)))
                (void)fatal_error(FAIL_RECORD,"record",result,elapsed);
            else failure_log(FAIL_RECORD,"record",result,elapsed,false);
        }
    }else if(!result){
        s_record_failing=false;
        if(current){
            memset(&job,0,sizeof(job));
            job.owner=owner;job.generation=generation;job.revision=revision;
            job.frame.stream_id=owner==DEMO_TIRTC_OWNER_WECHAT?0U:10U;
            job.frame.ts=timestamp;
            if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
                result=demo_opus_encode_20ms(&s_dev_opus,s_capture,job.payload,sizeof(job.payload));
                job.frame.media=TIRTC_AUDIO_OPUS;
                job.frame.flags=TIRTC_AUDIOSAMPLE_16K16B1C;
                job.frame.length=(uint32_t)result;
            }else{
                uint32_t sample;
                for(sample=0U;sample<CALL_NETWORK_SAMPLES_8K;++sample){
                    int32_t mixed=(int32_t)s_capture[sample*2U]+(int32_t)s_capture[sample*2U+1U];
                    job.payload[sample]=demo_g711_alaw_encode_sample((int16_t)(mixed/2));
                }
                job.frame.media=TIRTC_AUDIO_ALAW;
                job.frame.flags=TIRTC_AUDIOSAMPLE_8K16B1C;
                job.frame.length=CALL_NETWORK_SAMPLES_8K;
            }
            job.enqueue_ms=now();
            liot_rtos_enter_critical();
            current=s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&s_capture_revision==revision;
            liot_rtos_exit_critical();
            if(!current){s_tx_failing=false;liot_rtos_enter_critical();
                s_record_busy=false;liot_rtos_exit_critical();return;}
            if(owner==DEMO_TIRTC_OWNER_DEV_CHAT && result<=0){
                (void)fatal_error(FAIL_RECORD,"opus_encode",result,0);
                goto capture_finished;
            }
            {
                int queue_result=liot_rtos_queue_release(s_tx_queue,sizeof(job),(uint8_t *)&job,LIOT_NO_WAIT);
                if(owner==DEMO_TIRTC_OWNER_DEV_CHAT && queue_result!=LIOT_OSI_SUCCESS)
                    ++s_dev_tx_drops;
                if(queue_result!=LIOT_OSI_SUCCESS)s_tx_failing=false;
                result=queue_result==LIOT_OSI_SUCCESS ? 1 : -1;
            }
        }else s_tx_failing=false;
    }
capture_finished:
    liot_rtos_enter_critical();s_record_busy=false;liot_rtos_exit_critical();
    if(result<=0)liot_rtos_task_sleep_ms(5);
}
#if DEMO_AI_AUDIO_AEC_ENABLE
static void dev_capture_worker(void *arg)
{
    (void)arg;
    for(;;){
        uint32_t generation;
        liot_rtos_enter_critical();
        generation=s_owned&&s_running&&s_owner==DEMO_TIRTC_OWNER_DEV_CHAT?s_generation:0;
        liot_rtos_exit_critical();
        if(generation){
            capture_once(DEMO_TIRTC_OWNER_DEV_CHAT,generation);
            liot_rtos_task_sleep_ms(1U);
        }else (void)liot_rtos_semaphore_wait(s_dev_capture_wake,LIOT_WAIT_FOREVER);
    }
}

static int dev_capture_start_service(void)
{
    liot_task_t task=NULL;
    if(s_dev_capture_task) return 0;
    if(!s_dev_capture_wake &&
       (liot_rtos_semaphore_create(&s_dev_capture_wake,0U)!=LIOT_OSI_SUCCESS ||
        !s_dev_capture_wake)) return -1;
    if(liot_rtos_task_create(&task,64U*1024U,LIOT_APP_TASK_PRIORITY,
                            "pro_dev_capture",dev_capture_worker,NULL)!=LIOT_OSI_SUCCESS ||
       !task)return -1;
    s_dev_capture_task=task;
    return 0;
}
#endif

int calls_audio_start_service(void)
{
    liot_task_t tx_task=NULL;int result;
    liot_rtos_enter_critical();if(s_task||s_creating||s_tx_task||s_tx_creating){liot_rtos_exit_critical();return 0;}s_creating=true;s_tx_creating=true;liot_rtos_exit_critical();
    result=(int)liot_rtos_queue_create(&s_tx_queue,sizeof(call_tx_job_t),CALL_TX_QUEUE_DEPTH);
    if(result!=LIOT_OSI_SUCCESS){liot_rtos_enter_critical();s_creating=false;s_tx_creating=false;liot_rtos_exit_critical();return -1;}
    /* TX only sends already encoded packets and never touches the codec. */
    result=(int)liot_rtos_task_create(&tx_task,12U*1024U,LIOT_APP_TASK_PRIORITY,"call_tx",tx_worker,NULL);
    if(result||!tx_task){
        /* No worker was created, so this queue has no consumer yet. Keep
         * creation fenced while releasing it, then allow a clean retry. */
        (void)liot_rtos_queue_delete(s_tx_queue);s_tx_queue=NULL;
    }
    liot_rtos_enter_critical();s_creating=false;s_tx_creating=false;if(!result&&tx_task){s_task=tx_task;s_tx_task=tx_task;}liot_rtos_exit_critical();
    return !result&&tx_task ? 0 : -1;
}
int calls_audio_start(demo_tirtc_owner_e owner,uint32_t generation)
{
    demo_ai_audio_owner_e audio_owner;uint8_t volume,mic;uint32_t revision,capture_revision,extra=0;int result;
    if(!generation||!s_task||s_owned)return DEMO_AI_AUDIO_ERR_BUSY;
    if(owner==DEMO_TIRTC_OWNER_WECHAT)audio_owner=DEMO_AI_AUDIO_OWNER_WECHAT;
    else if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)audio_owner=DEMO_AI_AUDIO_OWNER_DEVICE;
    else if(owner==DEMO_TIRTC_OWNER_LIVE)audio_owner=DEMO_AI_AUDIO_OWNER_LIVE;
    else return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    memset(s_failure_log,0,sizeof(s_failure_log));
    result=demo_ai_audio_acquire(audio_owner,&s_lease);if(result){failure_log(FAIL_START,"acquire",result,0,true);return result;}
    liot_rtos_enter_critical();
    s_owned=true;s_owner=owner;s_generation=generation;s_running=false;s_error=0;
    s_dev_tx_drops=s_tx_expired=s_dev_sdk_busy=0;
    s_session_speaker=true;s_session_microphone=owner!=DEMO_TIRTC_OWNER_LIVE;
    s_uplink_subscribed=owner!=DEMO_TIRTC_OWNER_LIVE;revise_config_locked(true);
    volume=speaker_allowed_locked()?s_volume:0;
    mic=capture_level_locked();
    revision=s_config_revision;capture_revision=s_capture_revision;
    s_received=s_received_bytes=s_sent_bytes=0;
    s_first_rx_pending=s_first_rx_logged=s_first_tx_logged=false;
    liot_rtos_exit_critical();
    result=demo_ai_audio_prepare_session(&s_lease,volume,mic);if(result){failure_log(FAIL_START,"prepare",result,0,true);return result;}
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
#if DEMO_AI_AUDIO_AEC_ENABLE
        result=dev_capture_start_service();
        if(result){failure_log(FAIL_START,"dev_capture_task",result,0,true);return result;}
#endif
        result=demo_opus_init(&s_dev_opus);
        if(result){failure_log(FAIL_START,"opus",result,0,true);return result;}
    }
    /* Lite DEV submits speech directly; do not queue a separate silent
     * preamble while the full-duplex call is connecting. */
    if(owner!=DEMO_TIRTC_OWNER_DEV_CHAT){
        result=demo_ai_audio_prewarm_session(&s_lease,&extra);if(result){failure_log(FAIL_START,"prewarm",result,0,true);return result;}
    }
    discard_rx();s_guard=extra!=0;s_tail=now()+extra;s_fraction=0;
    s_peak=s_drops=s_played=s_sent=0;s_tx_timestamp=now();s_pause_seen=false;s_last_log=now();
    s_tx_failing=s_record_failing=s_play_failing=s_draining=false;
    liot_rtos_enter_critical();s_applied_revision=revision;s_applied_capture_revision=capture_revision;
    s_mute_pending=false;s_running=true;liot_rtos_exit_critical();
#if DEMO_AI_AUDIO_AEC_ENABLE
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)(void)liot_rtos_semaphore_release(s_dev_capture_wake);
#endif
    return 0;
}
/* RX packet inspection is stateless and does not borrow the decoder. */
static int dev_packet_samples(const TIRTCFRAMEINFO *frame,const void *data)
{
    if(frame->stream_id!=10U || frame->length>DEMO_AI_OPUS_MAX_PACKET) return -1;
    if(frame->media==TIRTC_AUDIO_OPUS && frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C)
        return demo_opus_packet_samples(data,(int)frame->length);
    if(frame->media!=TIRTC_AUDIO_ALAW || frame->length>DEV_ALAW_MAX_BYTES) return -1;
    if(frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C) return (int)frame->length*2;
    return frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C?(int)frame->length:-1;
}

void calls_audio_receive(demo_tirtc_owner_e owner,uint32_t generation,const TIRTCFRAMEINFO *frame,const void *data)
{
    uint32_t write,first,tail;uint8_t rate;bool new_segment;int dev_samples=0;
    if(!frame||!data||!frame->length||frame->length>RX_BYTES)return;
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)dev_samples=dev_packet_samples(frame,data);
    rate=frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C?8:
         frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C?16:0;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)||!s_running||!speaker_allowed_locked()||s_mute_pending||s_draining){liot_rtos_exit_critical();return;}
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        if(dev_samples<=0 || dev_samples>DEMO_OPUS_MAX_DECODE_SAMPLES ||
           s_dev_count==DEV_RX_DEPTH){++s_drops;liot_rtos_exit_critical();return;}
        dev_rx_packet_t *packet=&s_dev_rx[(s_dev_read+s_dev_count)%DEV_RX_DEPTH];
        packet->length=(uint16_t)frame->length;
        packet->samples=(uint16_t)dev_samples;
        packet->media=frame->media;
        packet->flags=frame->flags;
        memcpy(packet->payload,data,frame->length);
        ++s_dev_count;
        s_dev_queued_samples+=(uint32_t)dev_samples;
        if(s_dev_count>s_peak)s_peak=s_dev_count;
        ++s_received;s_received_bytes+=frame->length;
        dev_playout_observe(&s_dev_playout,frame->ts,((uint32_t)dev_samples+15U)/16U,now());
        liot_rtos_exit_critical();
        return;
    }
    if(frame->media!=TIRTC_AUDIO_ALAW||!rate||
       (owner==DEMO_TIRTC_OWNER_WECHAT ? frame->stream_id!=0U :
        (frame->stream_id!=10U&&frame->stream_id!=14U))||frame->length>RX_BYTES-s_count){
        s_drops++;liot_rtos_exit_critical();return;
    }
    tail=(s_segment_head+s_segment_count+RX_SEGMENTS-1U)%RX_SEGMENTS;
    if(owner==DEMO_TIRTC_OWNER_WECHAT&&
       (frame->length>WX_RX_MAX_BYTES||s_segment_count>=WX_RX_PACKETS)){
        s_drops++;liot_rtos_exit_critical();return;
    }
    new_segment=owner==DEMO_TIRTC_OWNER_WECHAT||!s_segment_count||s_segments[tail].rate!=rate;
    if(new_segment&&s_segment_count==RX_SEGMENTS){s_drops++;liot_rtos_exit_critical();return;}
    write=(s_read+s_count)%RX_BYTES;first=RX_BYTES-write;if(first>frame->length)first=frame->length;
    memcpy(s_rx+write,data,first);if(first<frame->length)memcpy(s_rx,(const uint8_t*)data+first,frame->length-first);
    if(new_segment){tail=(s_segment_head+s_segment_count)%RX_SEGMENTS;s_segments[tail].bytes=0;s_segments[tail].rate=rate;s_segment_count++;}
    s_segments[tail].bytes=(uint16_t)(s_segments[tail].bytes+frame->length);
    s_count+=frame->length;if(s_count>s_peak)s_peak=s_count;s_received++;s_received_bytes+=frame->length;
    if(!s_first_rx_logged&&!s_first_rx_pending){s_first_rx_pending=true;s_first_rx_stream=frame->stream_id;s_first_rx_rate=rate;s_first_rx_length=frame->length;}
    liot_rtos_exit_critical();
}
static void log_metrics(demo_tirtc_owner_e owner,uint32_t generation)
{
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)return;
    if((uint32_t)(now()-s_last_log)>=5000U){s_last_log=now();
        TIRTC_LOG_DEBUG("[AUDIO42] owner=%u gen=%lu tx=%lu tx_bytes=%lu rx=%lu rx_bytes=%lu play=%lu q=%lu peak=%lu drop=%lu capture_busy=%u\r\n",
            (unsigned)owner,(unsigned long)generation,(unsigned long)s_sent,(unsigned long)s_sent_bytes,
            (unsigned long)s_received,(unsigned long)s_received_bytes,(unsigned long)s_played,(unsigned long)s_count,
            (unsigned long)s_peak,(unsigned long)s_drops,s_record_busy?1U:0U);}
}
static int apply_controls(void)
{
    uint8_t volume,mic;uint32_t revision,capture_revision,discarded=0;bool busy,mute;int result;
    liot_rtos_enter_critical();revision=s_config_revision;capture_revision=s_capture_revision;
    volume=speaker_allowed_locked()?s_volume:0;mic=capture_level_locked();
    busy=s_record_busy;mute=s_mute_pending;liot_rtos_exit_critical();
    if(mute&&!s_draining){
        if(busy)return DEMO_AI_AUDIO_ERR_BUSY;
        result=demo_ai_audio_set_levels(&s_lease,0,mic);if(result){failure_log(FAIL_CONTROL,"mute",result,0,result!=DEMO_AI_AUDIO_ERR_BUSY);return result;}
        discard_rx();s_drain_since=now();
        liot_rtos_enter_critical();s_draining=true;s_mute_pending=false;liot_rtos_exit_critical();
    }
    if(s_draining){
        if((s_guard&&before(s_tail+120U))||!demo_ai_audio_play_done(&s_lease)){
            if((uint32_t)(now()-s_drain_since)>=3000U)return fatal_error(FAIL_CONTROL,"mute_drain",DEMO_AI_AUDIO_ERR_BUSY,now()-s_drain_since);
            return DEMO_AI_AUDIO_ERR_BUSY;
        }
        liot_rtos_enter_critical();s_draining=false;liot_rtos_exit_critical();
        s_guard=false;s_continuing=false;
    }
    if(revision==s_applied_revision)return 0;
    if(busy)return DEMO_AI_AUDIO_ERR_BUSY;
    result=demo_ai_audio_set_levels(&s_lease,volume,mic);if(result){failure_log(FAIL_CONTROL,"levels",result,0,result!=DEMO_AI_AUDIO_ERR_BUSY);return result;}
    if(capture_revision!=s_applied_capture_revision){
        result=demo_ai_audio_discard_capture_timed(&s_lease,&discarded);
        if(result){failure_log(FAIL_CONTROL,"discard",result,0,result!=DEMO_AI_AUDIO_ERR_BUSY);return result;}
    }
    liot_rtos_enter_critical();s_tx_timestamp+=discarded;
    s_applied_revision=revision;s_applied_capture_revision=capture_revision;
    liot_rtos_exit_critical();
    return 0;
}
/* The control task alone consumes packets. A producer never overwrites a
 * live head slot, including while its decoded PCM waits on the driver. */
static int dev_play_one(void)
{
    dev_rx_packet_t *packet;
    uint32_t count;
    int result;
#if !DEMO_AI_AUDIO_AEC_ENABLE
    if(!demo_ai_audio_play_done(&s_lease))return 0;
#endif
    liot_rtos_enter_critical();
    if(!s_running||!speaker_allowed_locked()||s_mute_pending||s_draining||!s_dev_count){
        liot_rtos_exit_critical();return 0;
    }
    packet=&s_dev_rx[s_dev_read];
    liot_rtos_exit_critical();
    if(!s_dev_pcm_samples){
        if(packet->media==TIRTC_AUDIO_OPUS)
            result=demo_opus_decode(&s_dev_opus,packet->payload,packet->length,
                                    s_dev_pcm,DEMO_OPUS_MAX_DECODE_SAMPLES);
        else if(packet->flags==TIRTC_AUDIOSAMPLE_8K16B1C){
            for(uint32_t i=0;i<packet->length;++i){
                int16_t sample=demo_g711_alaw_decode_sample(packet->payload[i]);
                s_dev_pcm[i*2U]=s_dev_pcm[i*2U+1U]=sample;
            }
            result=packet->samples;
        }else result=(int)demo_g711_alaw_decode(packet->payload,s_dev_pcm,packet->length);
        if(result!=packet->samples){
            liot_rtos_enter_critical();++s_drops;liot_rtos_exit_critical();
            goto next_packet;
        }
        s_dev_pcm_samples=packet->samples;
        if(s_dev_pcm_samples&1U)s_dev_pcm[s_dev_pcm_samples++]=0;
    }
    count=s_dev_pcm_samples-s_dev_pcm_sent;
#if !DEMO_AI_AUDIO_AEC_ENABLE
    if(count>DEMO_AI_AUDIO_FRAME_SAMPLES)count=DEMO_AI_AUDIO_FRAME_SAMPLES;
#endif
    liot_rtos_enter_critical();
    bool allowed=s_running&&speaker_allowed_locked()&&!s_mute_pending&&!s_draining;
    liot_rtos_exit_critical();
    if(!allowed)return 0;
    result=demo_ai_audio_play(&s_lease,s_dev_pcm+s_dev_pcm_sent,count);
#if DEMO_AI_AUDIO_AEC_ENABLE
    if(result==DEMO_AI_AUDIO_ERR_PLAY_BUSY){s_play_failing=false;return 0;}
#endif
    if(result){
        if(!s_play_failing){s_play_failing=true;s_play_failure_at=now();}
        if((uint32_t)(now()-s_play_failure_at)>=200U)
            return fatal_error(FAIL_PLAY,"dev_play",result,now()-s_play_failure_at);
        failure_log(FAIL_PLAY,"dev_play",result,now()-s_play_failure_at,false);
        return 0;
    }
    s_play_failing=false;
    s_dev_pcm_sent+=count;
    if(s_dev_pcm_sent<s_dev_pcm_samples)return 0;
    ++s_played;
next_packet:
    s_dev_pcm_samples=s_dev_pcm_sent=0U;
    liot_rtos_enter_critical();
    s_dev_queued_samples-=packet->samples;
    s_dev_read=(s_dev_read+1U)%DEV_RX_DEPTH;
    --s_dev_count;
    liot_rtos_exit_critical();
    return 1;
}

static int dev_service_downlink(void)
{
    bool done=demo_ai_audio_play_done(&s_lease),ready=true;
    uint32_t start=now();
    liot_rtos_enter_critical();
    if(done&&!s_dev_prebuffering
#if !DEMO_AI_AUDIO_AEC_ENABLE
       && !s_dev_count
#endif
    ){
        s_dev_prebuffering=true;
        if(s_played)dev_playout_underrun(&s_dev_playout);
    }
    if(s_dev_prebuffering)
        ready=dev_playout_should_start(&s_dev_playout,(s_dev_queued_samples+15U)/16U,
                                       s_dev_count,DEV_RX_DEPTH,start);
    if(s_dev_prebuffering && ready && s_dev_count){
        s_dev_prebuffering=false;
        dev_playout_started(&s_dev_playout);
    }
    liot_rtos_exit_critical();
    if(!ready)return 0;
    for(uint32_t i=0;i<DEV_PLAY_BATCH_MAX && (uint32_t)(now()-start)<DEV_PLAY_BUDGET_MS;++i){
        int result=dev_play_one();
        if(result<=0)return result;
    }
    return 0;
}

int calls_audio_step(demo_tirtc_owner_e owner,uint32_t generation)
{
    uint32_t started=now(),count,samples,source,first,extra,duration,fraction,i,queued;int result;bool playing,first_rx;
    uint8_t rate,first_stream,first_rate;uint32_t first_length,packets;
    const bool wechat=owner==DEMO_TIRTC_OWNER_WECHAT;
    const uint32_t prebuffer_ms=wechat?WX_PREBUFFER_MS:PREBUFFER_MS;
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
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        result=dev_service_downlink();
#if !DEMO_AI_AUDIO_AEC_ENABLE
        if(!result)capture_once(owner,generation);
#endif
        return result;
    }
    liot_rtos_enter_critical();playing=speaker_allowed_locked()&&!s_mute_pending&&!s_draining;
    count=s_count;packets=s_segment_count;liot_rtos_exit_critical();
    if(!playing){capture_once(owner,generation);return 0;}
    if(s_continuing&&!before(s_tail)&&demo_ai_audio_play_done(&s_lease)){s_continuing=false;s_prebuffer=false;}
    if(!count){s_prebuffer=false;capture_once(owner,generation);return 0;}
    if(!s_continuing){
        if(!s_prebuffer){s_prebuffer=true;s_prebuffer_at=s_partial?s_partial_at:now();}
        if((wechat?packets<WX_PREBUFFER_PACKETS:count<PREBUFFER_BYTES)&&
           (uint32_t)(now()-s_prebuffer_at)<prebuffer_ms){capture_once(owner,generation);return 0;}
    }
    for(i=0;i<24U&&(uint32_t)(now()-started)<20U;i++){
        liot_rtos_enter_critical();count=s_count;playing=s_running&&speaker_allowed_locked()&&!s_mute_pending&&!s_draining;
        source=s_segment_count?s_segments[s_segment_head].bytes:0;
        rate=s_segment_count?s_segments[s_segment_head].rate:8;
        const uint32_t limit=wechat&&rate==16U?DEMO_AI_AUDIO_FRAME_SAMPLES:CALL_NETWORK_SAMPLES_8K;
        if(source>limit)source=limit;
        samples=rate==16U?source:source*2U;
        /* WX keeps packet boundaries, so pad a lone sample at its tail.
         * LIVE can still pair adjacent samples from coalesced segments. */
        if(!wechat&&samples>1U&&(samples&1U))samples--;
        source=rate==16U?samples:samples/2U;
        first=RX_BYTES-s_read;if(first>source)first=source;
        memcpy(s_alaw,s_rx+s_read,first);if(source>first)memcpy(s_alaw+first,s_rx,source-first);
        liot_rtos_exit_critical();
        if(!playing||!count)break;
        if(!samples){
            /* An empty local PCM block cannot be submitted. */
            if(!s_partial){s_partial=true;s_partial_at=now();}
            if((uint32_t)(now()-s_partial_at)<prebuffer_ms)break;
            liot_rtos_enter_critical();consume_rx_locked(1);s_drops++;liot_rtos_exit_critical();s_partial=false;continue;
        }
        if(!wechat&&samples==1U){if(!s_partial){s_partial=true;s_partial_at=s_prebuffer?s_prebuffer_at:now();}
            if((uint32_t)(now()-s_partial_at)<prebuffer_ms)break;}
        else s_partial=false;
        queued=s_guard&&before(s_tail)?s_tail-now():0;
        duration=(samples+(samples&1U)+(s_continuing?s_fraction:0U))/16U;
        if(queued&&queued+duration+(s_continuing?0U:80U)+1U>PLAY_AHEAD)break;
        demo_g711_alaw_decode(s_alaw,s_pcm,source);
        if(rate==8U){
            uint32_t sample;
            for(sample=source; sample>0U; --sample){
                int16_t value=s_pcm[sample-1U];
                s_pcm[(sample-1U)*2U]=value;
                s_pcm[(sample-1U)*2U+1U]=value;
            }
        }
        if(samples&1U)s_pcm[samples]=0;
        extra=0;result=s_continuing?demo_ai_audio_play(&s_lease,s_pcm,samples+(samples&1U)):
            demo_ai_audio_play_warm(&s_lease,s_pcm,samples+(samples&1U),&extra);
        if(result){
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
        liot_rtos_enter_critical();consume_rx_locked(source);liot_rtos_exit_critical();
    }
    /* Capture after the playback burst in this same task. The call TX task
     * only sends already prepared frames and never touches I2S. */
    capture_once(owner,generation);
    return 0;
}
int calls_audio_stop(demo_tirtc_owner_e owner,uint32_t generation)
{
    int result;bool busy,tx_busy;uint32_t capture_dropped=0;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)){liot_rtos_exit_critical();return 0;}
    s_running=false;busy=s_record_busy;tx_busy=s_tx_busy;liot_rtos_exit_critical();
    if(busy||tx_busy)return DEMO_AI_AUDIO_ERR_BUSY;
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)capture_dropped=demo_ai_audio_capture_dropped(&s_lease);
    result=demo_ai_audio_stop(&s_lease);if(result){failure_log(FAIL_STOP,"stop",result,0,false);return result;}
    result=demo_ai_audio_release(&s_lease);if(result){failure_log(FAIL_STOP,"release",result,0,false);return result;}
    if(s_tx_queue!=NULL){
        call_tx_job_t stale;
        while(liot_rtos_queue_wait(s_tx_queue,(uint8_t *)&stale,sizeof(stale),LIOT_NO_WAIT)==LIOT_OSI_SUCCESS){}
    }
    if(owner==DEMO_TIRTC_OWNER_DEV_CHAT){
        TIRTC_LOG_DEBUG("[AUDIO42] dev_done gen=%lu tx=%lu rx=%lu play=%lu rx_drop=%lu capture_drop=%lu tx_drop=%lu tx_expired=%lu sdk_busy=%lu underrun=%lu buffer_peak_ms=%lu\r\n",
            (unsigned long)generation,(unsigned long)s_sent,(unsigned long)s_received,
            (unsigned long)s_played,(unsigned long)s_drops,(unsigned long)capture_dropped,
            (unsigned long)s_dev_tx_drops,(unsigned long)s_tx_expired,(unsigned long)s_dev_sdk_busy,
            (unsigned long)s_dev_playout.underruns,(unsigned long)s_dev_playout.peak_target_ms);
        demo_opus_deinit(&s_dev_opus);
    }else if(owner==DEMO_TIRTC_OWNER_WECHAT){
        TIRTC_LOG_DEBUG("[AUDIO42] wx_done gen=%lu rx_drop=%lu tx_expired=%lu\r\n",
            (unsigned long)generation,(unsigned long)s_drops,(unsigned long)s_tx_expired);
    }
    discard_rx();s_guard=s_continuing=false;s_owned=false;s_generation=0;return 0;
}
