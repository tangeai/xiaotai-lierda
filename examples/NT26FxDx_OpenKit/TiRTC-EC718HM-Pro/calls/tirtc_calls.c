/* SPDX-License-Identifier: MIT
 * Calls are three owners: bounded control/playback, blocking capture, and
 * blocking service requests. No SDK callback performs HTTP, codec or UI work. */
#include "tirtc_calls.h"
#include "calls_audio.h"
#include "calls_protocol.h"
#include "../ai/tirtc_ai.h"
#ifdef HWDEMO_GROUP_ROOM_EN
#include "../group/tirtc_group.h"
#endif
#include "../video/tirtc_video.h"
#include "../platform/json_guard.h"
#include "liot_os.h"
#include "liot_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define CALL_ERROR (-6301)
#define CALL_TIMEOUT (-6302)
#define CALL_STALE (-6303)
#define CALL_CLEANUP TIRTC_CALL_CLEANUP_PENDING
#define CALL_RETRY (-6305)
#define EVENT_COUNT 4U /* Three invitations, one callback-only terminal scratch. */
#define PENDING_CANCEL_COUNT 4U
#define JOB_COUNT 2U
#define CLEANUP_LIMIT 15000U
/* Only the control task changes phases. Callback fields are latched under the
 * same critical section used by control when closing/adopting an attempt. */
typedef enum { C_IDLE,C_OWNER,C_DIAL,C_RING,C_WX_NOTIFY,C_CONNECT,
 C_CONFIRM,C_ACTIVE,C_CLOSING,C_BLOCKED } phase_t;
typedef enum { J_NONE,J_PROFILE,J_DEV_DIAL,J_WX_DIAL,J_INFO,J_ROOM_GET,
 J_ROOM_RECOVER,J_ROOM_END,J_WX_REJECT } job_kind_t;
typedef struct {
 unsigned used; job_kind_t kind; uint32_t session;
 demo_binding_service_context_t context;
 tirtc_contact_t contact;
 calls_proto_event_t event;
 calls_proto_dial_t dial;
 calls_proto_access_t access;
 calls_proto_room_t room_info;
 char room[129],device[64]; bool video;
 calls_proto_room_action_t action; unsigned reason;
 int result;
} job_t;
typedef struct { unsigned used; bool cancelled; calls_proto_event_t event; demo_binding_service_context_t context; } mqtt_event_t;
static liot_task_t s_task,s_http_task;
static bool s_creating,s_http_creating,s_registered;
static phase_t s_phase;
static tirtc_calls_snapshot_t s_ui;
static uint32_t s_next_session,s_session,s_generation,s_started,s_phase_at,s_last_publish;
static bool s_owned,s_listener_live,s_callbacks,s_connect_done,s_peer_answered,s_confirm_pending;
static bool s_stop_requested,s_accept_requested,s_video_dirty,s_pending_dial;
static bool s_server_answered,s_room_owned,s_dial_submitted,s_end_pending,s_end_queued,s_remote_end,s_recovering,s_redialed;
static bool s_hangup_sent,s_audio_started,s_video_started;
#if TIRTC_F9_AV_FOCUS
static bool s_av_focus_control;
static uint8_t s_av_focus_saved_priority;
#endif
static int s_connect_error,s_lost,s_final_error;
static tirtc_conn_t s_connection,s_dev_confirmed;
static char s_confirm[192],s_room[129],s_answer_room[129],s_call_id[65],s_self[64];
static struct {char room[129],call_id[65];} s_pending_cancels[PENDING_CANCEL_COUNT];
static unsigned s_pending_cancel_next;
static uint32_t s_confirm_length,s_cleanup_at,s_cleanup_next;
static demo_tirtc_owner_e s_owner;
static demo_binding_service_context_t s_context,s_profile_context;
static bool s_profile_valid,s_profile_pending;
static uint32_t s_profile_retry;
static tirtc_contact_t s_contact,s_pending_contact;
static bool s_pending_video;
static calls_proto_event_t s_invite;
static mqtt_event_t s_events[EVENT_COUNT];
static job_t s_jobs[JOB_COUNT];
static uint32_t s_event_drops;
static char s_wx_closed_room[129],s_wx_closed_call[65];
static demo_binding_service_context_t s_wx_closed_context;
static uint32_t s_wx_tombstone_until,s_wx_unassociated_until;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
#define CALL_SDK_REPORT_MS 10000U
typedef struct {
 uint32_t seen, forwarded, invalid, last_arrival, max_arrival_gap;
} call_sdk_rx_t;
typedef struct {
 uint32_t last,short_step,fps12_step,fps9_step,mid_step,long_step,nonmonotonic;
 bool seen;
} call_video_pts_t;
static struct {
 call_sdk_rx_t audio, video;
 call_video_pts_t video_pts;
 uint32_t audio_pts,audio_duration_ms,audio_pts_pairs,audio_pts_gap_ms,audio_pts_gaps,audio_pts_nonmonotonic;
 demo_tirtc_transport_send_error_stats_t transport_start;
 uint32_t started, last_report;
 bool audio_pts_seen, active;
} s_sdk_media;
#endif
static uint32_t tick(void){return liot_rtos_get_running_time();}
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
/* SDK listener callbacks are already serialized with lifecycle changes by
 * the call lock. Keep only counters here and print from the control task. */
static void call_sdk_rx_observe(call_sdk_rx_t *rx, bool forwarded,
                                const TIRTCFRAMEINFO *frame, const void *data)
{
 uint32_t at;
 rx->seen++;
 if(!forwarded)return;
 if(!frame||!data||!frame->length)rx->invalid++;
 at=tick();
 if(rx->forwarded){
  const uint32_t gap=at-rx->last_arrival;
  if(gap>rx->max_arrival_gap)rx->max_arrival_gap=gap;
 }
 rx->last_arrival=at;
 rx->forwarded++;
}
static void call_sdk_audio_pts(const TIRTCFRAMEINFO *frame,const void *data)
{
 uint32_t bytes_per_ms,duration_ms;
 int32_t step;
 if(!frame||!data||!frame->length||frame->stream_id!=(s_ui.wechat?0U:10U)||
    !TIRTC_IS_AUDIO(frame->media))return;
 bytes_per_ms=frame->flags==TIRTC_AUDIOSAMPLE_8K16B1C?8U:
              frame->flags==TIRTC_AUDIOSAMPLE_16K16B1C?16U:0U;
 if(frame->media==TIRTC_AUDIO_PCM)bytes_per_ms*=2U;
 if(frame->media!=TIRTC_AUDIO_PCM&&frame->media!=TIRTC_AUDIO_ALAW)bytes_per_ms=0U;
 duration_ms=(bytes_per_ms&&frame->length%bytes_per_ms==0U)?frame->length/bytes_per_ms:0U;
 if(s_sdk_media.audio_pts_seen){
  step=(int32_t)(frame->ts-s_sdk_media.audio_pts);
  if(step<=0){s_sdk_media.audio_pts_nonmonotonic++;return;}
  if(duration_ms&&s_sdk_media.audio_duration_ms){
   s_sdk_media.audio_pts_pairs++;
   if((uint32_t)step>s_sdk_media.audio_duration_ms){
    s_sdk_media.audio_pts_gap_ms+=(uint32_t)step-s_sdk_media.audio_duration_ms;
    s_sdk_media.audio_pts_gaps++;
   }
  }
 }
 s_sdk_media.audio_pts=frame->ts;
 s_sdk_media.audio_duration_ms=duration_ms;
 s_sdk_media.audio_pts_seen=true;
}
/* RTP/bridge timestamps at the device SDK boundary may differ from the
 * sender's clock. Interval counts distinguish pacing from sparse arrivals. */
