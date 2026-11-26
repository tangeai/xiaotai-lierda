/* XiaoTai AI conversation facade. No SDK/device I/O on the UI thread. */
#ifndef TIRTC_AI_H
#define TIRTC_AI_H
#include "tirtc_ui.h"
int tirtc_ai_start_service(void);
/* RAM-only admission hint; includes queued starts and cleanup reservations. */
bool tirtc_ai_has_session(void);
/* HOME/CLOCK may prepare a revocable, audio-free idle connection. Page/stop
 * actions also invalidate idle attempts; START reuses one without a second
 * WHIP. Other registered runtime features disable this optional optimization. */
int tirtc_ai_action(const tirtc_ui_action_t *action);
/* Desired levels 0..10 (0=mute, >10 clamps); safe before service start and
 * from the UI thread. No I/O or persistence here: the media worker commits
 * the latest complete configuration before preparing/processing audio. */
void tirtc_ai_set_audio_config(uint8_t volume, uint8_t mic_gain,
                               bool speaker_enabled, bool mic_enabled);
#if TIRTC_LOCAL_ACK_ENABLE
/* Local entry cue, not cloud AI voice. 0=female, 1=male. RAM only, no I/O,
 * safe before service startup; the worker freezes the next cue's selection.
 * Invalid values return -1 without changing the current selection. */
int tirtc_ai_set_ack_voice(int32_t voice);
#endif
void tirtc_ai_publish(void);
#endif
