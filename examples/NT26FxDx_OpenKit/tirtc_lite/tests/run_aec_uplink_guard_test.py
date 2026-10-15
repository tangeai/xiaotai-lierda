#!/usr/bin/env python3
"""Run production WX/LIVE/GROUP uplink functions with deterministic cancellation.

Only SDK/RTOS boundaries are faked. Functions, including KEY1 debounce and the
GROUP toggle, are extracted from current source; no test seam enters firmware.
These checks cover permission changes during capture/encoding, not real radio
or acoustic behavior. Both AEC settings are exercised.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from run_dev_playback_gain_test import compiler_command


def function(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if match is None:
        raise AssertionError("Production function missing: " + name)
    start = source.rfind("\n", 0, match.start()) + 1
    opening = source.index("{", match.start())
    clean = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                   lambda m: " " * len(m.group()), source, flags=re.S)
    depth = 1
    end = opening + 1
    while depth:
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    return source[start:end] + "\n"


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DEMO_WECHAT_IN_CALL 1
#define WX_ERR_AUDIO (-3)
#define TIRTC_E_BUSY (-4)
#define DEMO_TIRTC_OWNER_WECHAT 1
#define DEMO_TIRTC_OWNER_LIVE 2
#define DEMO_TIRTC_OWNER_GROUP_ROOM 3
#define DEMO_GROUP_CALL_NONE 0
#define TIRTC_AUDIO_ALAW 1
#define TIRTC_AUDIOSAMPLE_8K16B1C 0
#define WX_STREAM_ID 0
#define LIVE_UPLINK_STREAM_ID 10
#define ROOM_STREAM 1
#define WX_UPLINK_SAMPLES_8K 160
#define LIVE_UPLINK_8K_SAMPLES 160
#define ROOM_RX_SLOTS 8
#define ROOM_RX_MAX_AGE 160
#define ROOM_SEND_HIGH_WATER 12000
#define ROOM_ERR_AUDIO (-5)
#define KEY_COUNT 3
#define KEY_DEBOUNCE_MS 80
#define liot_trace(...) ((void)0)
static void liot_rtos_enter_critical(void);
static void liot_rtos_exit_critical(void);
typedef void *tirtc_conn_t;
typedef struct { unsigned stream_id, media, flags, ts, length; } TIRTCFRAMEINFO;
static tirtc_conn_t s_conn;
static unsigned s_generation,s_tirtc_session_generation,s_epoch,s_mic_capture_generation;
static bool s_tirtc_owned,s_audio_owned,s_request_hangup,s_remote_hangup_pending;
static bool s_conn_error_pending,s_disconnected_pending,s_home_allowed,s_active;
static bool s_remote_audio_enabled,s_stop_requested,s_remote_disconnected,s_uplink_subscribed;
#if DEMO_AI_AUDIO_AEC_ENABLE
static bool s_record_failure_active;
static uint32_t s_record_failure_since,s_record_failure_generation,s_uplink_epoch;
#endif
static bool s_mic_gate,s_accept_audio,s_enabled,s_foreground,s_stopping,s_mic_dirty;
static int s_state,s_connection_error,s_call,s_audio_lease,s_audio;
static int16_t s_capture_pcm[320],s_capture[320],s_play[1280];
static uint8_t s_uplink_alaw[160],s_alaw[160];
static unsigned s_tx_dropped,s_tx_frames,s_session_started;
static unsigned s_rx_count,s_rx_read,s_rx_bytes,s_rx_ring[ROOM_RX_SLOTS];
static struct { unsigned length,epoch,received_ms; bool used; uint8_t bytes[640]; } s_rx[ROOM_RX_SLOTS];
static struct { unsigned rx_dropped,tx_dropped; } s_snapshot;
static unsigned s_key_last_tick[KEY_COUNT];
static unsigned captures,sends,encodes,clock_ms,event_mode,event_phase;
static int capture_ret,failed_code;
static unsigned critical_depth,critical_exits,inject_exit;
static bool pause_quick,pause_command;
#if TEST_KIND == 1
#define LIVE_STOP_REMOTE_DISCONNECTED 1
static int s_stop_reason;
static int live_on_subscribe_audio(tirtc_conn_t,uint8_t);
static void live_on_unsubscribe_audio(tirtc_conn_t,uint8_t);
static void live_on_command(tirtc_conn_t,uint32_t,const void *,uint32_t);
#endif
static void pause_resume(void){
#if TEST_KIND == 1
    uint8_t off=0,on=1;
    if(pause_command) live_on_command(s_conn,LIVE_CMD_SET_SEND_AUDIO,&off,1);
    else live_on_unsubscribe_audio(s_conn,LIVE_UPLINK_STREAM_ID);
    if(pause_quick){
        if(pause_command) live_on_command(s_conn,LIVE_CMD_SET_SEND_AUDIO,&on,1);
        else assert(live_on_subscribe_audio(s_conn,LIVE_UPLINK_STREAM_ID)==0);
    }
#endif
}
static void liot_rtos_enter_critical(void){++critical_depth;}
static void liot_rtos_exit_critical(void){
    assert(critical_depth);--critical_depth;
    if(!critical_depth && ++critical_exits==inject_exit){
        inject_exit=0;pause_resume();
    }
}
static bool live_matches_connection_locked(tirtc_conn_t h){return h==s_conn;}
static void live_signal(void){}
static bool key_debounce_accept(uint8_t key_index);
static uint32_t liot_rtos_get_system_tick(void){return clock_ms;}
static uint32_t liot_rtos_get_running_time(void){return clock_ms;}
static uint32_t room_next(uint32_t n){return ++n ? n : 1U;}
static void room_wake(void){}
static void room_zero(void *p,size_t n){memset(p,0,n);}
static void room_fail(int error){failed_code=error;}
#if TEST_KIND == 2
bool demo_group_intercom_toggle_mic_for_generation(uint32_t generation);
#endif
static void event(void){
    switch(event_mode){
    case 1:
#if TEST_KIND == 0
        s_request_hangup=true;
#elif TEST_KIND == 1
        s_stop_requested=true;
#else
        assert(demo_group_intercom_toggle_mic_for_generation(s_generation));
#endif
        break;
    case 2:
#if TEST_KIND == 2
        ++s_epoch;
#else
        ++s_generation;
#endif
        break;
    case 3:s_conn=(void *)2;break;
    case 4:s_remote_audio_enabled=false;break;
    case 5:s_home_allowed=false;s_stop_requested=true;break;
    case 6:
#if TEST_KIND == 2
        /* Both presses satisfy the production ISR debounce. The final PCM
         * arrives 100 ms into the still-valid 120 ms capture wait. */
        assert(key_debounce_accept(1));
        assert(demo_group_intercom_toggle_mic_for_generation(s_generation));
        clock_ms+=80;
        assert(key_debounce_accept(1));
        assert(demo_group_intercom_toggle_mic_for_generation(s_generation));
        clock_ms+=20;
#endif
        break;
    case 7:++s_tirtc_session_generation;break;
    case 8:pause_resume();break;
    default:break;
    }
}
static int demo_ai_audio_record_20ms(void *lease,int16_t *pcm){
    unsigned i;(void)lease;captures++;
    for(i=0;i<320;i++)pcm[i]=(int16_t)(i+1);
    if(capture_ret==DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT)clock_ms+=120U;
    if(event_phase==1)event();
    return capture_ret;
}
static uint8_t demo_g711_alaw_encode_sample(int16_t pcm){
    (void)pcm;if(encodes++==0 && event_phase==2)event();return 0xD5;
}
static int16_t demo_g711_alaw_decode_sample(uint8_t b){return b;}
static int demo_ai_audio_play(void *l,const int16_t *p,unsigned n){(void)l;(void)p;(void)n;return 0;}
static int demo_tirtc_get_send_buffer_used(int o,unsigned g,size_t *u){(void)o;(void)g;*u=0;return 0;}
static int demo_tirtc_send_audio(int owner,unsigned generation,const TIRTCFRAMEINFO *f,const void *data){
    (void)owner;(void)data;
    assert(f->media==TIRTC_AUDIO_ALAW && f->flags==TIRTC_AUDIOSAMPLE_8K16B1C && f->length==160);
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(generation==1);
#else
    (void)generation;
#endif
    sends++;return 0;
}
'''

SUFFIX = r'''
static void reset(void){
    s_conn=(void *)1;s_generation=s_tirtc_session_generation=s_epoch=1;
    s_mic_capture_generation=0;s_tirtc_owned=s_audio_owned=true;
    s_request_hangup=s_remote_hangup_pending=s_conn_error_pending=s_disconnected_pending=false;
    s_home_allowed=s_active=s_remote_audio_enabled=true;s_stop_requested=s_remote_disconnected=false;
    s_mic_gate=s_accept_audio=s_enabled=s_foreground=true;s_stopping=s_mic_dirty=false;
    s_state=DEMO_WECHAT_IN_CALL;s_connection_error=s_call=0;s_rx_count=0;
    captures=sends=encodes=0;clock_ms=1000;event_mode=event_phase=0;
    capture_ret=failed_code=0;
    assert(!critical_depth);critical_exits=inject_exit=0;pause_quick=pause_command=false;
#if DEMO_AI_AUDIO_AEC_ENABLE
    s_record_failure_active=false;s_record_failure_since=s_record_failure_generation=s_uplink_epoch=0;
#endif
    memset(s_capture_pcm,0x7F,sizeof(s_capture_pcm));memset(s_capture,0x7F,sizeof(s_capture));
    memset(s_key_last_tick,0,sizeof(s_key_last_tick));
}
static int invoke(void){
#if TEST_KIND == 0
    return wx_send_uplink();
#elif TEST_KIND == 1
    return live_send_uplink_20ms((void *)1);
#else
    failed_code=0;room_audio_tick();return failed_code;
#endif
}
static void run(void){assert(invoke()==0);}
static void check(unsigned mode,unsigned phase,unsigned expect_send){
    unsigned i;int16_t *pcm;
    reset();event_mode=mode;event_phase=phase;run();assert(captures==1 && sends==expect_send);
    pcm=TEST_KIND==2 ? s_capture : s_capture_pcm;
#if DEMO_AI_AUDIO_AEC_ENABLE || TEST_KIND == 0
    if(expect_send==0)for(i=0;i<320;i++)assert(pcm[i]==0);
#else
    (void)i;(void)pcm;
#endif
}
static void capture_timeout_cases(void){
#if DEMO_AI_AUDIO_AEC_ENABLE
    for(unsigned wrapping=0;wrapping<2;wrapping++){
        reset();clock_ms=wrapping ? UINT32_MAX-500U : 1000U;
        capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
        assert(captures==1 && sends==0 && encodes==0 && s_record_failure_active);
        uint32_t first=s_record_failure_since;
        clock_ms=first+1999U-120U;run();assert(!sends && !encodes);
        clock_ms=first+2000U-120U;assert(invoke()<0);assert(!sends && !encodes);
        reset();capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
        capture_ret=0;run();assert(sends==1 && !s_record_failure_active);
        clock_ms+=3000U;capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
        assert(sends==1 && s_record_failure_active && s_record_failure_since==clock_ms);
    }
    reset();capture_ret=-99;assert(invoke()<0);assert(!sends && !encodes);
    reset();capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
    clock_ms+=3000U;event_mode=1;event_phase=1;run();
    assert(!s_record_failure_active && !sends && !encodes);
#if TEST_KIND == 1
    reset();capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
    live_on_unsubscribe_audio(s_conn,LIVE_UPLINK_STREAM_ID);
    assert(!s_record_failure_active);
    assert(live_on_subscribe_audio(s_conn,LIVE_UPLINK_STREAM_ID)==0);
    clock_ms+=3000U;run();assert(!sends && s_record_failure_since==clock_ms);
#elif TEST_KIND == 2
    reset();capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
    assert(demo_group_intercom_toggle_mic_for_generation(s_generation));
    assert(demo_group_intercom_toggle_mic_for_generation(s_generation));
    clock_ms+=3000U;run();assert(!sends && s_record_failure_since==clock_ms);
#endif
    printf("PASS feature=%d: isolated/persistent capture gap, success reset, uint32 wrap, cancellation and fast pause/PTT reset\n",TEST_KIND);
#endif
}
static void live_pause_race_cases(void){
#if TEST_KIND == 1 && DEMO_AI_AUDIO_AEC_ENABLE
    for(unsigned command=0;command<2;command++){
        for(unsigned quick=0;quick<2;quick++){
            /* Exact old race: callback immediately after capture's gate-check
             * critical exit, before the old second budget critical section. */
            reset();capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
            pause_command=command;pause_quick=quick;inject_exit=2U;run();
            assert(!s_record_failure_active && !sends && !encodes && inject_exit==0U);
            if(!quick)assert(live_on_subscribe_audio(s_conn,LIVE_UPLINK_STREAM_ID)==0);
            clock_ms+=3000U;run();
            assert(s_record_failure_active && s_record_failure_since==clock_ms && !sends);
            /* A capture spanning OFF/ON cannot re-open an old failure window
             * or publish old PCM even when the current permission is true. */
            for(unsigned outcome=0;outcome<3;outcome++){
                reset();capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;run();
                clock_ms+=3000U;pause_command=command;pause_quick=quick;
                capture_ret=outcome==0 ? DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT : outcome==1 ? -99 : 0;
                event_mode=8;event_phase=1;run();
                assert(!s_record_failure_active && !sends && !encodes);
                event_mode=event_phase=0;capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
                if(!quick)assert(live_on_subscribe_audio(s_conn,LIVE_UPLINK_STREAM_ID)==0);
                run();assert(s_record_failure_active && s_record_failure_since==clock_ms);
            }
        }
        reset();pause_command=command;pause_quick=true;event_mode=8;event_phase=2;
        run();assert(!sends); /* Quick pause during encoding retires this PCM. */
        reset();s_uplink_epoch=UINT32_MAX;pause_command=command;pause_quick=true;
        event_mode=8;event_phase=1;capture_ret=DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT;
        run();assert(s_uplink_epoch==0U && !s_record_failure_active && !sends);
    }
    puts("PASS LIVE: exact post-capture pause race, rapid OFF/ON, cancellation over errors, stale PCM and epoch wrap");
#endif
}
int main(void){
    unsigned phase,mode;
    live_pause_race_cases();
    capture_timeout_cases();
    check(0,0,1);
    for(phase=1;phase<=2;phase++){
#if TEST_KIND == 2
        check(1,phase,0); /* A single close already works in both backends. */
        check(2,phase,0); /* Session epoch invalidation remains unchanged. */
        check(6,phase,DEMO_AI_AUDIO_AEC_ENABLE ? 0 : 1);
#else
        for(mode=1;mode<=7;mode++){
            if(mode==6 || (TEST_KIND==0 && (mode==4 || mode==5)))continue;
            check(mode,phase,(DEMO_AI_AUDIO_AEC_ENABLE || TEST_KIND==0) ? 0 : 1);
        }
#endif
    }
    reset();
#if TEST_KIND == 0
    s_request_hangup=true;
#elif TEST_KIND == 1
    s_remote_audio_enabled=false;
#else
    s_mic_gate=false;
#endif
    run();
    assert(captures==((DEMO_AI_AUDIO_AEC_ENABLE || TEST_KIND!=1) ? 0U : 1U));
    assert(sends==((DEMO_AI_AUDIO_AEC_ENABLE || TEST_KIND!=1) ? 0U : 1U));
    printf("PASS feature=%d AEC=%d: normal, before/during capture, during encode, generation and permission gates\n",TEST_KIND,DEMO_AI_AUDIO_AEC_ENABLE);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    app = Path(__file__).resolve().parents[1]
    audio_header = (app / "include/audio_device.h").read_text(encoding="utf-8")
    play_busy = "\n".join(re.search(r"^#define " + name + r"[^\n]*", audio_header, re.M).group()
                          for name in ("DEMO_AI_AUDIO_ERR_PLAY_BUSY", "DEMO_AI_AUDIO_ERR_CAPTURE_TIMEOUT"))
    keys = (app / "src/ui/key_input.c").read_text(encoding="utf-8")
    groups = (
        ("wechat_call.c", "wx_send_uplink", "wx_uplink_is_current_locked"),
        ("platform_intercom.c", "live_send_uplink_20ms", "live_uplink_is_current_locked"),
        ("group_intercom.c", "room_audio_tick", "demo_group_intercom_toggle_mic_for_generation"),
    )
    with tempfile.TemporaryDirectory(prefix="lite-aec-uplink-") as temp:
        for kind, (filename, sender, guard) in enumerate(groups):
            source = (app / "src/features" / filename).read_text(encoding="utf-8")
            helper = function(source, guard)
            if kind == 1:
                helper = "#if DEMO_AI_AUDIO_AEC_ENABLE\n" + helper + "#endif\n"
            if kind == 0:
                job = re.search(r'typedef struct\s*\{\s*uint32_t generation;\s*uint32_t tirtc_generation;.*?\} wx_uplink_job_t;', source, re.S).group()
                # This suite probes capture/encode cancellation at enqueue.
                # Real queue ownership and delayed sending have their own test.
                helper = job + r'''
#define LIOT_NO_WAIT 0
#define LIOT_OSI_SUCCESS 0
typedef uint8_t uint8;
static void *s_uplink_queue=(void *)3;
static unsigned s_tx_session_queue_full;
static int liot_rtos_queue_release(void *q,unsigned size,uint8 *bytes,unsigned timeout){
    const wx_uplink_job_t *job=(const wx_uplink_job_t *)bytes;
    assert(q==s_uplink_queue && size==sizeof(*job) && timeout==LIOT_NO_WAIT && !critical_depth);
    return demo_tirtc_send_audio(DEMO_TIRTC_OWNER_WECHAT,job->tirtc_generation,&job->frame,job->payload);
}
''' + helper
            limit = re.search(r"^#define \w+_RECORD_FAILURE_LIMIT_MS[^\n]*", source, re.M).group()
            if kind == 1:
                helper += function(source, "live_on_subscribe_audio") + function(source, "live_on_unsubscribe_audio") + function(source, "live_on_command")
                limit += "\n" + "\n".join(re.findall(r"^#define LIVE_CMD_.*$", source, re.M))
            body = play_busy + "\n" + limit + "\n" + PREFIX + function(keys, "key_debounce_accept") + helper + function(source, sender) + SUFFIX
            for enabled in (0, 1):
                c = Path(temp) / f"feature-{kind}-{enabled}.c"
                exe = c.with_suffix(".exe")
                c.write_text(body, encoding="utf-8")
                subprocess.run(cc + ["-std=c99", "-O2", f"-DTEST_KIND={kind}",
                                     f"-DDEMO_AI_AUDIO_AEC_ENABLE={enabled}", str(c), "-o", str(exe)], check=True)
                subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: production WX/LIVE/GROUP AEC uplink cancellation guards")


if __name__ == "__main__":
    main()