static void call_sdk_video_pts(const TIRTCFRAMEINFO *frame,const void *data)
{
 call_video_pts_t *pts=&s_sdk_media.video_pts;
 if(!frame||!data||frame->length<4U||frame->media!=TIRTC_VIDEO_JPEG||
    frame->stream_id!=(s_ui.wechat?1U:11U))return;
 if(pts->seen){
  int32_t step=(int32_t)(frame->ts-pts->last);
  if(step<=0){pts->nonmonotonic++;return;}
  if(step<70)pts->short_step++;
  else if(step<=95)pts->fps12_step++;
  else if(step<=130)pts->fps9_step++;
  else if(step<=180)pts->mid_step++;
  else pts->long_step++;
 }
 pts->last=frame->ts;
 pts->seen=true;
}
static void call_sdk_media_report(bool final)
{
 uint32_t at=tick(),elapsed,audio_pts_pairs,audio_pts_gaps,audio_pts_gap_ms,audio_pts_nonmonotonic;
 call_sdk_rx_t audio,video;
 call_video_pts_t video_pts;
 demo_tirtc_transport_send_error_stats_t transport;
 if(!s_sdk_media.active||(!final&&at-s_sdk_media.last_report<CALL_SDK_REPORT_MS))return;
 s_sdk_media.last_report=at;
 liot_rtos_enter_critical();
 at=tick();
 elapsed=at-s_sdk_media.started;
 audio=s_sdk_media.audio;
 video=s_sdk_media.video;
 video_pts=s_sdk_media.video_pts;
 audio_pts_pairs=s_sdk_media.audio_pts_pairs;
 audio_pts_gaps=s_sdk_media.audio_pts_gaps;
 audio_pts_gap_ms=s_sdk_media.audio_pts_gap_ms;
 audio_pts_nonmonotonic=s_sdk_media.audio_pts_nonmonotonic;
 liot_rtos_exit_critical();
 liot_trace("[CALL-SDK] id=%lu gen=%lu wx=%u final=%u ms=%lu "
  "a_seen=%lu a_fwd=%lu a_invalid=%lu a_gap_max_ms=%lu a_idle_ms=%lu "
  "v_seen=%lu v_fwd=%lu v_invalid=%lu v_gap_max_ms=%lu v_idle_ms=%lu "
  "a_pts_pairs=%lu a_pts_gaps=%lu a_pts_gap_ms=%lu a_pts_nonmonotonic=%lu\r\n",
  (unsigned long)s_session,(unsigned long)s_generation,s_ui.wechat?1U:0U,final?1U:0U,
  (unsigned long)elapsed,(unsigned long)audio.seen,(unsigned long)audio.forwarded,
  (unsigned long)audio.invalid,(unsigned long)audio.max_arrival_gap,
  (unsigned long)(audio.forwarded&&(int32_t)(at-audio.last_arrival)>=0?at-audio.last_arrival:0U),
  (unsigned long)video.seen,(unsigned long)video.forwarded,(unsigned long)video.invalid,
  (unsigned long)video.max_arrival_gap,
  (unsigned long)(video.forwarded&&(int32_t)(at-video.last_arrival)>=0?at-video.last_arrival:0U),
  (unsigned long)audio_pts_pairs,(unsigned long)audio_pts_gaps,
  (unsigned long)audio_pts_gap_ms,(unsigned long)audio_pts_nonmonotonic);
 if(s_ui.type==TIRTC_CALL_VIDEO)
  liot_trace("[CALL-VPTS] gen=%lu final=%u lt70=%lu from70to95=%lu from96to130=%lu from131to180=%lu gt180=%lu nonmonotonic=%lu\r\n",
   (unsigned long)s_generation,final?1U:0U,(unsigned long)video_pts.short_step,
   (unsigned long)video_pts.fps12_step,(unsigned long)video_pts.fps9_step,
   (unsigned long)video_pts.mid_step,(unsigned long)video_pts.long_step,
   (unsigned long)video_pts.nonmonotonic);
 demo_tirtc_get_transport_send_error_stats(&transport);
 liot_trace("[CALL-SEND] gen=%lu final=%u ms=%lu sdk_send_events=%lu e_minus_one=%lu e_six=%lu e_other=%lu saturated=%u\r\n",
  (unsigned long)s_generation,final?1U:0U,(unsigned long)elapsed,
  (unsigned long)(transport.matched-s_sdk_media.transport_start.matched),
  (unsigned long)(transport.errno_minus_one-s_sdk_media.transport_start.errno_minus_one),
  (unsigned long)(transport.errno_six-s_sdk_media.transport_start.errno_six),
  (unsigned long)(transport.errno_other-s_sdk_media.transport_start.errno_other),
  transport.matched==UINT32_MAX?1U:0U);
 if(final)s_sdk_media.active=false;
}
#endif
static bool due(uint32_t when){return (int32_t)(tick()-when)>=0;}
static void copy(char *out,size_t size,const char *in){if(size){snprintf(out,size,"%s",in?in:"");}}
static bool context_equal(const demo_binding_service_context_t*a,const demo_binding_service_context_t*b)
{return a->binding_generation==b->binding_generation&&a->credential_epoch==b->credential_epoch&&a->route_generation==b->route_generation&&a->sim_id==b->sim_id;}
static void set_phase(phase_t phase){liot_rtos_enter_critical();s_phase=phase;s_phase_at=tick();liot_rtos_exit_critical();}
static void publish(tirtc_ui_call_state_t state,const char *message,int error)
{
 tirtc_ui_call_t ui;bool changed;
 liot_rtos_enter_critical();changed=s_ui.state!=state||s_ui.error!=error;s_ui.state=state;s_ui.error=error;copy(s_ui.message,sizeof(s_ui.message),message);s_ui.revision++;
 memset(&ui,0,sizeof(ui));ui.state=s_ui.state;ui.type=s_ui.type;ui.session_id=s_ui.session_id;ui.seconds=s_ui.seconds;ui.revision=s_ui.revision;
 ui.incoming=s_ui.incoming;ui.wechat=s_ui.wechat;ui.video_enabled=s_ui.video_enabled;ui.error=s_ui.error;
 copy(ui.peer_id,sizeof(ui.peer_id),s_ui.peer_id);copy(ui.peer_name,sizeof(ui.peer_name),s_ui.peer_name);copy(ui.message,sizeof(ui.message),s_ui.message);
 liot_rtos_exit_critical();(void)tirtc_ui_publish_call(&ui);s_last_publish=tick();
 if(changed)liot_trace("[CALL22] state id=%lu state=%u error=%d\r\n",(unsigned long)ui.session_id,(unsigned)state,error);
}
void tirtc_calls_get_snapshot(tirtc_calls_snapshot_t*out){if(out){liot_rtos_enter_critical();*out=s_ui;liot_rtos_exit_critical();}}
bool tirtc_calls_has_session(void){bool result;liot_rtos_enter_critical();result=s_phase!=C_IDLE||s_pending_dial;liot_rtos_exit_critical();return result;}
void tirtc_calls_set_audio_config(uint8_t volume,uint8_t mic,bool speaker,bool microphone){calls_audio_set_config(volume,mic,speaker,microphone);}
static bool callback_current(tirtc_conn_t conn){return s_callbacks&&s_owned&&conn&&conn==s_connection&&s_phase!=C_CLOSING&&s_phase!=C_BLOCKED;}
static void connect_callback(int error,tirtc_conn_t conn,void*arg)
{
 liot_rtos_enter_critical();
 if(s_callbacks&&s_phase==C_CONNECT&&(uint32_t)(uintptr_t)arg==s_session){s_connect_done=true;s_connect_error=error?error:(conn?0:CALL_ERROR);if(!error&&conn)s_connection=conn;}
 liot_rtos_exit_critical();
}
static int accepted(tirtc_conn_t conn)
{
 int result=-1;liot_rtos_enter_critical();
 if(s_callbacks&&s_owned&&s_owner==DEMO_TIRTC_OWNER_DEV_CHAT&&!s_ui.incoming&&
    (s_phase==C_DIAL||s_phase==C_CONFIRM)&&conn&&!s_connection&&!s_stop_requested){s_connection=conn;s_connect_done=true;result=0;}
 liot_rtos_exit_critical();return result;
}
static void connection_error(tirtc_conn_t conn,int error){liot_rtos_enter_critical();if(callback_current(conn))s_lost=error?error:CALL_ERROR;liot_rtos_exit_critical();}
static void disconnected(tirtc_conn_t conn){connection_error(conn,CALL_ERROR);}
static void audio_received(tirtc_conn_t conn,const TIRTCFRAMEINFO*frame,void*data)
{
 uint32_t generation=0;demo_tirtc_owner_e owner=DEMO_TIRTC_OWNER_NONE;
 liot_rtos_enter_critical();
 if(callback_current(conn)){
  if(s_phase==C_ACTIVE){generation=s_generation;owner=s_owner;}
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
  call_sdk_rx_observe(&s_sdk_media.audio,generation!=0,frame,data);
  if(generation)call_sdk_audio_pts(frame,data);
#endif
 }
 liot_rtos_exit_critical();
 if(generation)calls_audio_receive(owner,generation,frame,data);
}
static void video_received(tirtc_conn_t conn,const TIRTCFRAMEINFO*frame,void*data)
{
 uint32_t generation=0;
 liot_rtos_enter_critical();
 if(callback_current(conn)){
  if(s_phase==C_ACTIVE&&s_ui.type==TIRTC_CALL_VIDEO)generation=s_generation;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
  call_sdk_rx_observe(&s_sdk_media.video,generation!=0,frame,data);
  if(generation)call_sdk_video_pts(frame,data);
#endif
 }
 liot_rtos_exit_critical();
 if(generation)tirtc_video_receive(generation,frame,data);
}
static bool is_command(uint32_t word,uint16_t command){return word==command||(word&0xffffU)==command||(word&0x7fffU)==command;}
static void command_received(tirtc_conn_t conn,uint32_t word,const void*data,uint32_t length)
{
 liot_rtos_enter_critical();
 if(s_callbacks&&s_owned&&s_phase==C_CONNECT&&s_ui.wechat&&!s_connection&&conn){s_connection=conn;}
 if(callback_current(conn)){
  if(is_command(word,0x2001)){s_remote_end=true;s_stop_requested=true;}
  else if(is_command(word,0x2000)&&(length||s_ui.wechat||s_ui.incoming)&&length<sizeof(s_confirm)&&(!length||(data&&!memchr(data,0,length)))){
   if(length){memcpy(s_confirm,data,length);}s_confirm[length]=0;s_confirm_length=length;s_confirm_pending=true;
  }
 }
 liot_rtos_exit_critical();
}
/* The reverse RTC connection can subscribe before dial HTTP returns its room.
 * Audio and video delivery still require C_ACTIVE and the matching handle. */
