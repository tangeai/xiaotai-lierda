#ifndef TIRTC_LOCAL_ACK_H
#define TIRTC_LOCAL_ACK_H
#include "audio_device.h"

#define TIRTC_LOCAL_ACK_PENDING 0
#define TIRTC_LOCAL_ACK_DONE 1
#define TIRTC_LOCAL_ACK_INVALID (-1801)
#define TIRTC_LOCAL_ACK_TIMEOUT (-1802)
#define TIRTC_LOCAL_ACK_TIMEOUT_MS 5000U

/* Single AI worker owns this cursor. No task, allocation or audio ownership
 * is created here. Session generation and the existing AI audio lease must
 * both match on every step. Immutable PCM is copied by the normal audio HAL.
 */
typedef struct {
    const int16_t *pcm;
    demo_ai_audio_lease_t lease;
    uint32_t session_generation, samples, offset, started_ms, queued_until_ms;
    uint32_t submitted_frames;
    bool active, first, suppressed;
} tirtc_local_ack_t;

int tirtc_local_ack_begin(tirtc_local_ack_t *state,
                         const demo_ai_audio_lease_t *lease,
                         uint32_t session_generation, unsigned voice,
                         uint32_t now_ms, uint32_t existing_play_until_ms);
/* Worker only, after applying the latest speaker level. At most four 20ms
 * blocks per call and at most 80ms queued ahead. Guard is rechecked before
 * each HAL submission; false yields without consuming PCM. BUSY retains the
 * exact frame. Muting permanently skips this cue's remainder, including after
 * a quick unmute. DONE also requires the estimated tail and HAL play_done;
 * it proves playback submission/drain, never acoustic audibility.
 */
int tirtc_local_ack_step(tirtc_local_ack_t *state,
                        const demo_ai_audio_lease_t *lease,
                        uint32_t session_generation, uint32_t now_ms,
                        bool speaker_enabled, bool (*guard)(void *), void *user);
/* RAM only. Caller performs its existing mute/stop/release cleanup. */
void tirtc_local_ack_cancel(tirtc_local_ack_t *state);
#endif
