#!/usr/bin/env python3
"""Compile production Lite AI downlink ownership and buffering regressions.

The actual conditional pool capacity, slot functions, receive callback and
decode/submission loop come from ai_chat.c. Opus decoding and the audio/RTOS
boundaries are deterministic fakes. This tests packet preservation and drop
diagnostics; it does not claim real SDK DMA or acoustic playback completion.
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
#define DEMO_AI_OPUS_MAX_PACKET 1275U
#define TIRTC_AUDIO_OPUS 1U
#define TIRTC_AUDIOSAMPLE_16K16B1C 1U
#define LIOT_NO_WAIT 0U
#define LIOT_OSI_SUCCESS 0
#define DEMO_AI_CHAT_SPEAKING 1
typedef void *tirtc_conn_t;
typedef struct { uint32_t stream_id,length,media,flags,ts; } TIRTCFRAMEINFO;
typedef struct {
    uint16_t length;
    uint8_t media,flags;
    uint32_t ts;
    uint8_t payload[DEMO_AI_OPUS_MAX_PACKET];
} ai_audio_message_t;
static ai_audio_message_t s_audio_pool[AI_AUDIO_QUEUE_DEPTH];
static bool s_audio_pool_used[AI_AUDIO_QUEUE_DEPTH];
static bool s_command_pool_used[AI_COMMAND_QUEUE_DEPTH];
static uint32_t s_audio_pool_in_use,s_audio_pool_peak;
static uint32_t s_audio_rx_received,s_audio_rx_pool_drop,s_audio_rx_queue_drop;
static uint32_t s_audio_rx_format_drop,s_audio_rx_decode_drop;
static uint32_t s_audio_last_rx_ts,s_audio_last_decoded_ts,s_audio_submitted_samples;
static uint32_t s_audio_backpressure_count,s_audio_backpressure_logged_ms;
static tirtc_conn_t s_conn=(void *)1;
static void *s_audio_queue=(void *)2;
static uint32_t s_callback_producers,s_rx_dropped,s_last_audio_ms;
static uint32_t s_play_pending_samples,s_rx_frames;
static bool s_server_speaking,s_first_play_reported;
static int s_audio_lease;
static int16_t s_decode_pcm[AI_DECODE_MAX_SAMPLES],s_play_pcm[AI_PLAY_BATCH_SAMPLES];
static uint8_t queue_slots[AI_AUDIO_QUEUE_DEPTH];
static int16_t output[320U*512U];
static uint32_t now_ms,queued,qread,qwrite,capacity,buffered,submitted;
static uint32_t play_calls,busy_calls,decode_calls,signals,backpressure_logs;
static uint32_t critical_depth,cases;
static bool force_queue_failure,inject_audio_after_empty;
static void packet(unsigned id);
static bool active=true,s_remote_ended=true,s_play_prebuffering;
static uint32_t s_prebuffer_start_ms,s_prebuffer_starts,s_prebuffer_ready,s_prebuffer_timeouts;
static uint32_t s_prebuffer_tail_flushes,s_prebuffer_wait_ms,s_prebuffer_max_ms;
static uint32_t done_at,decoded_length[65536];
static bool cancel_decode;
static bool ai_request_is_active(void){return active;}
static bool demo_ai_audio_play_done(void *lease){
    assert(lease==&s_audio_lease);
    return !buffered && (int32_t)(now_ms-done_at)>=0;
}
static void liot_rtos_enter_critical(void){ ++critical_depth; }
static void liot_rtos_exit_critical(void){ assert(critical_depth); --critical_depth; }
static void liot_trace(const char *format, ...){
    if(strstr(format,"playback backpressure")!=NULL) ++backpressure_logs;
}
static void ai_signal(void){ ++signals; }
static void ai_set_state(int state,int error){ assert(state==DEMO_AI_CHAT_SPEAKING && error==0); }
static uint32_t liot_rtos_get_running_time(void){ return now_ms; }
static uint32_t ai_request_elapsed_ms(void){ return now_ms; }
static int liot_rtos_queue_release(void *q,unsigned n,const void *p,unsigned timeout){
    assert(q==s_audio_queue && q!=NULL && n==sizeof(uint8_t) && timeout==LIOT_NO_WAIT);
    if(force_queue_failure || queued>=AI_AUDIO_QUEUE_DEPTH) return -1;
    queue_slots[qwrite]=*(const uint8_t *)p;
    qwrite=(qwrite+1U)%AI_AUDIO_QUEUE_DEPTH;
    ++queued;
    return LIOT_OSI_SUCCESS;
}
static int liot_rtos_queue_wait(void *q,void *p,unsigned n,unsigned timeout){
    assert(q==s_audio_queue && q!=NULL && n==sizeof(uint8_t) && timeout==LIOT_NO_WAIT);
    if(!queued){
        /* The empty result is already decided. A complete SDK callback can
         * preempt here before discard observes that result and exits. */
        if(inject_audio_after_empty){
            inject_audio_after_empty=false;
            packet(101U);
        }
        return -1;
    }
    *(uint8_t *)p=queue_slots[qread];
    qread=(qread+1U)%AI_AUDIO_QUEUE_DEPTH;
    --queued;
    return LIOT_OSI_SUCCESS;
}
static int demo_ai_opus_decode(const uint8_t *p,unsigned n,int16_t *pcm,unsigned cap){
    unsigned i,id;
    assert(n==2U && cap>=320U);
    ++decode_calls;
    id=p[0]+256U*p[1];
    if(id==65535U) return -1;
    if(id==65534U) return 0;
    if(cancel_decode) active=false;
    unsigned samples=decoded_length[id] ? decoded_length[id] : 320U;
    assert(samples<=cap);
    for(i=0;i<samples;i++) pcm[i]=(int16_t)id;
    return (int)samples;
}
static int demo_ai_audio_play(void *lease,const int16_t *pcm,unsigned count){
    assert(lease==&s_audio_lease && count>0U && critical_depth==0U);
    ++play_calls;
    if(buffered+count>capacity){ ++busy_calls; return -42; }
    assert(submitted+count<=sizeof(output)/sizeof(output[0]));
    memcpy(output+submitted,pcm,count*sizeof(*pcm));
    submitted+=count;
    buffered+=count;
    done_at=now_ms+160U;
    return 0;
}
'''

SUFFIX = r'''
static void reset(void){
    assert(critical_depth==0U);
    /* Original ownership/drop cases use the tail path without startup delay.
     * The dedicated scenarios below enable the real startup/rebuffer gates. */
    active=true;s_remote_ended=true;s_play_prebuffering=false;cancel_decode=false;
    s_prebuffer_start_ms=s_prebuffer_starts=s_prebuffer_ready=s_prebuffer_timeouts=0;
    s_prebuffer_tail_flushes=s_prebuffer_wait_ms=s_prebuffer_max_ms=done_at=0;
    memset(decoded_length,0,sizeof(decoded_length));
    memset(s_audio_pool_used,0,sizeof(s_audio_pool_used));
    memset(s_command_pool_used,0,sizeof(s_command_pool_used));
    memset(s_audio_pool,0,sizeof(s_audio_pool));
    memset(s_decode_pcm,0,sizeof(s_decode_pcm));
    memset(s_play_pcm,0,sizeof(s_play_pcm));
    memset(output,0,sizeof(output));
    s_conn=(void *)1; s_audio_queue=(void *)2;
    s_audio_pool_in_use=s_audio_pool_peak=s_audio_rx_received=0U;
    s_audio_rx_pool_drop=s_audio_rx_queue_drop=s_audio_rx_format_drop=s_audio_rx_decode_drop=0U;
    s_audio_last_rx_ts=s_audio_last_decoded_ts=s_audio_submitted_samples=0U;
    s_audio_backpressure_count=s_audio_backpressure_logged_ms=0U;
    s_callback_producers=s_rx_dropped=s_last_audio_ms=s_play_pending_samples=s_rx_frames=0U;
    s_server_speaking=s_first_play_reported=false;
    now_ms=1000U; queued=qread=qwrite=buffered=submitted=0U;
    capacity=4096U;
    play_calls=busy_calls=decode_calls=signals=backpressure_logs=0U;
    force_queue_failure=inject_audio_after_empty=false;
}
static void packet_type(unsigned id,unsigned ts,unsigned media,unsigned flags){
    uint8_t bytes[2]={(uint8_t)id,(uint8_t)(id>>8)};
    TIRTCFRAMEINFO frame={AI_STREAM_ID,2U,media,flags,ts};
    ai_on_audio(s_conn,&frame,bytes);
    /* The SDK callback buffer may be reused as soon as the callback returns. */
    memset(bytes,0xEE,sizeof(bytes));
    assert(s_callback_producers==0U && critical_depth==0U);
}
static void packet(unsigned id){
    packet_type(id,(id-1U)*20U,TIRTC_AUDIO_OPUS,TIRTC_AUDIOSAMPLE_16K16B1C);
}
static void consume_tick(unsigned tick){
    /* The audio boundary consumes 640 samples per 40ms, with 20ms AI turns. */
    if(tick%2U) buffered=buffered>=640U ? buffered-640U : 0U;
    now_ms+=20U;
}
static void drain(void){
    unsigned tick;
    for(tick=0U;tick<10000U && (queued || s_play_pending_samples || buffered);tick++){
        (void)ai_play_downlink();
        consume_tick(tick);
    }
    assert(tick<10000U && !queued && !s_play_pending_samples && !buffered);
    assert(s_audio_pool_in_use==0U && s_callback_producers==0U);
    assert(s_audio_submitted_samples==submitted && critical_depth==0U);
}
static void expect_frames(unsigned first,unsigned count){
    unsigned frame,i;
    assert(submitted==count*320U);
    for(frame=0U;frame<count;frame++)
        for(i=0U;i<320U;i++) assert(output[frame*320U+i]==(int16_t)(first+frame));
}
static void test_forty_packet_burst(void){
    unsigned id,keep=AI_AUDIO_QUEUE_DEPTH<40U ? AI_AUDIO_QUEUE_DEPTH : 40U;
    reset();
    for(id=1U;id<=40U;id++) packet(id);
    assert(queued==keep && s_audio_pool_in_use==keep && s_audio_pool_peak==keep);
    assert(s_audio_rx_received==40U && s_audio_rx_pool_drop==40U-keep);
    assert(s_rx_dropped==40U-keep && s_audio_rx_queue_drop==0U);
    drain(); expect_frames(1U,keep);
    assert(s_rx_frames==keep && s_audio_last_decoded_ts==(keep-1U)*20U);
    ++cases;
}
static void test_twice_realtime(void){
    unsigned tick,id=1U,i,played;
    reset();
    for(tick=0U;tick<150U;tick++){
        if(tick<50U){ packet(id++); packet(id++); }
        (void)ai_play_downlink();
        consume_tick(tick);
    }
    drain(); played=submitted/320U;
    assert(id==101U && s_audio_rx_received==100U && played+s_rx_dropped==100U);
    assert(busy_calls>0U && s_audio_backpressure_count==busy_calls);
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(s_rx_dropped==0U && s_audio_rx_pool_drop==0U);
    expect_frames(1U,100U);
#else
    /* This is the unchanged 16-slot receive pool under the same deliberately
     * small playback fake; normal non-AEC SDK buffering itself is not modeled. */
    assert(s_audio_rx_pool_drop>0U && s_rx_dropped==s_audio_rx_pool_drop);
    for(i=1U;i<played;i++) assert(output[i*320U]>output[(i-1U)*320U]);
    for(i=0U;i<submitted;i++) assert(output[i]==output[(i/320U)*320U]);
#endif
    (void)i; ++cases;
}
static void test_capacity_and_every_slot(void){
    unsigned id,i;
    reset();
    for(id=1U;id<=AI_AUDIO_QUEUE_DEPTH;id++) packet(id);
    assert(queued==AI_AUDIO_QUEUE_DEPTH && s_audio_pool_in_use==AI_AUDIO_QUEUE_DEPTH);
    for(i=0U;i<AI_AUDIO_QUEUE_DEPTH;i++){
        assert(s_audio_pool_used[i] && queue_slots[i]==(uint8_t)i);
        assert(s_audio_pool[i].payload[0]+256U*s_audio_pool[i].payload[1]==i+1U);
    }
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(AI_AUDIO_QUEUE_DEPTH==256U && queue_slots[255]==255U);
#else
    assert(AI_AUDIO_QUEUE_DEPTH==16U);
#endif
    for(id=AI_AUDIO_QUEUE_DEPTH+1U;id<=AI_AUDIO_QUEUE_DEPTH+3U;id++) packet(id);
    assert(queued==AI_AUDIO_QUEUE_DEPTH && s_audio_rx_pool_drop==3U && s_rx_dropped==3U);
    assert(s_audio_rx_received==AI_AUDIO_QUEUE_DEPTH+3U);
    assert(s_audio_last_rx_ts==(AI_AUDIO_QUEUE_DEPTH+2U)*20U);
    drain(); expect_frames(1U,AI_AUDIO_QUEUE_DEPTH);
    for(i=0U;i<AI_AUDIO_QUEUE_DEPTH;i++) assert(!s_audio_pool_used[i]);
    /* Releasing already-consumed slots must not underflow in-use accounting. */
    ai_audio_slot_release(0U); ai_audio_slot_release((uint8_t)(AI_AUDIO_QUEUE_DEPTH-1U));
    assert(s_audio_pool_in_use==0U);
    packet(1U); assert(queue_slots[qread]==0U && s_audio_pool_in_use==1U);
    (void)ai_play_downlink(); assert(s_audio_pool_in_use==0U);
    ++cases;
}
static void test_command_pool_isolation(void){
    unsigned i;
    reset();
    for(i=0U;i<AI_COMMAND_QUEUE_DEPTH;i++){
        assert(ai_command_slot_acquire(s_conn)==(int)i);
        ai_callback_producer_done();
    }
    assert(ai_command_slot_acquire(s_conn)<0 && s_audio_pool_in_use==0U);
    for(i=1U;i<=AI_AUDIO_QUEUE_DEPTH;i++) packet(i);
    assert(s_audio_pool_in_use==AI_AUDIO_QUEUE_DEPTH && s_rx_dropped==0U);
    drain(); expect_frames(1U,AI_AUDIO_QUEUE_DEPTH);
    for(i=0U;i<AI_COMMAND_QUEUE_DEPTH;i++){
        assert(s_command_pool_used[i]); ai_command_slot_release((uint8_t)i);
    }
    assert(s_audio_pool_in_use==0U && s_callback_producers==0U);
    assert(ai_audio_slot_acquire((void *)99)<0);
    assert(ai_command_slot_acquire((void *)99)<0);
    ++cases;
}
static void test_backpressure_and_log_budget(void){
    unsigned i,pending;
    int16_t copy[AI_PLAY_BATCH_SAMPLES];
    reset(); capacity=0U;
    packet(1U); packet(2U); packet(3U);
    assert(ai_play_downlink()==0);
    pending=s_play_pending_samples;
    assert(pending==960U && queued==0U && s_audio_pool_in_use==0U);
    memcpy(copy,s_play_pcm,pending*sizeof(*copy));
    assert(backpressure_logs==1U);
    for(i=1U;i<50U;i++){
        now_ms=1000U+i*20U;
        assert(ai_play_downlink()==0 && s_play_pending_samples==pending);
        assert(memcmp(copy,s_play_pcm,pending*sizeof(*copy))==0);
    }
    assert(busy_calls==50U && s_audio_backpressure_count==50U);
    assert(backpressure_logs==1U && submitted==0U && s_audio_submitted_samples==0U);
    now_ms=2000U; assert(ai_play_downlink()==0);
    assert(backpressure_logs==2U && s_audio_backpressure_count==51U);
    packet(4U); packet(5U);
    capacity=4096U; drain(); expect_frames(1U,5U);
    assert(s_rx_dropped==0U && decode_calls==5U && s_audio_rx_received==5U);
    ++cases;
}
static void test_queue_failure_release(void){
    reset(); force_queue_failure=true; packet(1U);
    assert(s_audio_rx_received==1U && s_audio_rx_queue_drop==1U && s_rx_dropped==1U);
    assert(s_audio_rx_pool_drop==0U && s_audio_pool_in_use==0U && !s_audio_pool_used[0]);
    force_queue_failure=false; s_audio_queue=NULL; packet(2U);
    assert(s_audio_rx_queue_drop==2U && s_rx_dropped==2U && s_audio_pool_in_use==0U);
    s_audio_queue=(void *)2; packet(3U); drain(); expect_frames(3U,1U);
    assert(s_audio_pool_peak==1U && s_audio_rx_received==3U);
    ++cases;
}
static void test_format_and_decode_failures(void){
    reset();
    packet_type(1U,0U,TIRTC_AUDIO_OPUS+1U,TIRTC_AUDIOSAMPLE_16K16B1C);
    packet_type(2U,20U,TIRTC_AUDIO_OPUS,TIRTC_AUDIOSAMPLE_16K16B1C+1U);
    packet(65535U); packet(65534U); packet(4U);
    drain(); expect_frames(4U,1U);
    assert(s_audio_rx_received==5U && s_audio_rx_format_drop==2U);
    assert(s_audio_rx_decode_drop==2U && s_rx_dropped==4U && decode_calls==3U);
    assert(s_audio_rx_pool_drop==0U && s_audio_rx_queue_drop==0U);
    assert(s_audio_last_decoded_ts==60U && s_rx_frames==1U);
    ++cases;
}
static void test_callback_rejection(void){
    uint8_t bytes[2]={1,0};
    TIRTCFRAMEINFO frame={AI_STREAM_ID,2U,TIRTC_AUDIO_OPUS,TIRTC_AUDIOSAMPLE_16K16B1C,0U};
    reset();
    ai_on_audio((void *)99,&frame,bytes);
    ai_on_audio(s_conn,NULL,bytes); ai_on_audio(s_conn,&frame,NULL);
    frame.stream_id=AI_STREAM_ID+1U; ai_on_audio(s_conn,&frame,bytes);
    frame.stream_id=AI_STREAM_ID; frame.length=0U; ai_on_audio(s_conn,&frame,bytes);
    frame.length=DEMO_AI_OPUS_MAX_PACKET+1U; ai_on_audio(s_conn,&frame,bytes);
    assert(queued==0U && s_audio_rx_received==0U && s_rx_dropped==0U);
    assert(s_audio_pool_in_use==0U && s_callback_producers==0U && signals==0U);
    ++cases;
}
static void test_invalid_slot_off_only(void){
#if !DEMO_AI_AUDIO_AEC_ENABLE
    uint8_t invalid=255U;
    reset();
    assert(liot_rtos_queue_release(s_audio_queue,sizeof(invalid),&invalid,LIOT_NO_WAIT)==0);
    assert(ai_play_downlink()==0 && s_rx_dropped==1U && decode_calls==0U);
    ai_audio_slot_release(invalid);
    assert(s_audio_pool_in_use==0U && s_callback_producers==0U);
    ++cases;
#endif
}
static void test_discard_callback_race(void){
    unsigned id,live,next;
    reset();
    /* Drain every real slot first, including index 255 in the AEC build. */
    for(id=1U;id<=AI_AUDIO_QUEUE_DEPTH;id++) packet(id);
    inject_audio_after_empty=true;
    ai_discard_audio_queue();
    assert(!inject_audio_after_empty && s_callback_producers==0U);
    assert(queued==1U && s_audio_pool_in_use==1U);
    live=queue_slots[qread];
    assert(s_audio_pool_used[live]);
    assert(s_audio_pool[live].payload[0]==101U && s_audio_pool[live].payload[1]==0U);
    for(id=0U;id<AI_AUDIO_QUEUE_DEPTH;id++)
        assert(s_audio_pool_used[id]==(id==live));
    packet(102U);
    next=queue_slots[(qread+1U)%AI_AUDIO_QUEUE_DEPTH];
    assert(queued==2U && s_audio_pool_in_use==2U && next!=live);
    assert(s_audio_pool[live].payload[0]==101U && s_audio_pool[next].payload[0]==102U);
    drain(); expect_frames(101U,2U);
    assert(s_audio_pool_in_use==0U && s_audio_pool_peak==AI_AUDIO_QUEUE_DEPTH);
    for(id=0U;id<AI_AUDIO_QUEUE_DEPTH;id++) assert(!s_audio_pool_used[id]);
    assert(s_rx_dropped==0U);
    ++cases;
}
static int old_main(void){
    test_forty_packet_burst();
    test_twice_realtime();
    test_capacity_and_every_slot();
    test_command_pool_isolation();
    test_backpressure_and_log_budget();
    test_queue_failure_release();
    test_format_and_decode_failures();
    test_callback_rejection();
    test_invalid_slot_off_only();
    test_discard_callback_race();
    printf("PASS AI downlink AEC=%d depth=%u: %u scenarios, burst/order/capacity/all slots, isolated pools, retained busy PCM, classified drops/throttled logs and discard-callback race\n",
           DEMO_AI_AUDIO_AEC_ENABLE,(unsigned)AI_AUDIO_QUEUE_DEPTH,cases);
    return 0;
}
'''


PREBUFFER_SCENARIOS = r'''
#if DEMO_AI_AUDIO_AEC_ENABLE
static bool s_capture_timeout_active;
static void fresh(void){reset();s_remote_ended=false;}
static void reenter(void){
    /* ACTUAL_REENTRY_RESET */
    s_first_play_reported=false;
    active=true;s_remote_ended=false;
    buffered=0U;done_at=0U;
}
static void interrupt_now(void){
    /* ACTUAL_INTERRUPT_RESET */
    buffered=0U;done_at=0U;
}
static void test_regular_stream(void){
    fresh();
    for(unsigned i=1;i<=4;i++){
        packet(i);
        assert(ai_play_downlink()==(i==4 ? 1:0));
        if(i<4){assert(play_calls==0 && s_play_prebuffering);now_ms+=20;}
    }
    assert(submitted==1280 && s_prebuffer_ready==1 && s_prebuffer_wait_ms==60);
    packet(5);now_ms+=20;assert(ai_play_downlink()==1);
    assert(s_prebuffer_starts==1 && submitted==1600 && s_rx_dropped==0);
    expect_frames(1,5);cases++;
}
static void test_fast_first_with_warmup(void){
    fresh();buffered=1920;done_at=now_ms+160;
    for(unsigned i=1;i<=4;i++) packet(i);
    assert(!demo_ai_audio_play_done(&s_audio_lease));
    assert(ai_play_downlink()==1 && s_prebuffer_starts==1 && s_prebuffer_ready==1);
    assert(submitted==1280 && s_prebuffer_wait_ms==0);cases++;
    fresh();buffered=1920;done_at=now_ms+160;packet(1);
    assert(ai_play_downlink()==0 && s_play_prebuffering && play_calls==0);
    assert(s_play_pending_samples==320);cases++;
}
static void test_lengths_and_short_timeout(void){
    unsigned lengths[]={320,640,960,1920};
    for(unsigned i=0;i<4;i++){
        fresh();decoded_length[1]=lengths[i];packet(1);
        assert(ai_play_downlink()==(lengths[i]>=1280 ? 1:0));
        if(lengths[i]<1280){
            now_ms+=119;assert(ai_play_downlink()==0 && play_calls==0);
            now_ms++;assert(ai_play_downlink()==1);
            assert(s_prebuffer_timeouts==1 && s_prebuffer_wait_ms==120);
        }else assert(s_prebuffer_ready==1 && s_prebuffer_wait_ms==0);
        assert(submitted==lengths[i] && s_rx_dropped==0);cases++;
    }
    fresh();decoded_length[1]=960;packet(1);assert(ai_play_downlink()==0);
    now_ms+=20;packet(2);assert(ai_play_downlink()==1);
    assert(submitted==1280 && s_prebuffer_ready==1);cases++;
    fresh();decoded_length[1]=960;packet(1);assert(ai_play_downlink()==0);
    decoded_length[2]=1920;packet(2);assert(ai_play_downlink()==1);
    assert(submitted==2880 && s_prebuffer_ready==1);cases++;
}
static void test_timeout_clocks(void){
    for(unsigned i=0;i<2;i++){
        fresh();now_ms=i ? UINT32_MAX-60U:0;packet(1);
        assert(ai_play_downlink()==0);now_ms+=119;
        assert(ai_play_downlink()==0);now_ms++;assert(ai_play_downlink()==1);
        assert(s_prebuffer_wait_ms==120);cases++;
    }
    fresh();packet(1);assert(ai_play_downlink()==0);
    now_ms+=100;assert(ai_play_downlink()==0);
    /* Existing capture may occupy the owner for 120 ms. No new blocking wait. */
    now_ms+=120;assert(ai_play_downlink()==1);
    assert(s_prebuffer_max_ms==220 && s_prebuffer_timeouts==1);cases++;
}
static void test_backpressure_gate_once(void){
    fresh();capacity=0;
    for(unsigned i=1;i<=4;i++)packet(i);
    assert(ai_play_downlink()==0 && s_prebuffer_ready==1 && !s_play_prebuffering);
    int16_t copy[1280];memcpy(copy,s_play_pcm,sizeof(copy));
    for(unsigned i=0;i<4;i++){now_ms+=20;assert(ai_play_downlink()==0);}
    assert(s_prebuffer_starts==1 && s_prebuffer_ready==1 && s_prebuffer_timeouts==0);
    assert(!memcmp(copy,s_play_pcm,sizeof(copy)) && s_play_pending_samples==1280);
    capacity=4096;assert(ai_play_downlink()==1);expect_frames(1,4);
    assert(s_rx_dropped==0);cases++;
}
static void test_rebuffer_and_remote_tail(void){
    fresh();for(unsigned i=1;i<=4;i++)packet(i);assert(ai_play_downlink()==1);
    buffered=0;now_ms+=80;packet(5);assert(ai_play_downlink()==1);
    assert(s_prebuffer_starts==1); /* Conservative tail not fully drained yet. */
    buffered=0;now_ms+=160;packet(6);assert(ai_play_downlink()==0);
    assert(s_prebuffer_starts==2 && s_play_prebuffering);
    s_remote_ended=true;assert(ai_play_downlink()==1);
    assert(s_prebuffer_tail_flushes==1 && s_prebuffer_timeouts==0);
    expect_frames(1,6);cases++;
    fresh();s_remote_ended=true;packet(1);assert(ai_play_downlink()==1);
    assert(s_prebuffer_starts==0);expect_frames(1,1);cases++;
}
static void test_cancel_interrupt_reentry(void){
    fresh();packet(1);assert(ai_play_downlink()==0);
    active=false;now_ms+=200;assert(ai_play_downlink()==0 && submitted==0);
    reenter();assert(!s_play_prebuffering && s_play_pending_samples==0 && s_prebuffer_starts==0);
    packet(2);assert(ai_play_downlink()==0);now_ms+=120;assert(ai_play_downlink()==1);
    assert(submitted==320 && output[0]==2);cases++;
    fresh();packet(1);assert(ai_play_downlink()==0);packet(2);
    interrupt_now();assert(queued==0 && !s_play_prebuffering && !s_play_pending_samples);
    packet(3);assert(ai_play_downlink()==0);now_ms+=120;assert(ai_play_downlink()==1);
    assert(submitted==320 && output[0]==3 && s_prebuffer_starts==2);cases++;
    fresh();packet(1);cancel_decode=true;
    assert(ai_play_downlink()==0 && submitted==0 && !active);cases++;
    fresh();active=false;packet(1);assert(ai_play_downlink()==0 && decode_calls==0 && queued==1);cases++;
}
static void cleanup_reset(void){
    /* ACTUAL_CLEANUP_RESET */
}
static void test_cleanup_and_normal_batch(void){
    fresh();packet(1);assert(ai_play_downlink()==0 && s_play_prebuffering);
    active=false;cleanup_reset();
    assert(!s_play_prebuffering && !s_play_pending_samples && !s_prebuffer_start_ms);
    reenter();assert(!s_play_prebuffering && !s_prebuffer_starts);cases++;
    fresh();for(unsigned i=1;i<=4;i++)packet(i);assert(ai_play_downlink()==1);
    for(unsigned i=5;i<=12;i++)packet(i);
    assert(ai_play_downlink()==1 && submitted==2240 && queued==5);
    assert(s_prebuffer_starts==1);cases++;
}
static void test_invalid_not_started(void){
    fresh();packet(65535);assert(ai_play_downlink()==0);
    assert(!s_play_prebuffering && s_prebuffer_starts==0 && s_rx_dropped==1);
    now_ms+=1000;packet(1);assert(ai_play_downlink()==0 && s_prebuffer_start_ms==now_ms);cases++;
}
#endif
int main(void){
    old_main();
#if DEMO_AI_AUDIO_AEC_ENABLE
    unsigned before=cases;
    test_regular_stream();test_fast_first_with_warmup();test_lengths_and_short_timeout();
    test_timeout_clocks();test_backpressure_gate_once();test_rebuffer_and_remote_tail();
    test_cancel_interrupt_reentry();test_invalid_not_started();test_cleanup_and_normal_batch();
    printf("PASS actual AI prebuffer: %u scenarios; samples/time/long frames/retry/fast-warm-start/cancel/interrupt/reentry/tail/clock\n",cases-before);
#endif
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
    # Preserve #if/#else around the real AEC/non-AEC capacities. Taking only
    # the first matching #define would silently give both builds 256 slots.
    definitions = source[source.index("#define AI_CMD_WORD"):
                         source.index("#define AI_ERR_CANCELLED")]
    packet_limit = re.search(r"^#define\s+DEMO_AI_OPUS_MAX_PACKET\s+(\d+U?)",
                             (app / "include/opus_codec.h").read_text(encoding="utf-8"), re.M)
    if packet_limit is None:
        raise AssertionError("Production Opus packet bound missing")
    prefix = PREFIX.replace("#define DEMO_AI_OPUS_MAX_PACKET 1275U",
                            "#define DEMO_AI_OPUS_MAX_PACKET " + packet_limit.group(1))
    names = ("ai_audio_slot_acquire", "ai_audio_slot_release",
             "ai_command_slot_acquire", "ai_command_slot_release",
             "ai_callback_producer_done", "ai_on_audio", "ai_play_downlink",
             "ai_discard_audio_queue")
    # Exercise the actual reset statements, so lifecycle tests cannot silently
    # model a reset that production forgot to perform.
    interrupt_start = source.index('else if (strcmp(method_text, "interrupt")')
    interrupt = source[source.index("            ai_discard_audio_queue();", interrupt_start):
                       source.index("            (void)demo_ai_audio_stop", interrupt_start)]
    run_start = source.index("static int ai_run_session(void)")
    reentry = source[source.index("    s_play_pending_samples = 0U;", run_start):
                     source.index("    ai_discard_command_queue();", run_start)]
    scenarios = PREBUFFER_SCENARIOS.replace("/* ACTUAL_INTERRUPT_RESET */", interrupt)
    scenarios = scenarios.replace("/* ACTUAL_REENTRY_RESET */", reentry)
    cleanup_start = source.index("static void ai_session_cleanup(")
    cleanup = source[source.index("    s_play_pending_samples = 0U;", cleanup_start):
                     source.index("    s_whip_done = false;", cleanup_start)]
    scenarios = scenarios.replace("/* ACTUAL_CLEANUP_RESET */", cleanup)
    body = definitions + prefix + "".join(function(source, name) for name in names) + SUFFIX + scenarios
    with tempfile.TemporaryDirectory(prefix="lite-ai-downlink-buffer-") as temp:
        for enabled in (0, 1):
            c = Path(temp) / f"downlink-{enabled}.c"
            exe = c.with_suffix(".exe")
            c.write_text(body, encoding="utf-8")
            subprocess.run(cc + ["-std=c99", "-O2", f"-DDEMO_AI_AUDIO_AEC_ENABLE={enabled}",
                                 str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