static bool can_subscribe_locked(tirtc_conn_t conn)
{
 return callback_current(conn)&&(s_phase==C_ACTIVE||s_phase==C_CONFIRM||s_phase==C_CONNECT||
        (s_phase==C_DIAL&&!s_ui.wechat&&!s_ui.incoming&&!s_stop_requested));
}
static int subscribe_audio(tirtc_conn_t conn,uint8_t stream)
{bool ok;liot_rtos_enter_critical();ok=can_subscribe_locked(conn)&&stream==(s_ui.wechat?0U:10U);liot_rtos_exit_critical();return ok?0:-1;}
static int subscribe_video(tirtc_conn_t conn,uint8_t stream)
{bool ok;liot_rtos_enter_critical();ok=can_subscribe_locked(conn)&&s_ui.type==TIRTC_CALL_VIDEO&&stream==(s_ui.wechat?1U:11U);liot_rtos_exit_critical();return ok?0:-1;}
static const demo_tirtc_listener_t s_listener={.on_conn_accepted=accepted,.on_conn_error=connection_error,.on_disconnected=disconnected,
 .on_audio=audio_received,.on_command=command_received,.on_subscribe_audio=subscribe_audio,.on_video=video_received,.on_subscribe_video=subscribe_video};
/* A room-only cancellation may precede the HTTP response and the invitation. */
static void remember_cancel_locked(const calls_proto_event_t*event)
{
 unsigned slot=s_pending_cancel_next;
 for(unsigned i=0;i<PENDING_CANCEL_COUNT;++i)
  if(!strcmp(s_pending_cancels[i].room,event->room)&&!strcmp(s_pending_cancels[i].call_id,event->call_id))return;
 copy(s_pending_cancels[slot].room,sizeof(s_pending_cancels[slot].room),event->room);
 copy(s_pending_cancels[slot].call_id,sizeof(s_pending_cancels[slot].call_id),event->call_id);
 s_pending_cancel_next=(slot+1U)%PENDING_CANCEL_COUNT;
}
static bool pending_cancel_matches_locked(void)
{
 for(unsigned i=0;i<PENDING_CANCEL_COUNT;++i)
  if((s_room[0]&&!strcmp(s_room,s_pending_cancels[i].room))||
     (s_ui.wechat&&s_call_id[0]&&!strcmp(s_call_id,s_pending_cancels[i].call_id)))return true;
 return false;
}
/* Ready invitations stay immutable until the single control task retires them. */
static void cancel_queued_invites_locked(const mqtt_event_t*signal)
{
 const calls_proto_event_t*end=&signal->event;
 for(unsigned i=0;i<EVENT_COUNT-1U;i++){
  mqtt_event_t*slot=&s_events[i];const calls_proto_event_t*invite=&slot->event;
  if(slot->used!=1||invite->wechat!=end->wechat||!context_equal(&slot->context,&signal->context))continue;
  if(end->room[0]&&strcmp(end->room,invite->room))continue;
  if(end->call_id[0]&&invite->call_id[0]&&strcmp(end->call_id,invite->call_id))continue;
  if(end->room[0]||(end->call_id[0]&&invite->call_id[0]))slot->cancelled=true;
 }
}
static void mqtt_received(demo_formal_mqtt_message_kind_e kind,const char*type,const char*channel,const cJSON*payload,void*arg)
{
 unsigned i,limit;int result;bool signal=type&&strcmp(type,"call_incoming");(void)arg;
 liot_rtos_enter_critical();
 i=signal?EVENT_COUNT-1U:0U;limit=signal?EVENT_COUNT:EVENT_COUNT-1U;
 while(i<limit&&s_events[i].used)i++;
 if(i<limit){s_events[i].used=2;s_events[i].cancelled=false;}else s_event_drops++;
 liot_rtos_exit_critical();if(i==limit)return;
 result=calls_proto_parse_mqtt(kind,type,channel,payload,&s_events[i].event);
 if(result==1&&!demo_binding_service_context(&s_events[i].context))result=0;
 liot_rtos_enter_critical();
 if(result==1&&signal){
  const calls_proto_event_t*e=&s_events[i].event;
  bool current=s_phase!=C_IDLE&&s_phase!=C_CLOSING&&s_phase!=C_BLOCKED&&context_equal(&s_events[i].context,&s_context)&&e->wechat==s_ui.wechat;
  bool match=current&&((s_room[0]&&e->room[0]&&!strcmp(s_room,e->room))||
    (e->wechat&&s_call_id[0]&&e->call_id[0]&&!strcmp(s_call_id,e->call_id)));
  if(e->type==CALLS_PROTO_CANCEL&&s_phase==C_IDLE)cancel_queued_invites_locked(&s_events[i]);
  if(e->type==CALLS_PROTO_CANCEL&&match){s_remote_end=true;s_stop_requested=true;}
  else if(e->type==CALLS_PROTO_CANCEL&&current&&!s_room[0]&&
          (s_phase==C_DIAL||(e->wechat&&s_phase==C_WX_NOTIFY))){
   remember_cancel_locked(e);
  }else if(e->type==CALLS_PROTO_ANSWERED&&match)s_peer_answered=true;
  else if(e->type==CALLS_PROTO_ANSWERED&&current&&s_phase==C_DIAL&&!s_room[0])copy(s_answer_room,sizeof(s_answer_room),e->room);
  result=0;
 }
 s_events[i].used=result==1?1U:0U;liot_rtos_exit_critical();
}
/* A job slot remains immutable from QUEUED through RUNNING; only its worker
 * writes result storage. Control never recycles an in-flight HTTP context. */
