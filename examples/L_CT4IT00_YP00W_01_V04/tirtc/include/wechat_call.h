/*
 * Copyright (c) 2026 探鸽智能
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT AND Apache-2.0
 *
 * WeChat voice-call facade for the small NT26F6D0 UI.
 */

#ifndef DEMO_WECHAT_H
#define DEMO_WECHAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    DEMO_WECHAT_OFFLINE = 0,
    DEMO_WECHAT_SYNCING,
    DEMO_WECHAT_READY,
    DEMO_WECHAT_RINGING,
    DEMO_WECHAT_DIALING,
    DEMO_WECHAT_CONNECTING,
    DEMO_WECHAT_WAIT_START,
    DEMO_WECHAT_IN_CALL,
    DEMO_WECHAT_STOPPING,
    DEMO_WECHAT_ERROR,
} demo_wechat_state_e;

typedef struct
{
    demo_wechat_state_e state;
    int error;
    uint8_t contact_count;
    uint8_t selected_contact;
    uint32_t contacts_revision;
    uint32_t incoming_generation;
    uint32_t session_sequence;
    uint32_t completed_sequence;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    char display_name[12];
#ifdef HWDEMO_GROUP_ROOM_EN
    /* Ringing invitation retained while ROOM releases shared media. */
    bool group_pending;
#endif
} demo_wechat_snapshot_t;

/* Dedicated owner of WX signaling, contact cache and G.711 media. */
void demo_wechat_task(void *argv);
void demo_wechat_mark_unavailable(void);

/* UI facade.  Every operation is non-blocking. */
void demo_wechat_enter(void);
void demo_wechat_leave(void);
void demo_wechat_select_next(void);
bool demo_wechat_call_selected(void);
/* Exact raw remark/OpenID lookup for a nonzero call-refresh ticket.
 * Returns 0/1/>1 matches, -2 while its refresh is pending/in progress, or
 * -1 for an invalid/superseded ticket, failed refresh or unavailable service.
 * This never starts I/O; completed failures require a new request/ticket.
 * A unique match copies its OpenID; an ambiguous match never selects a peer. */
int demo_wechat_find_contact(const char *target, char *open_id,
                              size_t capacity, uint32_t refresh_ticket);
/* UI owner must close incoming/LIVE admission and wait until all audio
 * owners are idle before queuing this known OpenID.
 * A fixed contact snapshot is passed to the ordinary worker outgoing flow. */
bool demo_wechat_call_contact(const char *open_id);
bool demo_wechat_answer(uint32_t incoming_generation);
void demo_wechat_hangup(void);
void demo_wechat_refresh_contacts(void);
/* Queue a refresh that starts after this request, even if older I/O is active.
 * Returns a nonzero ticket, or 0 while unavailable/busy/in retry backoff.
 * A new ticket supersedes an older cancelled AI request; no I/O runs here. */
uint32_t demo_wechat_refresh_contacts_for_call(void);
void demo_wechat_get_snapshot(demo_wechat_snapshot_t *out);

/* HOME admission is explicit: incoming WX calls ring only while true. */
void demo_wechat_set_home_allowed(bool allowed);
bool demo_wechat_blocks_live(void);
bool demo_wechat_is_idle(void);

void demo_wechat_set_audio_levels(uint8_t speaker_level, uint8_t mic_level);

#endif /* DEMO_WECHAT_H */
