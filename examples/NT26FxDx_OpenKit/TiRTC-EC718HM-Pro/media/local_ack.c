#include "local_ack.h"
#include <stddef.h>
#include <string.h>

#if !defined(TIRTC_LOCAL_ACK_ENABLE) || TIRTC_LOCAL_ACK_ENABLE != 1
#error "Compile local_ack.c only for an explicitly enabled local ACK product"
#endif

#include "local_ack_pcm.inc"

#define ACK_AHEAD_MS 80U
#define ACK_DRAIN_MS 160U
#define ACK_MAX_BLOCKS 4U
#define ACK_SAMPLES(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
typedef char ack_female_frames[(ACK_SAMPLES(s_ack_female) % 320U == 0U) ? 1 : -1];
typedef char ack_male_frames[(ACK_SAMPLES(s_ack_male) % 320U == 0U) ? 1 : -1];

static bool same_lease(const tirtc_local_ack_t *state,
                       const demo_ai_audio_lease_t *lease, uint32_t generation)
{
    return state && lease && state->active && state->pcm && generation &&
        state->session_generation == generation && lease->owner == DEMO_AI_AUDIO_OWNER_AI &&
        lease->owner == state->lease.owner && lease->generation == state->lease.generation &&
        demo_ai_audio_lease_is_valid(lease);
}

int tirtc_local_ack_begin(tirtc_local_ack_t *state,
                         const demo_ai_audio_lease_t *lease,
                         uint32_t generation, unsigned voice,
                         uint32_t now, uint32_t existing_until)
{
    if (!state || !lease || !generation || voice > 1U ||
        lease->owner != DEMO_AI_AUDIO_OWNER_AI || !demo_ai_audio_lease_is_valid(lease))
        return TIRTC_LOCAL_ACK_INVALID;
    memset(state, 0, sizeof(*state));
    state->lease = *lease;
    state->session_generation = generation;
    state->pcm = voice ? s_ack_male : s_ack_female;
    state->samples = voice ? ACK_SAMPLES(s_ack_male) : ACK_SAMPLES(s_ack_female);
    state->started_ms = now;
    state->queued_until_ms = (int32_t)(existing_until - now) > 0 ? existing_until : now;
    state->active = state->first = true;
    return 0;
}

int tirtc_local_ack_step(tirtc_local_ack_t *state,
                        const demo_ai_audio_lease_t *lease,
                        uint32_t generation, uint32_t now, bool speaker,
                        bool (*guard)(void *), void *user)
{
    unsigned blocks;
    if (!same_lease(state, lease, generation)) return TIRTC_LOCAL_ACK_INVALID;
    if ((uint32_t)(now - state->started_ms) >= TIRTC_LOCAL_ACK_TIMEOUT_MS)
        return TIRTC_LOCAL_ACK_TIMEOUT;
    if (!speaker) { state->suppressed = true; state->offset = state->samples; }
    for (blocks = 0; blocks < ACK_MAX_BLOCKS && state->offset < state->samples; ++blocks) {
        uint32_t extra = 0;
        int result;
        if ((int32_t)(state->queued_until_ms - now) >= (int32_t)ACK_AHEAD_MS) break;
        if (!guard || !guard(user)) return TIRTC_LOCAL_ACK_PENDING;
        if (!same_lease(state, lease, generation)) return TIRTC_LOCAL_ACK_INVALID;
        result = state->first ?
            demo_ai_audio_play_warm(lease, state->pcm + state->offset, 320U, &extra) :
            demo_ai_audio_play(lease, state->pcm + state->offset, 320U);
        if (result == DEMO_AI_AUDIO_ERR_PLAY_BUSY || result == DEMO_AI_AUDIO_ERR_BUSY)
            return TIRTC_LOCAL_ACK_PENDING;
        if (result) return result;
        state->first = false;
        state->offset += 320U;
        ++state->submitted_frames;
        if ((int32_t)(state->queued_until_ms - now) < 0) state->queued_until_ms = now;
        state->queued_until_ms += 20U + extra;
    }
    if (state->offset == state->samples &&
        (!state->submitted_frames || (int32_t)(now - state->queued_until_ms) >= (int32_t)ACK_DRAIN_MS) &&
        (int32_t)(now - state->queued_until_ms) >= 0 && demo_ai_audio_play_done(lease))
        return TIRTC_LOCAL_ACK_DONE;
    return TIRTC_LOCAL_ACK_PENDING;
}

void tirtc_local_ack_cancel(tirtc_local_ack_t *state)
{
    if (state) { state->active = false; state->pcm = NULL; }
}