static bool job_free(unsigned slot){bool free;liot_rtos_enter_critical();free=!s_jobs[slot].used;liot_rtos_exit_critical();return free;}
static bool queue_job_action(unsigned slot,job_kind_t kind,calls_proto_room_action_t action,unsigned reason)
{
 job_t*j=&s_jobs[slot];
 liot_rtos_enter_critical();if(j->used){liot_rtos_exit_critical();return false;}j->used=4;liot_rtos_exit_critical();
 memset((char*)j+sizeof(j->used),0,sizeof(*j)-sizeof(j->used));j->kind=kind;j->session=s_session;j->context=s_context;
 j->contact=s_contact;j->event=s_invite;copy(j->room,sizeof(j->room),s_room);copy(j->device,sizeof(j->device),s_self);j->video=s_ui.type==TIRTC_CALL_VIDEO;j->action=action;j->reason=reason;
 liot_rtos_enter_critical();j->used=1;liot_rtos_exit_critical();return true;
}
static bool queue_job(unsigned slot,job_kind_t kind){return queue_job_action(slot,kind,CALLS_PROTO_ROOM_REJECT,7);}
static void http_once(void)
{
 unsigned i;job_t*j=NULL;bool cancel;uint32_t started;
 liot_rtos_enter_critical();for(i=0;i<JOB_COUNT;i++)if(s_jobs[i].used==1){j=&s_jobs[i];j->used=2;break;}
 cancel=j&&j->session&&j->session==s_session&&(s_stop_requested||s_phase==C_CLOSING)&&
  (j->kind==J_DEV_DIAL||j->kind==J_WX_DIAL||j->kind==J_INFO||j->kind==J_ROOM_GET||j->kind==J_ROOM_RECOVER);
 liot_rtos_exit_critical();if(!j)return;started=tick();
 j->result=CALL_STALE;
 if(!cancel&&demo_binding_service_context_current(&j->context)){
  switch(j->kind){
   case J_PROFILE:j->result=calls_proto_profile(tirtc_video_profile_json(),&j->context);break;
   case J_DEV_DIAL:j->result=calls_proto_device_dial(&j->contact,j->video,&j->context,&j->dial);break;
   case J_WX_DIAL:j->result=calls_proto_wx_dial(&j->contact,j->device,j->video,&j->context,&j->dial);break;
   case J_INFO:j->result=calls_proto_device_info(j->contact.id,j->room,&j->context,&j->access);break;
   case J_ROOM_GET:j->result=calls_proto_room_get(&j->context,&j->room_info);break;
   case J_ROOM_RECOVER:case J_ROOM_END:j->result=calls_proto_room_action(j->action,j->room,"device",&j->context);break;
   case J_WX_REJECT:j->result=calls_proto_wx_reject(&j->event,j->reason,&j->context);break;
   default:break;
  }
 }
 liot_trace("[CALL22] service id=%lu kind=%u result=%d ms=%lu\r\n",(unsigned long)j->session,(unsigned)j->kind,j->result,(unsigned long)(tick()-started));
 liot_rtos_enter_critical();j->used=3;liot_rtos_exit_critical();
}
static void http_worker(void*arg){(void)arg;for(;;){http_once();liot_rtos_task_sleep_ms(20);}}
static void clear_job(job_t*j){memset(j,0,sizeof(*j));}
static void begin_close_from(int error,bool remote,const char *source)
{
 phase_t previous;uint32_t session,generation;bool video_started,remote_end;
 tirtc_video_snapshot_t video;
 if(s_phase==C_IDLE||s_phase==C_BLOCKED||s_phase==C_CLOSING)return;
 liot_rtos_enter_critical();previous=s_phase;session=s_session;generation=s_generation;video_started=s_video_started;
 s_callbacks=false;s_phase=C_CLOSING;s_stop_requested=true;s_remote_end|=remote;remote_end=s_remote_end;
 s_final_error=error;s_cleanup_at=tick();s_cleanup_next=tick();liot_rtos_exit_critical();
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
 call_sdk_media_report(true);
#endif
 /* Capture the pre-stop video state once. Heap queries and logging must not
  * hold the task critical section or change close/retry decisions. */
 memset(&video,0,sizeof(video));tirtc_video_get_snapshot(&video);
 liot_trace("[CALL29] close id=%lu phase=%u error=%d remote=%u source=%s video_started=%u video_active=%u video_error=%d free_heap=%lu\r\n",
  (unsigned long)session,(unsigned)previous,error,remote_end?1U:0U,source,
  video_started?1U:0U,(video.generation==generation&&video.active)?1U:0U,
  video.generation==generation?video.error:0,(unsigned long)liot_xPortGetFreeHeapSize());
 if(s_ui.wechat){copy(s_wx_closed_room,sizeof(s_wx_closed_room),s_room);copy(s_wx_closed_call,sizeof(s_wx_closed_call),s_call_id);s_wx_closed_context=s_context;s_wx_tombstone_until=tick()+60000U;
  if(!s_ui.incoming&&!s_invite.room[0])s_wx_unassociated_until=tick()+30000U;
 }
 if(s_video_started)tirtc_video_stop(s_generation);
 /* stop closes capture admission immediately; completion is fenced below. */
 (void)calls_audio_stop(s_owner,s_generation);
 s_end_pending=!s_remote_end;
 publish(TIRTC_CALL_CLOSING,"正在结束通话",error);
}
static void begin_close(int error,bool remote){begin_close_from(error,remote,"state");}
static bool reject_event(const mqtt_event_t*event,unsigned reason)
{
 job_t*j=&s_jobs[1];
 if(!job_free(1))return false;
 memset(j,0,sizeof(*j));j->kind=event->event.wechat?J_WX_REJECT:J_ROOM_END;j->event=event->event;j->context=event->context;
 copy(j->room,sizeof(j->room),event->event.room);j->action=CALLS_PROTO_ROOM_REJECT;j->reason=reason;
 liot_rtos_enter_critical();j->used=1;liot_rtos_exit_critical();return true;
}
/* Passive monitoring holds the sole runtime owner through teardown. Check
 * this independently of the visible page so MQTT/local actions cannot replace
 * its UI or stop AI on behalf of a call that cannot acquire the owner. */
