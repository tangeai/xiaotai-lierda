/* SPDX-License-Identifier: MIT AND Apache-2.0
 * Shared call/LIVE audio: 8k A-law uplink and bounded 8/16k A-law downlink.
 * Playback control never invokes Record. Same retained lease spans both tasks. */
#include "calls_audio.h"
#include "g711_codec.h"
#include "liot_os.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include <string.h>
#define RX_BYTES 15360U
#define RX_SEGMENTS 32U
#define PLAY_AHEAD 480U
#define PREBUFFER_BYTES 1920U
#define PREBUFFER_MS 120U
#define AUDIO_ERROR (-6208)
static liot_task_t s_task;
static bool s_creating, s_running, s_owned, s_record_busy;
static demo_ai_audio_lease_t s_lease=DEMO_AI_AUDIO_LEASE_INIT;
static demo_tirtc_owner_e s_owner;
static uint32_t s_generation;
static uint8_t s_volume=8, s_mic=10;
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
static uint8_t s_rx[RX_BYTES], s_alaw[160], s_capture_alaw[160];
static int16_t s_pcm[160], s_capture[160];
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
enum failure_kind { FAIL_RECORD, FAIL_SEND, FAIL_PLAY, FAIL_CONTROL, FAIL_STOP, FAIL_START, FAIL_COUNT };
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
static void revise_config_locked(void)
{if(!++s_config_revision)++s_config_revision;}
static void discard_rx(void)
{
    liot_rtos_enter_critical();s_read=s_count=0;s_segment_head=s_segment_count=0;
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
static void capture_once(void)
{
    demo_ai_audio_lease_t lease; demo_tirtc_owner_e owner=DEMO_TIRTC_OWNER_NONE; uint32_t generation=0, timestamp=0, revision=0;
    bool allowed, current; int result; TIRTCFRAMEINFO frame;
    liot_rtos_enter_critical();
    allowed=s_running&&s_owned&&!s_error&&!s_record_busy&&microphone_allowed_locked()&&s_config_revision==s_applied_revision;
    if(allowed){
        s_record_busy=true;lease=s_lease;owner=s_owner;generation=s_generation;revision=s_config_revision;
        if(s_pause_seen){s_tx_timestamp+=now()-s_pause_at;s_pause_seen=false;}
        timestamp=s_tx_timestamp;s_tx_timestamp+=20U;
    }else if(!s_pause_seen){s_pause_seen=true;s_pause_at=now();}
    liot_rtos_exit_critical();
    if(!allowed){s_tx_failing=s_record_failing=false;liot_rtos_task_sleep_ms(5);return;}
    result=demo_ai_audio_record_20ms(&lease,s_capture);
    liot_rtos_enter_critical();
    current=s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&s_config_revision==revision;
    liot_rtos_exit_critical();
    if(result && result!=DEMO_AI_AUDIO_ERR_BUSY){
        if(!s_record_failing){s_record_failing=true;s_record_failure_at=now();}
        if((uint32_t)(now()-s_record_failure_at)>=3000U)
            (void)fatal_error(FAIL_RECORD,"record",result,now()-s_record_failure_at);
        else failure_log(FAIL_RECORD,"record",result,now()-s_record_failure_at,false);
    }else if(!result){
        s_record_failing=false;
        if(current){
            demo_g711_alaw_encode(s_capture,s_capture_alaw,160);
            memset(&frame,0,sizeof(frame));frame.stream_id=owner==DEMO_TIRTC_OWNER_WECHAT?0U:10U;frame.media=TIRTC_AUDIO_ALAW;
            frame.flags=TIRTC_AUDIOSAMPLE_8K16B1C;frame.ts=timestamp;frame.length=160;
            liot_rtos_enter_critical();
            current=s_running&&audio_session_locked(owner,generation)&&microphone_allowed_locked()&&s_config_revision==revision;
            liot_rtos_exit_critical();
            if(!current){s_tx_failing=false;liot_rtos_enter_critical();s_record_busy=false;liot_rtos_exit_critical();return;}
            result=demo_tirtc_send_audio(owner,generation,&frame,s_capture_alaw);
            /* SDK contract: zero accepted no data; positive is local acceptance,
             * not confirmation that the peer received the frame. */
            if(result>0){
                s_sent++;s_sent_bytes+=frame.length;s_tx_failing=false;
                if(!s_first_tx_logged){s_first_tx_logged=true;
                    TIRTC_LOG_DEBUG("[AUDIO42] first_tx owner=%u gen=%lu stream=%u rate=8000 bytes=%lu\r\n",
                        (unsigned)owner,(unsigned long)generation,(unsigned)frame.stream_id,(unsigned long)frame.length);}
            }
            else {
                if(!s_tx_failing){s_tx_failing=true;s_tx_failure_at=now();}
                if((uint32_t)(now()-s_tx_failure_at)>=3000U)
                    (void)fatal_error(FAIL_SEND,"send",result,now()-s_tx_failure_at);
                else failure_log(FAIL_SEND,"send",result,now()-s_tx_failure_at,false);
            }
        }else s_tx_failing=false;
    }
    liot_rtos_enter_critical();s_record_busy=false;liot_rtos_exit_critical();
    if(result<=0)liot_rtos_task_sleep_ms(5);
}
static void capture_worker(void *arg){(void)arg;for(;;)capture_once();}
int calls_audio_start_service(void)
{
    liot_task_t task=NULL;int result;
    liot_rtos_enter_critical();if(s_task||s_creating){liot_rtos_exit_critical();return 0;}s_creating=true;liot_rtos_exit_critical();
    result=(int)liot_rtos_task_create(&task,8U*1024U,12,"call_capture",capture_worker,NULL);
    liot_rtos_enter_critical();s_creating=false;if(!result&&task)s_task=task;liot_rtos_exit_critical();
    return !result&&task ? 0 : -1;
}
int calls_audio_start(demo_tirtc_owner_e owner,uint32_t generation)
{
    demo_ai_audio_owner_e audio_owner;uint8_t volume,mic;uint32_t revision,extra=0;int result;
    if(!generation||!s_task||s_owned)return DEMO_AI_AUDIO_ERR_BUSY;
    if(owner==DEMO_TIRTC_OWNER_WECHAT)audio_owner=DEMO_AI_AUDIO_OWNER_WECHAT;
    else if(owner==DEMO_TIRTC_OWNER_DEV_CHAT)audio_owner=DEMO_AI_AUDIO_OWNER_DEVICE;
    else if(owner==DEMO_TIRTC_OWNER_LIVE)audio_owner=DEMO_AI_AUDIO_OWNER_LIVE;
    else return DEMO_AI_AUDIO_ERR_INVALID_LEASE;
    memset(s_failure_log,0,sizeof(s_failure_log));
    result=demo_ai_audio_acquire(audio_owner,&s_lease);if(result){failure_log(FAIL_START,"acquire",result,0,true);return result;}
    liot_rtos_enter_critical();
    s_owned=true;s_owner=owner;s_generation=generation;s_running=false;s_error=0;
    s_session_speaker=true;s_session_microphone=owner!=DEMO_TIRTC_OWNER_LIVE;
    s_uplink_subscribed=owner!=DEMO_TIRTC_OWNER_LIVE;revise_config_locked();
    volume=speaker_allowed_locked()?s_volume:0;mic=microphone_allowed_locked()?s_mic:0;revision=s_config_revision;
    s_received=s_received_bytes=s_sent_bytes=0;
    s_first_rx_pending=s_first_rx_logged=s_first_tx_logged=false;
    liot_rtos_exit_critical();
    result=demo_ai_audio_prepare_session(&s_lease,volume,mic);if(result){failure_log(FAIL_START,"prepare",result,0,true);return result;}
    result=demo_ai_audio_prewarm_session(&s_lease,&extra);if(result){failure_log(FAIL_START,"prewarm",result,0,true);return result;}
    discard_rx();s_guard=extra!=0;s_tail=now()+extra;s_fraction=0;
    s_peak=s_drops=s_played=s_sent=0;s_tx_timestamp=now();s_pause_seen=false;s_last_log=now();
    s_tx_failing=s_record_failing=s_play_failing=s_draining=false;
    liot_rtos_enter_critical();s_applied_revision=revision;s_mute_pending=false;s_running=true;liot_rtos_exit_critical();
    return 0;
}
void calls_audio_receive(demo_tirtc_owner_e owner,uint32_t generation,const TIRTCFRAMEINFO *frame,const void *data)
{
    uint32_t write,first,tail;uint8_t rate;bool new_segment;
    if(!frame||!data||!frame->length||frame->length>RX_BYTES)return;
    rate=frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C?8:
         frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C?16:0;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)||!s_running||!speaker_allowed_locked()||s_mute_pending||s_draining){liot_rtos_exit_critical();return;}
    if(frame->media!=TIRTC_AUDIO_ALAW||!rate||
       (owner==DEMO_TIRTC_OWNER_LIVE ? (frame->stream_id!=10U&&frame->stream_id!=14U) :
        (rate!=8U||frame->stream_id!=(owner==DEMO_TIRTC_OWNER_WECHAT?0U:10U)))||frame->length>RX_BYTES-s_count){
        s_drops++;liot_rtos_exit_critical();return;
    }
    tail=(s_segment_head+s_segment_count+RX_SEGMENTS-1U)%RX_SEGMENTS;
    new_segment=!s_segment_count||s_segments[tail].rate!=rate;
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
    if((uint32_t)(now()-s_last_log)>=5000U){s_last_log=now();
        TIRTC_LOG_DEBUG("[AUDIO42] owner=%u gen=%lu tx=%lu tx_bytes=%lu rx=%lu rx_bytes=%lu play=%lu q=%lu peak=%lu drop=%lu capture_busy=%u\r\n",
            (unsigned)owner,(unsigned long)generation,(unsigned long)s_sent,(unsigned long)s_sent_bytes,
            (unsigned long)s_received,(unsigned long)s_received_bytes,(unsigned long)s_played,(unsigned long)s_count,
            (unsigned long)s_peak,(unsigned long)s_drops,s_record_busy?1U:0U);}
}
static int apply_controls(void)
{
    uint8_t volume,mic;uint32_t revision,discarded=0;bool busy,mute;int result;
    liot_rtos_enter_critical();revision=s_config_revision;volume=speaker_allowed_locked()?s_volume:0;mic=microphone_allowed_locked()?s_mic:0;
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
    if(!playing)return 0;
    if(s_continuing&&!before(s_tail)&&demo_ai_audio_play_done(&s_lease)){s_continuing=false;s_prebuffer=false;}
    if(!count){s_prebuffer=false;return 0;}
    if(!s_continuing){
        if(!s_prebuffer){s_prebuffer=true;s_prebuffer_at=s_partial?s_partial_at:now();}
        if(count<PREBUFFER_BYTES&&(uint32_t)(now()-s_prebuffer_at)<PREBUFFER_MS)return 0;
    }
    for(i=0;i<24U&&(uint32_t)(now()-started)<20U;i++){
        liot_rtos_enter_critical();count=s_count;playing=s_running&&speaker_allowed_locked()&&!s_mute_pending&&!s_draining;
        source=s_segment_count?s_segments[s_segment_head].bytes:0;
        rate=s_segment_count?s_segments[s_segment_head].rate:8;
        if(source>160U)source=160U;
        samples=rate==16U?source/2U:source;
        if(samples>1U&&(samples&1U))samples--;
        source=samples*(rate==16U?2U:1U);
        first=RX_BYTES-s_read;if(first>source)first=source;
        memcpy(s_alaw,s_rx+s_read,first);if(source>first)memcpy(s_alaw+first,s_rx,source-first);
        liot_rtos_exit_critical();
        if(!playing||!count)break;
        if(!samples){
            /* One 16-kHz A-law byte cannot form an 8-kHz sample. Wait for
             * the matching byte; drop the orphan after the bounded tail wait. */
            if(!s_partial){s_partial=true;s_partial_at=now();}
            if((uint32_t)(now()-s_partial_at)<PREBUFFER_MS)break;
            liot_rtos_enter_critical();consume_rx_locked(1);s_drops++;liot_rtos_exit_critical();s_partial=false;continue;
        }
        if(samples==1U){if(!s_partial){s_partial=true;s_partial_at=s_prebuffer?s_prebuffer_at:now();}
            if((uint32_t)(now()-s_partial_at)<PREBUFFER_MS)break;}
        else s_partial=false;
        queued=s_guard&&before(s_tail)?s_tail-now():0;
        duration=(samples+(samples&1U)+(s_continuing?s_fraction:0U))/8U;
        if(queued&&queued+duration+(s_continuing?0U:80U)+1U>PLAY_AHEAD)break;
        demo_g711_alaw_decode(s_alaw,s_pcm,source);
        if(rate==16U){uint32_t sample;for(sample=0;sample<samples;sample++)
            s_pcm[sample]=(int16_t)(((int32_t)s_pcm[sample*2U]+s_pcm[sample*2U+1U])/2);}
        if(samples&1U)s_pcm[samples]=0;
        extra=0;result=s_continuing?demo_ai_audio_play(&s_lease,s_pcm,samples+(samples&1U)):
            demo_ai_audio_play_warm(&s_lease,s_pcm,samples+(samples&1U),&extra);
        if(result){
            if(result==DEMO_AI_AUDIO_ERR_WARM_PARTIAL)return fatal_error(FAIL_PLAY,"warm_partial",result,0);
            if(!s_play_failing){s_play_failing=true;s_play_failure_at=now();}
            if((uint32_t)(now()-s_play_failure_at)>=200U)return fatal_error(FAIL_PLAY,"play",result,now()-s_play_failure_at);
            failure_log(FAIL_PLAY,"play",result,now()-s_play_failure_at,false);
            break;
        }
        fraction=samples+(samples&1U)+(s_continuing?s_fraction:0U);
        if(!s_guard||!before(s_tail))s_tail=now();
        s_tail+=duration+extra;s_fraction=(uint8_t)(fraction%8U);
        s_guard=s_continuing=true;s_prebuffer=s_partial=s_play_failing=false;s_played++;
        liot_rtos_enter_critical();consume_rx_locked(source);liot_rtos_exit_critical();
    }
    return 0;
}
int calls_audio_stop(demo_tirtc_owner_e owner,uint32_t generation)
{
    int result;bool busy;
    liot_rtos_enter_critical();
    if(!audio_session_locked(owner,generation)){liot_rtos_exit_critical();return 0;}
    s_running=false;busy=s_record_busy;liot_rtos_exit_critical();
    if(busy)return DEMO_AI_AUDIO_ERR_BUSY;
    result=demo_ai_audio_stop(&s_lease);if(result){failure_log(FAIL_STOP,"stop",result,0,false);return result;}
    result=demo_ai_audio_release(&s_lease);if(result){failure_log(FAIL_STOP,"release",result,0,false);return result;}
    discard_rx();s_guard=s_continuing=false;s_owned=false;s_generation=0;return 0;
}
