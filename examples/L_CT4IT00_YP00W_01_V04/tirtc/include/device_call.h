/*
 * Copyright (c) 2026 探鸽智能
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: MIT AND Apache-2.0
 *
 * Device-to-device voice-call facade for the NT26F6D0 demo.
 */

#ifndef DEMO_DEV_CHAT_H
#define DEMO_DEV_CHAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    DEMO_DEV_CHAT_OFFLINE = 0,
    DEMO_DEV_CHAT_SYNCING,
    DEMO_DEV_CHAT_READY,
    DEMO_DEV_CHAT_RINGING,
    DEMO_DEV_CHAT_DIALING,
    DEMO_DEV_CHAT_CONNECTING,
    DEMO_DEV_CHAT_WAIT_CONFIRM,
    DEMO_DEV_CHAT_IN_CALL,
    DEMO_DEV_CHAT_STOPPING,
    DEMO_DEV_CHAT_ERROR,
} demo_dev_chat_state_e;

typedef struct
{
    demo_dev_chat_state_e state;
    int error;
    uint8_t contact_count;
    uint8_t selected_contact;
    bool selected_online;
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
} demo_dev_chat_snapshot_t;

/* Dedicated owner of device-call signaling, state and G.711 media. */
void demo_dev_chat_task(void *argv);
void demo_dev_chat_mark_unavailable(void);

/* Non-blocking UI facade. */
void demo_dev_chat_enter(void);
void demo_dev_chat_leave(void);
void demo_dev_chat_select_next(void);
/* Returns the reserved non-zero session sequence, or 0 when not accepted. */
uint32_t demo_dev_chat_call_selected(void);
/* Exact raw preferred-name/device-ID lookup for a nonzero call-refresh ticket.
 * Returns 0/1/>1 matches, -2 while its refresh is pending/in progress, or
 * -1 for an invalid/superseded ticket, failed refresh or unavailable service.
 * This never starts I/O; completed failures require a new request/ticket.
 * Outputs are populated only for a unique match. */
int demo_dev_chat_find_contact(const char *target, char *device_id,
                                size_t capacity, bool *online,
                                uint32_t refresh_ticket);
/* UI owner must close incoming/LIVE admission and wait until all audio
 * owners are idle before queuing this known online device.
 * Returns the reserved sequence, or zero without starting a call. */
uint32_t demo_dev_chat_call_device(const char *device_id);
bool demo_dev_chat_answer(uint32_t incoming_generation);
void demo_dev_chat_hangup(void);
void demo_dev_chat_refresh_contacts(void);
/* Queue a refresh that starts after this request, even if older I/O is active.
 * Returns a nonzero ticket, or 0 while unavailable/busy/in retry backoff.
 * A new ticket supersedes an older cancelled AI request; no I/O runs here. */
uint32_t demo_dev_chat_refresh_contacts_for_call(void);
void demo_dev_chat_get_snapshot(demo_dev_chat_snapshot_t *out);

/* Incoming calls are admitted only while HOME/DEV is the active UI owner. */
void demo_dev_chat_set_home_allowed(bool allowed);
bool demo_dev_chat_blocks_live(void);
bool demo_dev_chat_is_idle(void);

void demo_dev_chat_set_audio_levels(uint8_t speaker_level,
                                    uint8_t mic_level);

#endif /* DEMO_DEV_CHAT_H */