static bool live_owned(void)
{demo_tirtc_connection_snapshot_t snapshot;demo_tirtc_get_connection_snapshot(&snapshot);return snapshot.owner==DEMO_TIRTC_OWNER_LIVE;}
/* Caller holds the task critical section through the local intent commit.
 * NONE must use the shared non-recursive gate. AI already excludes passive
 * admission; reserving the call intent before its release preserves the
 * existing call-to-AI-stop handoff. GROUP is also preemptible: commit the
 * call intent before asking its worker to drain; never steal its live lease.
 * No SDK or action runs inside this gate. */
static bool call_admission_locked(bool *entered)
{
 demo_tirtc_connection_snapshot_t snapshot;*entered=false;
 demo_tirtc_get_connection_snapshot(&snapshot);
 if(snapshot.owner==DEMO_TIRTC_OWNER_AI)return true;
#ifdef HWDEMO_GROUP_ROOM_EN
 if(snapshot.owner==DEMO_TIRTC_OWNER_GROUP_ROOM)return true;
#endif
 if(snapshot.owner!=DEMO_TIRTC_OWNER_NONE)return false;
 *entered=demo_tirtc_admission_try_enter();return *entered;
}
static int new_call(const tirtc_contact_t*contact,bool video,const mqtt_event_t*slot)
{
 const calls_proto_event_t*invite=slot?&slot->event:NULL;
 tirtc_ui_action_t stop;demo_binding_snapshot_t binding;bool admission;
 demo_binding_get_snapshot(&binding);
 liot_rtos_enter_critical();
 if(slot&&slot->cancelled){liot_rtos_exit_critical();return CALL_STALE;}
 if(!call_admission_locked(&admission)){liot_rtos_exit_critical();return CALL_ERROR;}
 if(!++s_next_session){++s_next_session;}s_session=s_next_session;s_contact=*contact;s_context=contact->context;
 s_owner=contact->wechat?DEMO_TIRTC_OWNER_WECHAT:DEMO_TIRTC_OWNER_DEV_CHAT;s_phase=C_OWNER;s_phase_at=tick();
 s_generation=0;s_connection=s_dev_confirmed=NULL;s_owned=s_callbacks=s_connect_done=s_peer_answered=s_confirm_pending=false;
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
 memset(&s_sdk_media,0,sizeof(s_sdk_media));
#endif
 s_stop_requested=s_accept_requested=s_video_dirty=s_dial_submitted=s_end_pending=s_end_queued=s_remote_end=false;
 s_recovering=s_redialed=s_hangup_sent=s_audio_started=s_video_started=false;s_server_answered=false;s_room_owned=invite!=NULL;s_answer_room[0]=0;s_lost=s_connect_error=s_final_error=0;
 memset(s_pending_cancels,0,sizeof(s_pending_cancels));s_pending_cancel_next=0;
 memset(&s_ui,0,sizeof(s_ui));s_ui.session_id=s_session;s_ui.type=video?TIRTC_CALL_VIDEO:TIRTC_CALL_AUDIO;
 s_ui.video_enabled=video;s_ui.wechat=contact->wechat;s_ui.incoming=invite!=NULL;
 copy(s_ui.peer_id,sizeof(s_ui.peer_id),contact->id);copy(s_ui.peer_name,sizeof(s_ui.peer_name),contact->name[0]?contact->name:(contact->id[0]?contact->id:(contact->wechat?"微信联系人":"设备")));
 memset(&s_invite,0,sizeof(s_invite));if(invite)s_invite=*invite;
 copy(s_room,sizeof(s_room),invite?invite->room:"");copy(s_call_id,sizeof(s_call_id),invite?invite->call_id:"");copy(s_self,sizeof(s_self),binding.device_id);
 if(admission)demo_tirtc_admission_leave();
 liot_rtos_exit_critical();
#ifdef HWDEMO_GROUP_ROOM_EN
 tirtc_group_suspend();
#endif
 memset(&stop,0,sizeof(stop));stop.type=TIRTC_ACTION_AI_STOP;(void)tirtc_ai_action(&stop);
 demo_tirtc_set_feature_listener(contact->wechat?DEMO_TIRTC_FEATURE_WECHAT:DEMO_TIRTC_FEATURE_DEV_CHAT,&s_listener);s_listener_live=true;
 publish(invite?TIRTC_CALL_INCOMING:TIRTC_CALL_OUTGOING,invite?"收到来电":"正在拨号",0);
 liot_trace("[CALL22] begin id=%lu incoming=%u wx=%u video=%u\r\n",(unsigned long)s_session,invite?1U:0U,contact->wechat?1U:0U,video?1U:0U);
 return 0;
}
static bool wx_matches(const calls_proto_event_t*e)
{
 if((e->contact_id[0]&&strcmp(e->contact_id,s_contact.id))||(s_call_id[0]&&e->call_id[0]&&strcmp(s_call_id,e->call_id))||(e->from[0]&&strcmp(e->from,s_self)))return false;
 return true; /* Official minimum join omits all three optional identifiers. */
}
static void process_events(void)
{
 unsigned i;demo_binding_snapshot_t binding;demo_binding_get_snapshot(&binding);
 for(i=0;i<EVENT_COUNT-1U;i++){
  mqtt_event_t*e=&s_events[i];bool ready,cancelled,deferred=false;
  liot_rtos_enter_critical();ready=e->used==1;cancelled=e->cancelled;liot_rtos_exit_critical();if(!ready)continue;
  if(!cancelled&&demo_binding_service_context_current(&e->context)){
   if(e->event.type==CALLS_PROTO_INCOMING){
    bool same_context=context_equal(&e->context,&s_context);
    bool same_room=s_phase!=C_IDLE&&same_context&&s_room[0]&&!strcmp(s_room,e->event.room);
    bool outgoing_wx=same_context&&s_ui.wechat&&!s_ui.incoming&&e->event.wechat&&
        (s_phase==C_DIAL||s_phase==C_WX_NOTIFY)&&wx_matches(&e->event);
    bool closed_wx=e->event.wechat&&!due(s_wx_tombstone_until)&&context_equal(&e->context,&s_wx_closed_context)&&
        ((s_wx_closed_room[0]&&!strcmp(s_wx_closed_room,e->event.room))||
         (s_wx_closed_call[0]&&e->event.call_id[0]&&!strcmp(s_wx_closed_call,e->event.call_id)));
    if(same_room){}
    else if(closed_wx)deferred=!reject_event(e,7);
    else if(outgoing_wx&&!s_stop_requested&&!s_invite.room[0]){
     /* Wait for the HTTP call ID before accepting a keyed invitation. */
     if(s_phase==C_DIAL&&!s_call_id[0]&&e->event.call_id[0])deferred=true;
     else if(!due(s_wx_unassociated_until)&&(!s_call_id[0]||!e->event.call_id[0])){deferred=!reject_event(e,7);if(!deferred)begin_close(CALL_RETRY,false);}
     else{
      s_invite=e->event;
      liot_rtos_enter_critical();copy(s_room,sizeof(s_room),e->event.room);
      if(!s_call_id[0])copy(s_call_id,sizeof(s_call_id),e->event.call_id);
      if(pending_cancel_matches_locked()){s_remote_end=true;s_stop_requested=true;}
      liot_rtos_exit_critical();
     }
    }
    else if(s_phase!=C_IDLE||s_pending_dial||live_owned())deferred=!reject_event(e,5);
    else if(e->event.wechat&&e->event.from[0]&&!strcmp(e->event.from,binding.device_id))deferred=!reject_event(e,7);
    else{
     tirtc_contact_t contact;memset(&contact,0,sizeof(contact));contact.context=e->context;contact.wechat=e->event.wechat;contact.online=true;
     copy(contact.id,sizeof(contact.id),e->event.contact_id);copy(contact.name,sizeof(contact.name),e->event.name);
     copy(contact.wx_app_id,sizeof(contact.wx_app_id),e->event.app_id);copy(contact.wx_model_id,sizeof(contact.wx_model_id),e->event.model_id);
     if(new_call(&contact,e->event.video,e)==CALL_ERROR)deferred=!reject_event(e,5);
    }
   }else if(s_phase!=C_IDLE&&context_equal(&e->context,&s_context)&&!s_room[0]&&e->event.type==CALLS_PROTO_ANSWERED){copy(s_answer_room,sizeof(s_answer_room),e->event.room);
   }else if(s_phase!=C_IDLE&&context_equal(&e->context,&s_context)&&s_room[0]&&!strcmp(s_room,e->event.room)){
    if(e->event.type==CALLS_PROTO_CANCEL)begin_close(0,true);
    else if(e->event.type==CALLS_PROTO_ANSWERED){s_peer_answered=true;}
    /* A single callee rejection does not terminate a multi-device room. */
   }
  }
  if(!deferred){
   liot_rtos_enter_critical();e->used=2;liot_rtos_exit_critical();
   calls_proto_clear_event(&e->event);
   liot_rtos_enter_critical();e->used=0;liot_rtos_exit_critical();
  }
 }
}
static int start_connection(const char*peer,const char*token)
{
 int result;set_phase(C_CONNECT);liot_rtos_enter_critical();s_connect_done=false;s_connect_error=0;s_callbacks=true;liot_rtos_exit_critical();
 result=s_ui.wechat?demo_tirtc_whip_connect(s_owner,s_generation,peer,token,connect_callback,(void*)(uintptr_t)s_session):
  demo_tirtc_connect(s_owner,s_generation,peer,token,connect_callback,(void*)(uintptr_t)s_session);
 if(result)begin_close(result,false);else publish(TIRTC_CALL_CONNECTING,"正在连接",0);return result;
}
static void request_dial(void)
{
 if(!s_ui.wechat){int result=demo_tirtc_expect_incoming(s_owner,s_generation);if(result){begin_close(result,false);return;}}
 if(queue_job(0,s_ui.wechat?J_WX_DIAL:J_DEV_DIAL)){s_dial_submitted=true;set_phase(C_DIAL);}
}
static void process_job(void)
{
 job_t*j=&s_jobs[0];job_kind_t kind;int result;bool ready,cancelled;
 liot_rtos_enter_critical();ready=j->used==3;liot_rtos_exit_critical();if(!ready)return;
 kind=j->kind;result=j->result;
 if(kind==J_INFO&&!result)s_server_answered=true;
 liot_rtos_enter_critical();
 if(kind==J_DEV_DIAL&&(!result||result==40202)&&j->dial.room[0]){copy(s_room,sizeof(s_room),j->dial.room);s_room_owned=!result;s_dev_confirmed=NULL;if(!result&&s_answer_room[0]&&!strcmp(s_answer_room,s_room))s_peer_answered=true;}
 if(kind==J_WX_DIAL&&!result)copy(s_call_id,sizeof(s_call_id),j->dial.call_id);
 cancelled=!result&&pending_cancel_matches_locked();
 liot_rtos_exit_critical();
 if(cancelled)begin_close(0,true);
 if(s_stop_requested&&s_phase!=C_CLOSING&&s_phase!=C_BLOCKED)begin_close(0,s_remote_end);
 if(s_phase==C_CLOSING||s_phase==C_BLOCKED){if(result&&kind==J_ROOM_END)liot_trace("[CALL22] room cleanup result=%d\r\n",result);clear_job(j);return;}
 if(j->session!=s_session||!demo_binding_service_context_current(&j->context)){clear_job(j);begin_close(CALL_STALE,true);return;}
 if(kind==J_DEV_DIAL&&result==40202&&!s_redialed){s_redialed=true;clear_job(j);(void)queue_job(0,J_ROOM_GET);return;}
 if(kind==J_ROOM_GET&&!result){
  calls_proto_room_t room=j->room_info;clear_job(j);
  if(!room.present){if(s_recovering)s_recovering=false;s_room[0]=0;request_dial();return;}
  if(s_recovering||!s_room[0]||strcmp(room.room,s_room)){begin_close(40202,false);return;}
  if(queue_job_action(0,J_ROOM_RECOVER,room.answered?CALLS_PROTO_ROOM_HANGUP:(room.caller_role?CALLS_PROTO_ROOM_CANCEL:CALLS_PROTO_ROOM_REJECT),7)){s_recovering=true;}return;
 }
 if(kind==J_ROOM_RECOVER&&!result){clear_job(j);(void)queue_job(0,J_ROOM_GET);return;}
 if(result){clear_job(j);begin_close(result,false);return;}
 if(kind==J_INFO){calls_proto_access_t access=j->access;clear_job(j);(void)start_connection(access.peer,access.token);memset(&access,0,sizeof(access));return;}
 clear_job(j);
 if(kind==J_DEV_DIAL)set_phase(C_CONFIRM);
 else if(kind==J_WX_DIAL)set_phase(C_WX_NOTIFY);
}
static bool escaped_zero(const char*text)
{size_t i;for(i=0;text[i];i++)if(text[i]=='\\'){i++;if(!text[i]||!strncmp(text+i,"u0000",5))return true;}return false;}
static void process_confirmation(void)
{
 char text[sizeof(s_confirm)];bool pending;uint32_t length;tirtc_conn_t connection;
 bool dev_caller=!s_ui.wechat&&!s_ui.incoming;
 /* Consume an early room confirmation only after successful dial HTTP. */
 if(dev_caller&&(s_phase!=C_CONFIRM||!s_room_owned))return;
 liot_rtos_enter_critical();pending=s_confirm_pending;length=s_confirm_length;connection=s_connection;
 if(pending){memcpy(text,s_confirm,length+1U);s_confirm_pending=false;}liot_rtos_exit_critical();
 if(!pending)return;
 if(s_ui.wechat){s_peer_answered=true;return;}
 if(length&&!escaped_zero(text)){
  cJSON*json=demo_json_parse_with_length_opts(text,length+1U,NULL,true);cJSON*room=NULL;const cJSON*item;unsigned matches=0;
  if(cJSON_IsObject(json))for(item=json->child;item;item=item->next)if(item->string&&!strcmp(item->string,"room_id")){room=(cJSON*)item;matches++;}
  if(matches==1&&cJSON_IsString(room)&&room->valuestring&&!strcmp(room->valuestring,s_room)){
   s_peer_answered=true;
   if(dev_caller)s_dev_confirmed=connection;
  }
  cJSON_Delete(json);
 }
}
#if TIRTC_F9_AV_FOCUS
static bool restore_av_focus(void)
{
 bool media=calls_audio_set_av_focus(false);
 if(s_av_focus_control){
  uint8_t previous;
  if(liot_rtos_task_change_priority(s_task,s_av_focus_saved_priority,&previous)!=LIOT_OSI_SUCCESS)
   return false;
  s_av_focus_control=false;
 }
 return media;
}

