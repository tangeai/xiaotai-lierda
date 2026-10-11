#!/usr/bin/env python3
"""Exercise production AI remote-end tail draining with deterministic boundaries.

The actual tail function, request gate, result normalizer, and manager's finish
sequence are extracted from ai_chat.c. Queue/producer timing, playback completion,
and command interruption are fakes. This checks control flow and deadlines, not
vendor DMA completion, JSON parsing, network timing, or audible board behavior.
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
#include <stdio.h>
#include <string.h>
#define TIRTC_E_INVALID_HANDLE (-40002)
#define TIRTC_E_CONN_REMOTECLOSE (-40008)
#define TIRTC_E_CONN_OTHER_ERROR (-40009)
#define liot_trace(...) ((void)0)
static bool s_remote_ended,s_audio_owned,s_call_committed,s_conn_error_pending;
static bool s_disconnected_pending,s_want_active;
static int s_conn_error,s_audio_lease;
static uint32_t s_running_request_id,s_control_sequence,s_callback_producers;
static uint32_t s_play_pending_samples;
static bool s_audio_pool_used[AI_AUDIO_QUEUE_DEPTH];
static void *s_audio_queue = (void *)1;
static uint32_t clock_ms,elapsed,queued,done_at,busy_until,event_at,continuous_until;
static uint32_t next_submit,submit_interval;
static unsigned plays,accepted,sleeps,done_calls,command_polls,critical_depth;
static bool stuck,continuous,interrupt_pending,queue_race,queue_race_injected;
static int event_kind;
enum { EV_NONE,EV_AUDIO,EV_PRODUCER_FINISH,EV_BACK,EV_REQUEST,EV_CALL,
       EV_ERROR,EV_REMOTE_CLOSE,EV_DISCONNECT,EV_LOSE_AUDIO,EV_INTERRUPT };
static unsigned passed;
static void pool_sync(void) {
    memset(s_audio_pool_used,0,sizeof(s_audio_pool_used));
    s_audio_pool_used[0] = queued != 0;
}
static void enqueue(void) { queued++;pool_sync(); }
static void liot_rtos_enter_critical(void) { critical_depth++; }
static void liot_rtos_exit_critical(void) { assert(critical_depth);critical_depth--; }
static uint32_t liot_rtos_get_running_time(void) { return clock_ms; }
static int liot_rtos_queue_get_cnt(void *queue,uint32_t *count) {
    assert(queue==s_audio_queue);*count=queued;
    /* Simulate producer enqueue+release after the sampled empty count. */
    if(queue_race && !queue_race_injected && elapsed==AI_REMOTE_TAIL_QUIET_MS) {
        assert(queued==0);enqueue();queue_race_injected=true;
        assert(s_callback_producers==0);
    }
    return 0;
}
static bool demo_ai_audio_play_done(void *lease) {
    assert(lease==&s_audio_lease);done_calls++;
    return !stuck && elapsed>=done_at;
}
static bool ai_remote_tail_interrupted(void) { command_polls++;return interrupt_pending; }
static int ai_play_downlink(void) {
    assert(critical_depth==0);plays++;
    assert(s_play_pending_samples || queued);
    if(!s_play_pending_samples) { queued--;pool_sync();s_play_pending_samples=320; }
    if(elapsed<busy_until || elapsed<next_submit)return 0;
    s_play_pending_samples=0;accepted++;done_at=elapsed+80U;
    next_submit=elapsed+submit_interval;
    return 1;
}
static void liot_rtos_task_sleep_ms(uint32_t ms) {
    assert(critical_depth==0 && ms==AI_ACTIVE_POLL_MS);
    sleeps++;clock_ms+=ms;elapsed+=ms;assert(elapsed<=10000U);
    if(continuous && (!continuous_until || elapsed<continuous_until))enqueue();
    if(event_kind!=EV_NONE && elapsed>=event_at) {
        switch(event_kind) {
        case EV_AUDIO:enqueue();break;
        case EV_PRODUCER_FINISH:enqueue();s_callback_producers=0;break;
        case EV_BACK:s_want_active=false;break;
        case EV_REQUEST:s_control_sequence++;break;
        case EV_CALL:s_call_committed=true;break;
        case EV_ERROR:s_conn_error_pending=true;s_conn_error=TIRTC_E_CONN_OTHER_ERROR;break;
        case EV_REMOTE_CLOSE:s_conn_error_pending=true;s_conn_error=TIRTC_E_CONN_REMOTECLOSE;break;
        case EV_DISCONNECT:s_disconnected_pending=true;break;
        case EV_LOSE_AUDIO:s_audio_owned=false;break;
        case EV_INTERRUPT:interrupt_pending=true;break;
        default:assert(0);
        }
        event_kind=EV_NONE;
    }
}
static void reset(void) {
    s_remote_ended=true;s_audio_owned=true;s_call_committed=false;
    s_conn_error_pending=false;s_disconnected_pending=false;s_conn_error=0;
    s_want_active=true;s_running_request_id=s_control_sequence=7;
    s_callback_producers=s_play_pending_samples=0;
    clock_ms=1000;elapsed=queued=done_at=busy_until=event_at=continuous_until=0;
    next_submit=submit_interval=0;
    plays=accepted=sleeps=done_calls=command_polls=critical_depth=0;
    stuck=continuous=interrupt_pending=queue_race=queue_race_injected=false;
    event_kind=EV_NONE;pool_sync();
}
static void pass(const char *name) {
    assert(critical_depth==0);passed++;
    printf("PASS AEC=%d %s\n",DEMO_AI_AUDIO_AEC_ENABLE,name);
}
'''

TESTS = r'''
#if DEMO_AI_AUDIO_AEC_ENABLE
static void test_guards(void) {
    unsigned i;
    for(i=0;i<10;i++) {
        int result=AI_ERR_CANCELLED;
        reset();enqueue();
        switch(i) {
        case 0:s_remote_ended=false;break;
        case 1:result=TIRTC_E_CONN_OTHER_ERROR;break;
        case 2:s_audio_owned=false;break;
        case 3:s_want_active=false;break;
        case 4:s_control_sequence++;break;
        case 5:s_running_request_id=0;break;
        case 6:s_call_committed=true;break;
        case 7:s_conn_error_pending=true;s_conn_error=TIRTC_E_CONN_OTHER_ERROR;break;
        case 8:result=TIRTC_E_INVALID_HANDLE;break;
        case 9:result=-12345;break;
        }
        ai_finish_remote_audio(result);
        assert(elapsed==0 && plays==0 && command_polls==0 && queued==1);
        assert(result==(i==1 ? TIRTC_E_CONN_OTHER_ERROR : i==8 ? TIRTC_E_INVALID_HANDLE : i==9 ? -12345 : AI_ERR_CANCELLED));
        pass("entry guard preserves result and pending audio");
    }
}
static void test_completion(void) {
    int result=AI_ERR_CANCELLED;
    reset();ai_finish_remote_audio(result);
    assert(elapsed==AI_REMOTE_TAIL_QUIET_MS && plays==0 && result==AI_ERR_CANCELLED);
    pass("empty remote end waits for late audio");
    reset();clock_ms=0;ai_finish_remote_audio(0);
    assert(elapsed==AI_REMOTE_TAIL_QUIET_MS);pass("quiet window accepts zero clock");
    reset();enqueue();enqueue();enqueue();ai_finish_remote_audio(0);
    assert(accepted==3 && queued==0 && s_play_pending_samples==0);
    assert(elapsed==1300);pass("received tail drains before 1200ms quiet");
    reset();event_kind=EV_AUDIO;event_at=190;ai_finish_remote_audio(0);
    assert(accepted==1 && elapsed==1470);pass("late final frame resets quiet window");
    reset();event_kind=EV_AUDIO;event_at=250;ai_finish_remote_audio(0);
    assert(accepted==1 && elapsed==1530);pass("250ms late media survives the extended quiet window");
    reset();event_kind=EV_AUDIO;event_at=900;ai_finish_remote_audio(0);
    assert(accepted==1 && elapsed==2180);pass("900ms late media survives the extended quiet window");
    reset();s_callback_producers=1;event_kind=EV_PRODUCER_FINISH;event_at=240;
    ai_finish_remote_audio(0);assert(accepted==1 && elapsed==1520);
    pass("in-flight producer prevents early completion");
    reset();queue_race=true;ai_finish_remote_audio(0);
    assert(queue_race_injected && accepted==1 && elapsed==2490);
    pass("post-count enqueue is visible through pool ownership");
    reset();enqueue();busy_until=100;ai_finish_remote_audio(0);
    assert(accepted==1 && plays==11 && s_play_pending_samples==0 && elapsed==1380);
    pass("busy playback retains one pending frame without duplication");
    reset();s_play_pending_samples=320;busy_until=30;ai_finish_remote_audio(0);
    assert(accepted==1 && plays==4 && elapsed==1310);
    pass("already decoded pending frame drains");
    reset();s_conn_error_pending=true;s_conn_error=TIRTC_E_CONN_REMOTECLOSE;
    enqueue();ai_finish_remote_audio(AI_ERR_CANCELLED);
    assert(accepted==1 && elapsed==1280 && s_conn_error==TIRTC_E_CONN_REMOTECLOSE);
    pass("normal remote close is eligible to drain");
    reset();s_conn_error_pending=true;s_conn_error=0;
    ai_finish_remote_audio(0);assert(elapsed==1200);pass("empty error publication is not a genuine failure");
}
static void test_deadlines(void) {
    reset();continuous=true;continuous_until=4000;enqueue();ai_finish_remote_audio(0);
    assert(elapsed==5270 && accepted==400 && queued==0);
    pass("healthy media continues past three seconds and then drains");
    reset();queued=200;pool_sync();submit_interval=20;ai_finish_remote_audio(0);
    assert(elapsed==5260 && accepted==200 && queued==0 && s_play_pending_samples==0);
    pass("four seconds of existing healthy backlog is not cut at three seconds");
    reset();enqueue();event_kind=EV_AUDIO;event_at=2000;busy_until=2900;
    ai_finish_remote_audio(0);assert(elapsed==4190 && accepted==2);
    pass("successful late submission resets the no-progress deadline");
    reset();stuck=true;ai_finish_remote_audio(0);
    assert(elapsed==AI_REMOTE_TAIL_TIMEOUT_MS && plays==0);
    pass("stuck playback completion exits after three seconds without progress");
    reset();stuck=true;busy_until=500;enqueue();ai_finish_remote_audio(0);
    assert(elapsed==3500 && accepted==1);
    pass("a later stall is measured from the last successful submission");
    reset();enqueue();busy_until=UINT32_MAX;ai_finish_remote_audio(0);
    assert(elapsed==AI_REMOTE_TAIL_TIMEOUT_MS && accepted==0 && s_play_pending_samples==320);
    pass("persistent backpressure preserves pending frame until cleanup");
    reset();s_callback_producers=1;ai_finish_remote_audio(0);
    assert(elapsed==AI_REMOTE_TAIL_TIMEOUT_MS);pass("stuck callback producer has a bounded timeout");
    reset();s_audio_pool_used[AI_AUDIO_QUEUE_DEPTH-1]=true;ai_finish_remote_audio(0);
    assert(elapsed==AI_REMOTE_TAIL_TIMEOUT_MS);pass("every occupied pool slot blocks quiet completion");
    reset();clock_ms=UINT32_MAX-100U;event_kind=EV_AUDIO;event_at=190;
    ai_finish_remote_audio(0);assert(elapsed==1470 && accepted==1 && clock_ms==1369U);
    pass("late tail and quiet deadline survive uint32 clock wrap");
    reset();clock_ms=UINT32_MAX-100U;stuck=true;ai_finish_remote_audio(0);
    assert(elapsed==AI_REMOTE_TAIL_TIMEOUT_MS && clock_ms==2899U);
    pass("no-progress timeout survives uint32 clock wrap");
}
static void test_interruptions(void) {
    int kind;
    for(kind=EV_BACK;kind<=EV_INTERRUPT;kind++) {
        bool normal=kind==EV_REMOTE_CLOSE || kind==EV_DISCONNECT;
        reset();event_kind=kind;event_at=30;enqueue();busy_until=100;
        ai_finish_remote_audio(AI_ERR_CANCELLED);
        if(normal) {
            assert(elapsed==1380 && accepted==1 && s_play_pending_samples==0);
        } else {
            assert(elapsed==30 && accepted==0 && plays==3 && s_play_pending_samples==320);
            if(kind==EV_ERROR)assert(s_conn_error_pending && s_conn_error==TIRTC_E_CONN_OTHER_ERROR);
        }
        pass(normal ? "normal close during tail still drains" : "local interruption outranks pending tail within one tick");
    }
    reset();interrupt_pending=true;enqueue();ai_finish_remote_audio(0);
    assert(elapsed==0 && plays==0 && queued==1);pass("already queued interrupt wins before playback");
    reset();event_kind=EV_AUDIO;event_at=AI_REMOTE_TAIL_QUIET_MS;ai_finish_remote_audio(0);
    assert(accepted==1 && elapsed==2480);pass("audio at quiet deadline is not dropped");
    for(kind=EV_BACK;kind<=EV_INTERRUPT;kind++) {
        if(kind==EV_REMOTE_CLOSE || kind==EV_DISCONNECT)continue;
        reset();continuous=true;enqueue();event_kind=kind;event_at=5000;
        ai_finish_remote_audio(AI_ERR_CANCELLED);
        assert(elapsed==5000 && accepted==500 && queued==1);
        pass("cancellation still preempts a healthy long-running tail");
    }
}
#endif
static unsigned manager_step,manager_tail_calls,manager_cleanup_calls;
static int manager_input,manager_normalized;
static int ai_run_session(void) { assert(manager_step==0);manager_step=1;return manager_input; }
static int observed_normalize(int result) {
    assert(manager_step==1);manager_step=2;
    manager_normalized=ai_normalize_session_result(result);return manager_normalized;
}
#if DEMO_AI_AUDIO_AEC_ENABLE
static void observed_finish(int result) {
    assert(manager_step==2 && result==manager_normalized);manager_step=3;manager_tail_calls++;
    ai_finish_remote_audio(result);
}
#endif
static void ai_session_cleanup(void) {
    assert(manager_step==(DEMO_AI_AUDIO_AEC_ENABLE ? 3U : 2U));
    manager_step=4;manager_cleanup_calls++;
}
#define ai_normalize_session_result observed_normalize
#define ai_finish_remote_audio observed_finish
static int manager_finish(void) {
    int ret;
'''

END = r'''
    return ret;
}
#undef ai_normalize_session_result
#undef ai_finish_remote_audio
static void test_manager(void) {
    unsigned i;
    for(i=0;i<5;i++) {
        int result;
        reset();s_remote_ended=false;enqueue();
        manager_step=manager_tail_calls=manager_cleanup_calls=0;
        manager_input=i==0 ? TIRTC_E_CONN_REMOTECLOSE : i==1 ? TIRTC_E_INVALID_HANDLE : i==2 ? TIRTC_E_CONN_OTHER_ERROR : i==3 ? 0 : AI_ERR_CANCELLED;
        if(i==1)s_disconnected_pending=true;
        if(i==3 || i==4)s_remote_ended=true;
        result=manager_finish();
        assert(result==(i<2 ? AI_ERR_CANCELLED : manager_input));
        assert(manager_step==4 && manager_cleanup_calls==1 && manager_tail_calls==DEMO_AI_AUDIO_AEC_ENABLE);
        if(DEMO_AI_AUDIO_AEC_ENABLE && i!=2)assert(accepted==1 && elapsed==1280);
        else assert(accepted==0 && elapsed==0 && queued==1);
        pass("manager normalizes then conditionally drains before cleanup without changing result");
    }
}
int main(void) {
    assert(AI_AUDIO_QUEUE_DEPTH==(DEMO_AI_AUDIO_AEC_ENABLE ? 256U : 16U));
#if DEMO_AI_AUDIO_AEC_ENABLE
    test_guards();test_completion();test_deadlines();test_interruptions();
#endif
    test_manager();
    printf("PASS AI remote tail AEC=%d: %u scenarios\n",DEMO_AI_AUDIO_AEC_ENABLE,passed);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    app = Path(__file__).resolve().parents[1]
    source = (app / "src/features/ai_chat.c").read_text(encoding="utf-8")
    names = ("AI_REMOTE_TAIL_TIMEOUT_MS", "AI_REMOTE_TAIL_QUIET_MS",
             "AI_ACTIVE_POLL_MS", "AI_AUDIO_QUEUE_DEPTH", "AI_ERR_CANCELLED")
    start = re.search(r"^#define\s+AI_CMD_WORD\b", source, re.M)
    end = re.search(r"^#define\s+AI_ERR_CANCELLED\s+[^\n]+", source, re.M)
    if start is None or end is None:
        raise AssertionError("Production constant block missing")
    # Keep the source's conditional AEC=256 / non-AEC=16 queue capacity.
    # Selecting the first matching #define would silently test 256 in both.
    defines = source[start.start():end.end()]
    for name in names:
        match = re.search(r"^#define\s+" + name + r"\s+[^\n]+", defines, re.M)
        if match is None:
            raise AssertionError("Production constant missing: " + name)
    sequence = re.findall(r"ret = ai_run_session\(\);.*?ai_session_cleanup\(\);", source, re.S)
    if len(sequence) != 1:
        raise AssertionError("Expected one production session finish sequence")
    body = (defines + "\n" + PREFIX +
            function(source, "ai_request_is_active") +
            function(source, "ai_normalize_session_result") +
            "#if DEMO_AI_AUDIO_AEC_ENABLE\n" + function(source, "ai_finish_remote_audio") +
            "#endif\n" + TESTS + sequence[0] + "\n" + END)
    with tempfile.TemporaryDirectory(prefix="lite-ai-remote-tail-") as temp:
        for enabled in (0, 1):
            c = Path(temp) / f"ai-remote-tail-{enabled}.c"
            exe = c.with_suffix(".exe")
            c.write_text(body, encoding="utf-8")
            subprocess.run(cc + ["-std=c99", "-O2", f"-DDEMO_AI_AUDIO_AEC_ENABLE={enabled}",
                                 str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: production AI remote-end tail regression (host boundaries only)")


if __name__ == "__main__":
    main()
