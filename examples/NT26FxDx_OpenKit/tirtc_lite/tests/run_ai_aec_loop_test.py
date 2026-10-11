#!/usr/bin/env python3
"""Check production AI loop fairness, tail completion and cancellation gates.

The actual result normalization, uplink function and active loop are compiled against deterministic
audio/network boundaries. No board hooks are added to production code. The
driver and cloud protocol are fakes; this does not test acoustic cancellation.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from run_aec_uplink_guard_test import function
from run_dev_playback_gain_test import compiler_command


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define DEMO_AI_AUDIO_FRAME_MS 20U
#define AI_SEND_HIGH_WATER_BYTES (12U*1024U)
#define AI_ERR_CANCELLED (-8)
#define AI_ERR_CONNECTION (-9)
#define DEMO_TIRTC_OWNER_AI 1
#define TIRTC_E_BUSY (-10)
#define TIRTC_E_INVALID_HANDLE (-40002)
#define TIRTC_E_TIMEOUTED (-40005)
#define TIRTC_E_CONN_TIMEOUTCLOSE (-40007)
#define TIRTC_E_CONN_REMOTECLOSE (-40008)
#define TIRTC_E_CONN_OTHER_ERROR (-40009)
#define AI_STREAM_ID 1
#define TIRTC_AUDIO_OPUS 1
#define TIRTC_AUDIOSAMPLE_16K16B1C 1
#define DEMO_AI_CHAT_LISTENING 1
#define DEMO_AI_CHAT_SPEAKING 2
#define AI_ACTIVE_POLL_MS 10U
#define AI_PREBUFFER_PACKETS 3U
#define AI_PREBUFFER_TIMEOUT_MS 80U
#define AI_SPEECH_IDLE_MS 800U
#define liot_trace(...) ((void)0)
typedef struct { unsigned stream_id,media,flags,ts,length; } TIRTCFRAMEINFO;
static bool s_server_speaking,s_remote_ended,s_conn_error_pending,s_disconnected_pending,s_levels_dirty;
static unsigned s_tirtc_session_generation,s_tx_dropped,s_tx_frames,s_play_pending_samples,s_prebuffer_start_ms,s_last_audio_ms;
static unsigned s_running_request_id;
#if DEMO_AI_AUDIO_AEC_ENABLE
static bool s_capture_timeout_active;
static uint32_t s_capture_timeout_started_ms,s_capture_timeout_request_id;
#endif
static int s_audio_lease,s_speaker_level,s_mic_level,s_audio_queue,s_event_sem;
static int16_t s_capture_pcm[320];
static unsigned char s_uplink_opus[512];
static bool active,done,cancel_capture,cancel_encode,stop_on_play,tail_round;
static unsigned captures,sends,encodes,sleeps,waits,plays,clock_ms,queued,target_sends;
static unsigned capture_cost_ms,capture_timeouts_left;
static int capture_error,state;
static int s_conn_error,close_stage,close_error,sdk_error;
static bool close_disconnected;
static unsigned close_delay_ms,close_publish_at;
static bool close_publication_pending;
enum { CLOSE_BUFFER=1,CLOSE_CAPTURE,CLOSE_ENCODE,CLOSE_SEND };
static size_t buffered;
static void inject_close(int stage){
    if(close_stage!=stage)return;
    if(close_delay_ms){close_publish_at=clock_ms+close_delay_ms;close_publication_pending=true;return;}
    if(close_error){s_conn_error=close_error;s_conn_error_pending=true;}
    if(close_disconnected)s_disconnected_pending=true;
}
static bool ai_request_is_active(void){return active;}
static bool demo_ai_audio_play_done(void *lease){(void)lease;return tail_round ? captures>=2 : done;}
static void liot_rtos_task_sleep_ms(unsigned ms){
    assert(ms==20 || ms==1);sleeps++;clock_ms+=ms;
    if(ms==20)s_remote_ended=true;
    if(close_publication_pending && (int32_t)(clock_ms-close_publish_at)>=0){
        close_publication_pending=false;
        if(close_error){s_conn_error=close_error;s_conn_error_pending=true;}
        if(close_disconnected)s_disconnected_pending=true;
    }
}
static int demo_tirtc_get_send_buffer_used(int o,unsigned g,size_t *b){
    (void)o;(void)g;*b=buffered;inject_close(CLOSE_BUFFER);
    return close_stage==CLOSE_BUFFER ? sdk_error : 0;
}
static int demo_ai_audio_record_20ms(void *l,int16_t *pcm){
    unsigned i;int result=capture_error;(void)l;captures++;clock_ms+=capture_cost_ms;
    if(capture_timeouts_left){capture_timeouts_left--;result=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;}
    if(result==0)for(i=0;i<320;i++)pcm[i]=123;
    if(cancel_capture)active=false;
    inject_close(CLOSE_CAPTURE);
    return result;
}
static int demo_ai_opus_encode_20ms(const int16_t *p,unsigned char *o,size_t n){
    unsigned i;assert(n>=3);for(i=0;i<320;i++)assert(p[i]==123);
    memset(o,0x71,n);encodes++;
    if(cancel_encode)active=false;
    inject_close(CLOSE_ENCODE);
    return 3;
}
static unsigned liot_rtos_get_running_time(void){return clock_ms;}
static int demo_tirtc_send_audio(int o,unsigned g,const TIRTCFRAMEINFO *f,void *p){
    (void)o;(void)g;(void)p;
    assert(f->stream_id==AI_STREAM_ID && f->media==TIRTC_AUDIO_OPUS && f->flags==TIRTC_AUDIOSAMPLE_16K16B1C && f->length==3);
    sends++;inject_close(CLOSE_SEND);
    if(close_stage>=CLOSE_CAPTURE)return sdk_error;
    if(sends>=target_sends)s_remote_ended=true;return 0;
}
static void ai_set_state(int s,int e){state=s;(void)e;}
static void ai_refresh_call_contacts(void){}
static void ai_process_commands(void){
    /* Model a normal round_end arriving while the last driver block still
     * plays; the next capture advances beyond the tail completion time. */
    if(tail_round && captures==1)s_server_speaking=false;
}
static void ai_disconnect_deferred(void){}
static void ai_poll_call_lookup(void){}
static int demo_ai_audio_set_levels(void *l,int s,int m){(void)l;(void)s;(void)m;return 0;}
static int liot_rtos_queue_get_cnt(int q,uint32_t *n){(void)q;*n=tail_round ? (captures==0 ? 3 : 0) : queued;return 0;}
static int ai_play_downlink(void){
    plays++;s_server_speaking=true;ai_set_state(DEMO_AI_CHAT_SPEAKING,0);
    if(stop_on_play)s_remote_ended=true;
    return 0;
}
static int liot_rtos_semaphore_wait(int s,unsigned ms){(void)s;(void)ms;waits++;s_remote_ended=true;return 0;}
'''

SUFFIX = r'''
static void reset(void){
    s_server_speaking=s_remote_ended=s_conn_error_pending=s_disconnected_pending=s_levels_dirty=false;
    s_tx_dropped=s_tx_frames=s_play_pending_samples=s_prebuffer_start_ms=s_last_audio_ms=0;
    active=done=true;cancel_capture=cancel_encode=tail_round=false;stop_on_play=true;
    captures=sends=encodes=sleeps=waits=plays=queued=0;clock_ms=1000;target_sends=1;capture_error=0;buffered=0;state=0;
    capture_cost_ms=20U;capture_timeouts_left=0U;s_running_request_id=1U;
#if DEMO_AI_AUDIO_AEC_ENABLE
    s_capture_timeout_active=false;s_capture_timeout_started_ms=s_capture_timeout_request_id=0U;
#endif
    s_conn_error=close_stage=close_error=sdk_error=0;close_disconnected=false;
    close_delay_ms=close_publish_at=0;close_publication_pending=false;
    memset(s_capture_pcm,0x7F,sizeof(s_capture_pcm));memset(s_uplink_opus,0x7F,sizeof(s_uplink_opus));
}
static void test_capture_timeout_budget(void){
#if DEMO_AI_AUDIO_AEC_ENABLE
    unsigned i,start;
    /* Playback keeps running through missing capture frames; no failed
     * capture is encoded or submitted, and a successful frame recovers. */
    reset();queued=3;stop_on_play=false;capture_timeouts_left=3;capture_cost_ms=120;
    assert(ai_active_loop()==0);
    assert(captures==4 && plays==4 && sends==1 && encodes==1 && !s_capture_timeout_active);
    reset();queued=3;stop_on_play=false;capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;capture_cost_ms=120;
    assert(ai_active_loop()==DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT);
    assert(captures==18 && plays==18 && sends==0 && encodes==0 && clock_ms==3160);
    /* A zero-valued start and a 32-bit wrap both retain the exact 2 s bound. */
    for(i=0;i<2;i++){
        reset();capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;capture_cost_ms=0;
        start=i ? UINT32_MAX-1000U : 0U;clock_ms=start;
        assert(ai_send_uplink_20ms()==0);
        clock_ms=start+1999U;assert(ai_send_uplink_20ms()==0);
        clock_ms=start+2000U;assert(ai_send_uplink_20ms()==DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT);
        assert(captures==3 && encodes==0 && sends==0);
    }
    reset();capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
    assert(ai_send_uplink_20ms()==0);clock_ms+=1900U;
    capture_error=0;assert(ai_send_uplink_20ms()==0 && !s_capture_timeout_active);
    capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;clock_ms+=3000U;
    assert(ai_send_uplink_20ms()==0 && sends==1 && encodes==1);
    capture_error=-7;assert(ai_send_uplink_20ms()==-7 && !s_capture_timeout_active);
    assert(sends==1 && encodes==1);
    /* Cancellation before and during an already-expired capture gap wins
     * over the timeout, and re-entry gets a fresh budget. */
    for(i=0;i<2;i++){
        reset();capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
        assert(ai_send_uplink_20ms()==0);clock_ms+=3000U;
        if(i)cancel_capture=true;else active=false;
        assert(ai_send_uplink_20ms()==AI_ERR_CANCELLED && !s_capture_timeout_active);
        assert(captures==1U+i && sends==0 && encodes==0);
        active=true;cancel_capture=false;
        assert(ai_send_uplink_20ms()==0);
    }
    reset();capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
    assert(ai_send_uplink_20ms()==0);clock_ms+=5000U;s_running_request_id++;
    assert(ai_send_uplink_20ms()==0 && sends==0 && encodes==0);
    /* Network backpressure pauses capture, so it cannot consume a capture
     * failure budget. Level reconfiguration also starts a fresh attempt. */
    clock_ms+=5000U;buffered=AI_SEND_HIGH_WATER_BYTES+1U;
    assert(ai_send_uplink_20ms()==0 && !s_capture_timeout_active);
    buffered=0;clock_ms+=5000U;assert(ai_send_uplink_20ms()==0);
    reset();capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
    assert(ai_send_uplink_20ms()==0);clock_ms+=5000U;s_levels_dirty=true;
    queued=3;stop_on_play=false;capture_error=0;capture_timeouts_left=1;
    assert(ai_active_loop()==0 && captures==3 && sends==1 && encodes==1);
    assert(!s_capture_timeout_active && !s_levels_dirty);
#else
    reset();capture_error=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
    assert(ai_send_uplink_20ms()==DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT);
    assert(captures==1 && sends==0 && encodes==0);
#endif
}
static void test_connection_close_races(void){
    int stage,kind,expected;unsigned delay,before;
    for(stage=CLOSE_BUFFER;stage<=CLOSE_SEND;stage++){
        for(kind=0;kind<6;kind++){
            reset();close_stage=stage;sdk_error=TIRTC_E_INVALID_HANDLE;
            expected=TIRTC_E_INVALID_HANDLE;
            if(kind==0){close_error=TIRTC_E_CONN_REMOTECLOSE;expected=AI_ERR_CANCELLED;}
            if(kind==1){close_error=TIRTC_E_CONN_TIMEOUTCLOSE;expected=close_error;}
            if(kind==2){close_error=TIRTC_E_CONN_OTHER_ERROR;expected=close_error;close_disconnected=true;}
            if(kind==3){close_disconnected=true;expected=AI_ERR_CANCELLED;}
            /* kind 4 has no callback evidence: INVALID_HANDLE stays an error. */
            if(kind==5){close_error=TIRTC_E_CONN_REMOTECLOSE;sdk_error=TIRTC_E_TIMEOUTED;expected=sdk_error;}
            before=clock_ms;assert(ai_active_loop()==expected);
            assert(s_remote_ended==(expected==AI_ERR_CANCELLED));
            assert(captures==(stage==CLOSE_BUFFER ? 0U : 1U));
            assert(sends==(stage==CLOSE_BUFFER ? 0U : 1U));
            assert(clock_ms-before==(stage==CLOSE_BUFFER ? 0U : 20U)+(kind==4 ? 20U : 0U));
        }
    }
    for(stage=CLOSE_BUFFER;stage<=CLOSE_SEND;stage++){
        for(delay=1;delay<=2;delay++){
            for(kind=0;kind<3;kind++){
                reset();close_stage=stage;close_delay_ms=delay;sdk_error=TIRTC_E_INVALID_HANDLE;
                close_error=kind==0 ? TIRTC_E_CONN_REMOTECLOSE :
                    (kind==1 ? TIRTC_E_CONN_TIMEOUTCLOSE : TIRTC_E_CONN_OTHER_ERROR);
                expected=kind==0 ? AI_ERR_CANCELLED : close_error;
                before=clock_ms;assert(ai_active_loop()==expected);
                assert(!close_publication_pending && s_conn_error_pending);
                assert(s_remote_ended==(kind==0));
                /* A close scheduled in capture can arrive only once the
                 * resolver yields; the callback cannot be skipped by encode. */
                assert(clock_ms-before==(stage==CLOSE_BUFFER ? 0U : 20U)+delay);
            }
        }
    }
    reset();close_stage=CLOSE_BUFFER;close_delay_ms=21U;
    close_error=TIRTC_E_CONN_REMOTECLOSE;sdk_error=TIRTC_E_INVALID_HANDLE;
    before=clock_ms;assert(ai_active_loop()==TIRTC_E_INVALID_HANDLE);
    assert(clock_ms-before==20U && !s_conn_error_pending && !s_remote_ended);
    reset();s_conn_error_pending=true;s_conn_error=TIRTC_E_CONN_TIMEOUTCLOSE;
    assert(ai_active_loop()==TIRTC_E_CONN_TIMEOUTCLOSE);
    assert(!s_remote_ended && captures==0 && sends==0);
    reset();s_conn_error_pending=true;s_conn_error=TIRTC_E_CONN_REMOTECLOSE;
    assert(ai_active_loop()==AI_ERR_CANCELLED && s_remote_ended);
    assert(captures==0 && sends==0);
}
int main(void){
    int scenario,r;unsigned i;
    for(scenario=0;scenario<6;scenario++){
        reset();
        if(scenario==1)s_play_pending_samples=320;
        if(scenario==2)queued=1;
        if(scenario==3)queued=3;
        if(scenario==4){s_server_speaking=true;s_last_audio_ms=1000;}
        if(scenario==5){s_server_speaking=true;s_last_audio_ms=100;}
        r=ai_active_loop();assert(r==0);
#if DEMO_AI_AUDIO_AEC_ENABLE
        assert(captures==1 && sends==1 && encodes==1 && waits==0 && sleeps==0);
#else
        if(scenario==0 || scenario==5)assert(captures==1 && sends==1);
        else assert(captures==0 && sends==0);
#endif
    }
    reset();buffered=AI_SEND_HIGH_WATER_BYTES+1;assert(ai_active_loop()==0);
    assert(captures==0 && sends==0 && sleeps==1 && clock_ms==1020);
    reset();capture_error=-7;assert(ai_active_loop()==-7);
    assert(captures==1 && sends==0 && encodes==0);
    reset();cancel_encode=true;
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(ai_active_loop()==AI_ERR_CANCELLED);assert(captures==1 && sends==0 && encodes==1);
    for(i=0;i<320;i++)assert(s_capture_pcm[i]==0);
    for(i=0;i<sizeof(s_uplink_opus);i++)assert(s_uplink_opus[i]==0);
    reset();cancel_capture=true;assert(ai_active_loop()==AI_ERR_CANCELLED);
    assert(captures==1 && sends==0 && encodes==0);
    for(i=0;i<320;i++)assert(s_capture_pcm[i]==0);
    reset();queued=3;target_sends=50;stop_on_play=false;assert(ai_active_loop()==0);
    assert(sends==50 && captures==50 && plays==50 && clock_ms==2000 && waits==0 && sleeps==0);
    reset();tail_round=true;stop_on_play=false;target_sends=4;assert(ai_active_loop()==0);
    assert(sends==4 && plays==1 && !s_server_speaking && demo_ai_audio_play_done(0));
    assert(state==DEMO_AI_CHAT_LISTENING);
#else
    /* Retain the exact legacy behavior when the AEC branch is disabled. */
    assert(ai_active_loop()==0);assert(captures==1 && sends==1 && encodes==1);(void)i;
#endif
    test_connection_close_races();
    test_capture_timeout_budget();
    printf("PASS AI AEC=%d: playback/prebuffer/backpressure fairness, high-water, capture failure, conditional cancellation/tail completion\n",DEMO_AI_AUDIO_AEC_ENABLE);
    printf("PASS AI AEC=%d: 24 immediate + 24 delayed close races, 2 loop-entry cases, 20ms deadline; unknown handle and genuine errors preserved\n",DEMO_AI_AUDIO_AEC_ENABLE);
    printf("PASS AI AEC=%d: transient capture recovery, bounded persistent failure, clock wrap, cancel/pause/level/new-session resets, no stale PCM; legacy errors preserved\n",DEMO_AI_AUDIO_AEC_ENABLE);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--source", type=Path, help="Optional baseline ai_chat.c for negative regression")
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    app = Path(__file__).resolve().parents[1]
    current_source = (app / "src/features/ai_chat.c").read_text(encoding="utf-8")
    source = args.source.read_text(encoding="utf-8") if args.source else current_source
    header = (app / "include/audio_device.h").read_text(encoding="utf-8")
    constants = "\n".join(re.search(r"^#define\s+" + name + r"\s+[^\r\n]+", text, re.M).group()
                          for name, text in (("AI_CAPTURE_TIMEOUT_RETRY_MS", current_source),
                                             ("DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT", header)))
    body = (constants + "\n" + PREFIX + function(source, "ai_deadline_pending") +
            function(source, "ai_normalize_session_result") +
            function(source, "ai_connection_end_result") +
            function(source, "ai_resolve_uplink_result") +
            function(source, "ai_send_uplink_20ms") + function(source, "ai_active_loop") + SUFFIX)
    with tempfile.TemporaryDirectory(prefix="lite-ai-aec-loop-") as temp:
        for enabled in (0, 1):
            c = Path(temp) / f"ai-loop-{enabled}.c"
            exe = c.with_suffix(".exe")
            c.write_text(body, encoding="utf-8")
            subprocess.run(cc + ["-std=c99", "-O2", f"-DDEMO_AI_AUDIO_AEC_ENABLE={enabled}",
                                 str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: production AI AEC loop regression")


if __name__ == "__main__":
    main()