static void begin_av_focus(void)
{
 if(!s_audio_started||s_ui.type!=TIRTC_CALL_VIDEO)return;
 if(liot_rtos_task_change_priority(s_task,TIRTC_F9_AV_AUDIO_PRIORITY,
                                   &s_av_focus_saved_priority)==LIOT_OSI_SUCCESS){
  s_av_focus_control=true;
  if(calls_audio_set_av_focus(true))return;
  (void)restore_av_focus();
 }
 liot_trace("[AV-FOCUS] audio priority unavailable\r\n");
}
#endif

static void activate(void)
{
 int result;uint8_t audio=s_ui.wechat?0U:10U,video=s_ui.wechat?1U:11U,enabled=1;
 if(s_stop_requested){begin_close(0,s_remote_end);return;}
 result=calls_audio_start(s_owner,s_generation);if(result){begin_close(result,false);return;}s_audio_started=true;
 if(s_stop_requested){begin_close(0,s_remote_end);return;}
#if TIRTC_F9_AV_FOCUS
 begin_av_focus();
#endif
 if(s_ui.type==TIRTC_CALL_VIDEO){result=tirtc_video_start(s_owner,s_generation,s_ui.wechat,s_ui.incoming);if(result){begin_close(result,false);return;}s_video_started=true;}
 result=demo_tirtc_subscribe_audio(s_owner,s_generation,audio);if(result<0){begin_close(result,false);return;}
 if(s_ui.type==TIRTC_CALL_VIDEO){result=demo_tirtc_subscribe_video(s_owner,s_generation,video);if(result<0){begin_close(result,false);return;}
  if(s_ui.wechat){result=demo_tirtc_send_command(s_owner,s_generation,0x10000U|0x1105U,&enabled,1);if(result<0){begin_close(result,false);return;}}}
 s_started=tick();
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
 s_sdk_media.started=s_sdk_media.last_report=s_started;
 demo_tirtc_get_transport_send_error_stats(&s_sdk_media.transport_start);
 s_sdk_media.active=true;
#endif
 set_phase(C_ACTIVE);publish(TIRTC_CALL_CONNECTED,"通话中",0);
}
static void cleanup_step(void)
{
 int result;bool pending;
 if(!due(s_cleanup_next))return;
 s_cleanup_next=tick()+1000U;
 /* The deadline is a UI warning, not permission to abandon teardown or
  * free an in-flight callback/DMA owner. Keep retrying the same safe gates. */
 if(s_phase!=C_BLOCKED&&(uint32_t)(tick()-s_cleanup_at)>=CLEANUP_LIMIT){
  set_phase(C_BLOCKED);publish(TIRTC_CALL_ERROR,"正在后台结束通话，请稍后重试",CALL_CLEANUP);
 }
 if(s_owned&&!s_hangup_sent){
  if(!s_remote_end&&s_connection){const char*reason="{\"reason\":1}";(void)demo_tirtc_send_command(s_owner,s_generation,0x2001,reason,(uint32_t)strlen(reason));}
  s_hangup_sent=true;
 }
 /* HTTP may already have created a room when cancellation arrived. Wait for
  * the immutable job response, then cancel exactly that room before reuse. */
 liot_rtos_enter_critical();pending=s_jobs[0].used!=0;liot_rtos_exit_critical();if(pending)return;
 if(s_end_pending&&!s_end_queued){
  if(!s_ui.wechat&&s_room_owned&&s_room[0]){
   if(queue_job_action(0,J_ROOM_END,s_ui.incoming?(s_server_answered?CALLS_PROTO_ROOM_HANGUP:CALLS_PROTO_ROOM_REJECT):
     (s_peer_answered?CALLS_PROTO_ROOM_HANGUP:CALLS_PROTO_ROOM_CANCEL),7)){s_end_queued=true;return;}
  }else if(s_ui.wechat&&!s_connection&&s_invite.room[0]){
   if(queue_job(0,J_WX_REJECT)){s_end_queued=true;return;}
  }else s_end_queued=true;
 }
 if(s_video_started){tirtc_video_stop(s_generation);if(!tirtc_video_is_stopped(s_generation))return;s_video_started=false;}
 result=calls_audio_stop(s_owner,s_generation);if(result)return;s_audio_started=false;
#if TIRTC_F9_AV_FOCUS
 if(!restore_av_focus())return;
#endif
 if(s_owned){
  (void)demo_tirtc_cancel_expected_incoming(s_owner,s_generation);
  (void)demo_tirtc_disconnect(s_owner,s_generation);
  result=demo_tirtc_session_release(s_owner,s_generation);if(result)return;
  s_owned=false;s_generation=0;
 }
 if(s_listener_live){demo_tirtc_set_feature_listener(s_ui.wechat?DEMO_TIRTC_FEATURE_WECHAT:DEMO_TIRTC_FEATURE_DEV_CHAT,NULL);s_listener_live=false;}
 calls_proto_clear_event(&s_invite);s_room[0]=s_call_id[0]=0;
 set_phase(C_IDLE);publish(s_final_error?TIRTC_CALL_ERROR:TIRTC_CALL_ENDED,s_final_error?(s_final_error==CALL_RETRY?"上次呼叫通知迟到，请30秒后重试":"通话中断，请重试"):"通话已结束",s_final_error);
}
static void auxiliary_step(void)
{
 job_t*j=&s_jobs[1];bool ready;
 liot_rtos_enter_critical();ready=j->used==3;liot_rtos_exit_critical();
 if(ready){if(j->kind==J_PROFILE){s_profile_pending=false;if(!j->result&&demo_binding_service_context_current(&j->context)){s_profile_context=j->context;s_profile_valid=true;}else s_profile_retry=tick()+15000U;}clear_job(j);}
 if((s_phase==C_IDLE||s_phase==C_OWNER)&&!s_profile_pending&&job_free(1)&&due(s_profile_retry)){
  demo_binding_service_context_t context;
  if(demo_binding_service_context(&context)&&demo_tirtc_is_ready()&&(!s_profile_valid||!context_equal(&context,&s_profile_context))){
   memset(j,0,sizeof(*j));j->kind=J_PROFILE;j->context=context;s_profile_pending=true;
   liot_rtos_enter_critical();j->used=1;liot_rtos_exit_critical();
  }
 }
}
static void step(void)
{
 bool pending,stop,accept,video_dirty,pending_video=false;int lost,result;tirtc_contact_t pending_contact;
 process_events();auxiliary_step();process_job();
 liot_rtos_enter_critical();pending=s_pending_dial;if(pending&&s_phase==C_IDLE){pending_contact=s_pending_contact;pending_video=s_pending_video;s_pending_dial=false;s_phase=C_OWNER;}else pending=false;
 liot_rtos_exit_critical();
 if(pending&&new_call(&pending_contact,pending_video,NULL)!=0){liot_rtos_enter_critical();s_phase=C_IDLE;liot_rtos_exit_critical();}
 liot_rtos_enter_critical();stop=s_stop_requested;accept=s_accept_requested;video_dirty=s_video_dirty;s_video_dirty=false;lost=s_lost;liot_rtos_exit_critical();
 if(s_phase==C_IDLE)return;
 if(s_phase==C_CLOSING||s_phase==C_BLOCKED){cleanup_step();return;}
 if(stop){begin_close(0,s_remote_end);cleanup_step();return;}
 if(lost){begin_close_from(lost,true,"lost");return;}
 if(!demo_binding_service_context_current(&s_context)){begin_close_from(CALL_STALE,true,"context");return;}
 if(!demo_tirtc_is_ready()){begin_close_from(CALL_STALE,true,"rtc_ready");return;}
 process_confirmation();
 if(s_phase==C_OWNER){
  if(!s_profile_valid||!context_equal(&s_profile_context,&s_context)){if((uint32_t)(tick()-s_phase_at)>=20000U)begin_close(CALL_TIMEOUT,false);return;}
  result=demo_tirtc_session_claim(s_owner,&s_generation);
  if(!result){s_owned=true;s_callbacks=true;if(s_ui.incoming)set_phase(C_RING);else request_dial();}
  else if((uint32_t)(tick()-s_phase_at)>=12000U)begin_close(result,false);
 }else if(s_phase==C_RING){
  if(accept){s_accept_requested=false;if(s_ui.wechat)(void)start_connection(s_invite.peer,s_invite.token);
   else if(queue_job(0,J_INFO)){set_phase(C_DIAL);publish(TIRTC_CALL_CONNECTING,"正在接听",0);}}
  else if((uint32_t)(tick()-s_phase_at)>=45000U)begin_close(CALL_TIMEOUT,false);
 }else if(s_phase==C_WX_NOTIFY){
  if(s_invite.peer[0]&&s_invite.token[0])(void)start_connection(s_invite.peer,s_invite.token);
  else if((uint32_t)(tick()-s_phase_at)>=30000U)begin_close(CALL_TIMEOUT,false);
 }else if(s_phase==C_CONNECT){
  if(s_connect_done){
   if(s_connect_error){begin_close(s_connect_error,false);return;}
   if(!s_ui.wechat){char json[160];int length=snprintf(json,sizeof(json),"{\"room_id\":\"%s\"}",s_room);
    if(length<0||(size_t)length>=sizeof(json)){begin_close(CALL_ERROR,false);return;}
    result=demo_tirtc_send_command(s_owner,s_generation,0x2000,json,(uint32_t)length);if(result<0){begin_close(result,false);return;}s_peer_answered=true;}
   set_phase(C_CONFIRM);
  }else if((uint32_t)(tick()-s_phase_at)>=15000U)begin_close(CALL_TIMEOUT,false);
 }else if(s_phase==C_CONFIRM){
  bool ready;
  liot_rtos_enter_critical();
  ready=s_connection&&!s_stop_requested&&((s_ui.wechat||s_ui.incoming)?s_peer_answered:s_dev_confirmed==s_connection);
  liot_rtos_exit_critical();
  if(ready)activate();
  else if((uint32_t)(tick()-s_phase_at)>=30000U)begin_close(CALL_TIMEOUT,false);
 }else if(s_phase==C_ACTIVE){
  if(video_dirty&&s_video_started){result=tirtc_video_set_enabled(s_generation,s_ui.video_enabled);if(result){begin_close(result,false);return;}}
  if(s_video_started){tirtc_video_snapshot_t snapshot;tirtc_video_get_snapshot(&snapshot);if(snapshot.generation==s_generation&&snapshot.error){begin_close(snapshot.error,false);return;}}
  result=calls_audio_step(s_owner,s_generation);if(result){begin_close(result,false);return;}
#if TIRTC_CALL_MEDIA_DIAGNOSTICS
  call_sdk_media_report(false);
#endif
  if((uint32_t)(tick()-s_last_publish)>=1000U){liot_rtos_enter_critical();s_ui.seconds=(tick()-s_started)/1000U;liot_rtos_exit_critical();publish(TIRTC_CALL_CONNECTED,"通话中",0);}
 }
}
static void worker(void*arg)
{
 (void)arg;
 for(;;){
  uint32_t started=tick(),elapsed;
  step();
  /* Record already waits for the next 20 ms of PCM. Do not insert a 5 ms
   * capture gap after every frame; retain the idle/control polling floor. */
  elapsed=(uint32_t)(tick()-started);
  if(elapsed<5U)liot_rtos_task_sleep_ms(5U-elapsed);
#if TIRTC_F9_AV_FOCUS
  else if(s_av_focus_control)liot_rtos_task_sleep_ms(1U);
#endif
 }
}
static bool decimal_session(const char*text,uint32_t*out)
{uint32_t value=0;unsigned i;if(!text||!text[0])return false;for(i=0;text[i];i++){unsigned digit=(unsigned)(text[i]-'0');if(i>=10||digit>9U||value>(UINT32_MAX-digit)/10U)return false;value=value*10U+digit;}*out=value;return value!=0;}
int tirtc_calls_dial_contact(const tirtc_contact_t *contact,bool video)
{
 bool admission;
 if(!s_task||!s_http_task||!contact||!contact->id[0]||
    !memchr(contact->id,0,sizeof(contact->id))||(!contact->wechat&&!contact->online)||
    !demo_binding_service_context_current(&contact->context))return -2;
 if(!demo_tirtc_is_ready())return -3;
 liot_rtos_enter_critical();
 if(s_phase!=C_IDLE||s_pending_dial||!call_admission_locked(&admission)){liot_rtos_exit_critical();return -4;}
 s_pending_contact=*contact;s_pending_video=video;s_pending_dial=true;
 if(admission)demo_tirtc_admission_leave();
 liot_rtos_exit_critical();
#ifdef HWDEMO_GROUP_ROOM_EN
 tirtc_group_suspend();
#endif
 return 0;
}
int tirtc_calls_action(const tirtc_ui_action_t*a)
{
 tirtc_contact_t contact;uint32_t session=0;bool dial;
 if(!a)return -1;
 dial=a->type==TIRTC_ACTION_CALL_AUDIO||a->type==TIRTC_ACTION_CALL_VIDEO;
 if(dial){
  if(!s_task||!s_http_task||!memchr(a->text,0,sizeof(a->text))||!tirtc_contacts_lookup(a->text,a->value!=0,&contact)||(!contact.wechat&&!contact.online))return -2;
  return tirtc_calls_dial_contact(&contact,a->type==TIRTC_ACTION_CALL_VIDEO);
 }
 if(a->type==TIRTC_ACTION_SET_VIDEO_ENABLED){if(!memchr(a->extra,0,sizeof(a->extra))||!decimal_session(a->extra,&session))return -2;}
 else session=(uint32_t)a->value;
 liot_rtos_enter_critical();
 if(!session||session!=s_session||s_phase==C_IDLE||s_phase==C_CLOSING||s_phase==C_BLOCKED){liot_rtos_exit_critical();return -4;}
 switch(a->type){
  case TIRTC_ACTION_ACCEPT_CALL:if(!s_ui.incoming||(s_phase!=C_RING&&s_phase!=C_OWNER)){liot_rtos_exit_critical();return -4;}s_accept_requested=true;break;
  case TIRTC_ACTION_REJECT_CALL:if(!s_ui.incoming||s_phase==C_ACTIVE){liot_rtos_exit_critical();return -4;}s_stop_requested=true;break;
  case TIRTC_ACTION_HANGUP:s_stop_requested=true;break;
  case TIRTC_ACTION_SET_VIDEO_ENABLED:if(s_ui.type!=TIRTC_CALL_VIDEO||s_phase!=C_ACTIVE){liot_rtos_exit_critical();return -4;}s_ui.video_enabled=a->value!=0;s_video_dirty=true;break;
  default:liot_rtos_exit_critical();return -1;
 }
 liot_rtos_exit_critical();return 0;
}
int tirtc_calls_start_service(void)
{
 liot_task_t task=NULL;int result;bool create;
 result=calls_audio_start_service();if(result)return result;
 if(!s_registered){result=demo_formal_mqtt_register_handler(mqtt_received,NULL);if(result)return result;s_registered=true;}
 liot_rtos_enter_critical();create=!s_http_task&&!s_http_creating;if(create)s_http_creating=true;liot_rtos_exit_critical();
 if(create){result=(int)liot_rtos_task_create(&task,16U*1024U,10,"call_http",http_worker,NULL);liot_rtos_enter_critical();s_http_creating=false;if(!result&&task)s_http_task=task;liot_rtos_exit_critical();if(result||!task)return -1;}
 task=NULL;liot_rtos_enter_critical();create=!s_task&&!s_creating;if(create)s_creating=true;liot_rtos_exit_critical();
 if(create){result=(int)liot_rtos_task_create(&task,CALLS_AUDIO_CONTROL_STACK_BYTES,13,"call_control",worker,NULL);liot_rtos_enter_critical();s_creating=false;if(!result&&task)s_task=task;liot_rtos_exit_critical();if(result||!task)return -1;}
 return 0;
}
