/* Calls use independent capture and playback; no SDK I/O from callbacks. */
#ifndef TIRTC_CALLS_AUDIO_H
#define TIRTC_CALLS_AUDIO_H
#include "tirtc_runtime.h"
#include "audio_device.h"
#ifndef TIRTC_DEV_OPUS_ENABLE
#define TIRTC_DEV_OPUS_ENABLE 0
#endif
/* Fixed-point Opus encoder/decoder use substantial VLA scratch, as in Lite. */
#if TIRTC_DEV_OPUS_ENABLE
#define CALLS_AUDIO_CONTROL_STACK_BYTES (64U*1024U)
#else
#define CALLS_AUDIO_CONTROL_STACK_BYTES (16U*1024U)
#endif
int calls_audio_start_service(void);
int calls_audio_start(demo_tirtc_owner_e owner, uint32_t generation);
#if TIRTC_F9_AV_FOCUS
/* Only the owning call-control task changes media worker priorities. */
bool calls_audio_set_av_focus(bool enabled);
#endif
#if TIRTC_F9_CALL_AUDIO_QOS
/* The active call's microphone has recently encountered TX backpressure.
 * Safe from the video worker; never performs SDK I/O or waits. */
bool calls_audio_video_should_defer(demo_tirtc_owner_e owner, uint32_t generation);
#endif
void calls_audio_set_config(uint8_t volume, uint8_t mic, bool speaker, bool microphone);
/* Global saved preferences stay separate from a remote session's gates.
 * LIVE starts with speaker allowed, microphone denied and no uplink subscriber.
 * Effective media is the intersection of these gates and global preferences.
 * Bounded metadata only: safe from an SDK callback, no hardware calls. */
int calls_audio_set_session_controls(demo_tirtc_owner_e owner, uint32_t generation,
                                     bool speaker, bool microphone, bool uplink_subscribed);
/* Copy bounded media. LIVE accepts A-law streams 10/14, mono 8/16 kHz;
 * WX retains A-law stream 0 / 8 kHz. With TIRTC_DEV_OPUS_ENABLE, DEV sends
 * Opus stream 10 / 16 kHz and accepts Opus or legacy A-law 8/16 kHz.
 * DEV packet inspection/copy is bounded; decode/resample
 * happens in step() and always feeds the shared 16 kHz local PCM device. */
void calls_audio_receive(demo_tirtc_owner_e owner, uint32_t generation,
                         const TIRTCFRAMEINFO *frame, const void *data);
/* Called by control/playback owner every 5 ms during a call. */
int calls_audio_step(demo_tirtc_owner_e owner, uint32_t generation);
/* First call closes capture admission. BUSY retains both owners until a
 * potentially blocked SDK record returns; never free/reuse them on timeout. */
/* A stale/different owner or generation is a harmless no-op. This also
 * protects LIVE when a local call cancels before obtaining its RTC owner. */
int calls_audio_stop(demo_tirtc_owner_e owner, uint32_t generation);
bool calls_audio_busy(void);
#endif
