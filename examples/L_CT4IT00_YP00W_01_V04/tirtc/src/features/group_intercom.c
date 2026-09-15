/* SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * Independent Room owner. Only this worker touches Room audio/state;
 * blocking HTTP/WHIP submission runs on the bounded service worker.
 */
#include "group_intercom.h"
#include "audio_device.h"
#include "device_binding.h"
#include "formal_mqtt.h"
#include "g711_codec.h"
#include "network_manager.h"
#include "tirtc_runtime.h"
#include "cJSON.h"
#include "liot_dev.h"
#include "liot_log.h"
#include "liot_os.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROOM_COMMAND               0x2200U
#define ROOM_STREAM                1U
#define ROOM_ID_MAX                65U
#define ROOM_WIRE_ID_MAX           256U
#define ROOM_SESSION_MAX           65U
#define ROOM_CONNECT_MAX           1152U
#define ROOM_HTTP_MAX              4096U
#define ROOM_HTTP_TIMEOUT          10000U
#define ROOM_RX_SLOTS              8U
#define ROOM_RX_MAX                640U
#define ROOM_RX_MAX_AGE            160U
#define ROOM_COMMAND_SLOTS         2U
#define ROOM_COMMAND_MAX           4096U
#define ROOM_SYNC_INTERVAL         60000U
#define ROOM_MIC_RETRY_INTERVAL    1000U
#define ROOM_RESOURCE_TIMEOUT      30000U
#define ROOM_CONNECT_TIMEOUT       20000U
#define ROOM_JOIN_TIMEOUT          8000U
#define ROOM_MAX_RETRIES            6U
#define ROOM_UNKNOWN_LEASE_WAIT     60000U
#define ROOM_SEND_HIGH_WATER        4096U
#define ROOM_ERR_PROTOCOL         (-6101)
#define ROOM_ERR_AUDIO            (-6102)
#define ROOM_ERR_OFFLINE          (-6103)
#define ROOM_ERR_LEASE            (-6104)
#define ROOM_ERR_INIT             (-6105)

typedef struct {
    char room_id[ROOM_ID_MAX];
    char room_code[7];
    uint32_t version;
    bool desired;
    bool closed;
} room_assignment_t;

typedef enum {
    ROOM_JOB_ASSIGNMENT = 0, ROOM_JOB_TOKEN, ROOM_JOB_PRESENCE,
    ROOM_JOB_TERMINAL, ROOM_JOB_CONNECT
} room_job_kind_e;

typedef struct {
    room_job_kind_e kind;
    uint32_t intent;
    uint32_t epoch;
    uint32_t sync_sequence;
    uint32_t version;
    uint32_t timeout_ms;
    char room_id[ROOM_ID_MAX];
    char session_id[ROOM_SESSION_MAX];
    char presence[16];
    char peer[ROOM_CONNECT_MAX];
    char token[ROOM_CONNECT_MAX];
} room_job_t;

typedef struct {
    int code;
    uint32_t started_ms;
    room_assignment_t assignment;
    char peer[ROOM_CONNECT_MAX];
    char token[ROOM_CONNECT_MAX];
    uint32_t heartbeat_ms;
    uint32_t lease_ms;
} room_http_result_t;

typedef struct {
    bool used;
    uint32_t epoch;
    uint32_t received_ms;
    uint16_t length;
    uint8_t bytes[ROOM_RX_MAX];
} room_audio_slot_t;

typedef struct {
    bool used;
    uint32_t epoch;
    uint16_t length;
    char bytes[ROOM_COMMAND_MAX + 1U];
} room_command_slot_t;

static liot_sem_t s_wake;
static liot_sem_t s_http_wake;
static bool s_init_attempted;
static volatile bool s_initialized;
/* These fields are shared only in short critical sections. */
static bool s_enabled;
static bool s_foreground;
static bool s_suspend_requested;
static bool s_media_idle = true;
static bool s_accept_audio;
static bool s_accept_commands;
static bool s_mic_gate;
static bool s_mic_dirty;
static bool s_levels_dirty;
static uint8_t s_speaker = DEMO_AI_AUDIO_DEFAULT_SPK_LEVEL;
static uint8_t s_mic_level = DEMO_AI_AUDIO_DEFAULT_MIC_LEVEL;
static uint32_t s_intent;
static uint32_t s_sync_sequence;
static demo_group_call_e s_call;
static uint32_t s_call_ticket;
static uint32_t s_ticket_sequence;
static demo_group_snapshot_t s_snapshot;

/* All business fields below are owned by the Room worker unless annotated. */
static room_assignment_t s_assignment;
static uint32_t s_seen_intent;
static uint32_t s_sync_done;
static uint32_t s_epoch;
static uint32_t s_generation;
static bool s_claimed;
static bool s_joined;
static bool s_stopping;
static bool s_cleanup_sent;
static bool s_stopped_audio;
static bool s_token_requested;
static bool s_manual_error;
static bool s_waiting_token;
static demo_group_state_e s_after_cleanup;
static int s_cleanup_error;
static uint32_t s_deadline;
static uint32_t s_retry_at;
static uint32_t s_sync_at;
static uint32_t s_heartbeat_at;
static uint32_t s_mic_retry_at;
static uint32_t s_lease_deadline;
static uint32_t s_heartbeat_ms;
static uint32_t s_lease_ms;
static uint32_t s_join_id;
static uint32_t s_session_started;
static unsigned int s_failures;
static bool s_conflict_wait;
static uint32_t s_conflict_deadline;
static char s_session[ROOM_SESSION_MAX];
static char s_media_session[129];
static char s_device[DEMO_BIND_DEVICE_ID_MAX];
static char s_wire_room[ROOM_WIRE_ID_MAX];
static demo_ai_audio_lease_t s_audio = DEMO_AI_AUDIO_LEASE_INIT;
/* Opaque handles are compared for callback correlation only, never called. */
static tirtc_conn_t s_wire_connection;
static bool s_connect_pending;
static bool s_connect_event;
static bool s_sdk_submit_busy;
static int s_connect_error;
static int s_callback_error;
static uint32_t s_callback_epoch;
static struct { uint32_t epoch; } s_connect_context;
static uint32_t s_producers;
static room_audio_slot_t s_rx[ROOM_RX_SLOTS];
static uint8_t s_rx_ring[ROOM_RX_SLOTS];
static uint8_t s_rx_read, s_rx_write, s_rx_count;
static uint32_t s_rx_bytes;
static room_command_slot_t s_commands[ROOM_COMMAND_SLOTS];
static uint8_t s_cmd_ring[ROOM_COMMAND_SLOTS];
static uint8_t s_cmd_read, s_cmd_write, s_cmd_count;
static bool s_command_gap;
static uint32_t s_command_oversize;
static __attribute__((aligned(16))) int16_t s_capture[320];
static __attribute__((aligned(16))) int16_t s_play[ROOM_RX_MAX * 2U];
static uint8_t s_alaw[160];
/* One immutable job/result mailbox. Terminal reporting has one separate
 * saved tuple, so old HTTP can finish without retaining media ownership. */
static room_job_t s_http_job;
static room_http_result_t s_http_result;
static char s_http_response[ROOM_HTTP_MAX];
static bool s_http_busy;
static bool s_http_done;
static bool s_terminal_pending;
static room_job_t s_terminal;
static bool s_transport_was_ready;
static bool s_bound_seen;

static void room_wake(void) {
    if (s_wake != NULL) (void)liot_rtos_semaphore_release(s_wake);
}
static uint32_t room_next(uint32_t value) {
    ++value;
    return value != 0U ? value : 1U;
}
static bool room_due(uint32_t deadline) {
    return (int32_t)(liot_rtos_get_running_time() - deadline) >= 0;
}
static void room_zero(void *data, size_t size) {
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (size-- != 0U) *p++ = 0U;
}
static void room_state(demo_group_state_e state, int error) {
    bool changed;
    liot_rtos_enter_critical();
    changed = s_snapshot.state != state || s_snapshot.error != error;
    s_snapshot.state = state;
    s_snapshot.error = error;
    liot_rtos_exit_critical();
    if (changed) liot_trace("[ROOM] state=%u error=%d epoch=%u\r\n",
        (unsigned int)state, error, (unsigned int)s_epoch);
}
static bool room_allowed(void) {
    bool allowed;
    liot_rtos_enter_critical();
    allowed = s_enabled && s_foreground && s_call == DEMO_GROUP_CALL_NONE;
    liot_rtos_exit_critical();
    return allowed;
}
static const char *room_string(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}
static bool room_copy(const cJSON *object, const char *key, char *out,
                      size_t capacity, bool required) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    size_t length;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return !required && item == NULL;
    length = strlen(item->valuestring);
    if (length >= capacity || (required && length == 0U)) return false;
    memcpy(out, item->valuestring, length + 1U);
    return true;
}
static bool room_number(const cJSON *object, const char *key,
                        uint32_t *value, bool nonzero) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    double number;
    if (!cJSON_IsNumber(item)) return false;
    number = item->valuedouble;
    if (!(number >= (nonzero ? 1.0 : 0.0) && number <= 4294967295.0)) return false;
    *value = (uint32_t)number;
    return number == (double)*value;
}
static cJSON *room_parse_json(const char *text, size_t length) {
    size_t i;
    unsigned int depth=0U;
    bool quoted=false, escaped=false;
    if (text == NULL || length == 0U || memchr(text,'\0',length) != NULL) return NULL;
    /* cJSON's default nesting budget is too large for a 12 KiB worker.
     * Bound recursion before entering the parser, respecting JSON strings. */
    for (i=0U;i<length;++i) {
        char c=text[i];
        if (quoted) {
            if (escaped) escaped=false;
            else if (c=='\\') escaped=true;
            else if (c=='"') quoted=false;
        } else if (c=='"') quoted=true;
        else if (c=='{' || c=='[') { if (++depth>16U) return NULL; }
        else if (c=='}' || c==']') { if (depth==0U) return NULL; --depth; }
    }
    if (quoted || depth != 0U) return NULL;
    return cJSON_ParseWithOpts(text,NULL,1);
}
static bool room_format(const cJSON *object) {
    uint32_t rate, channels;
    return cJSON_IsObject(object) && strcmp(room_string(object,"codec"),"g711a") == 0 &&
        room_number(object,"sample_rate",&rate,true) && rate == 8000U &&
        room_number(object,"channels",&channels,true) && channels == 1U;
}
static cJSON *room_audio_descriptor(void) {
    cJSON *object = cJSON_CreateObject();
    if (object == NULL || cJSON_AddStringToObject(object,"codec","g711a") == NULL ||
        cJSON_AddNumberToObject(object,"sample_rate",8000) == NULL ||
        cJSON_AddNumberToObject(object,"channels",1) == NULL) {
        cJSON_Delete(object); return NULL;
    }
    return object;
}
static int room_send_json(cJSON *object) {
    char *json;
    int result = ROOM_ERR_PROTOCOL;
    if (object == NULL) return result;
    json = cJSON_PrintUnformatted(object);
    if (json != NULL) {
        result = demo_tirtc_send_command(DEMO_TIRTC_OWNER_GROUP_ROOM,
            s_generation, ROOM_COMMAND, json, (uint32_t)strlen(json));
        cJSON_free(json);
    }
    cJSON_Delete(object);
    return result;
}
static cJSON *room_rpc(const char *method) {
    cJSON *object = cJSON_CreateObject();
    if (object == NULL || cJSON_AddStringToObject(object,"jsonrpc","2.0") == NULL ||
        cJSON_AddStringToObject(object,"method",method) == NULL) {
        cJSON_Delete(object); return NULL;
    }
    return object;
}
static int room_send_mic(bool on) {
    cJSON *object = room_rpc("set_mic_state");
    cJSON *params = object != NULL ? cJSON_AddObjectToObject(object,"params") : NULL;
    if (params == NULL || cJSON_AddStringToObject(params,"mic_state",on ? "on" : "off") == NULL) {
        cJSON_Delete(object); return ROOM_ERR_PROTOCOL;
    }
    return room_send_json(object);
}
static int room_send_join(void) {
    cJSON *object = room_rpc("join_room");
    cJSON *params, *input, *output;
    if (object == NULL) return ROOM_ERR_PROTOCOL;
    params = cJSON_AddObjectToObject(object,"params");
    input = room_audio_descriptor();
    output = room_audio_descriptor();
    if (params == NULL || input == NULL || output == NULL ||
        cJSON_AddNumberToObject(object,"id",s_join_id) == NULL ||
        cJSON_AddStringToObject(params,"room_id",s_assignment.room_id) == NULL ||
        cJSON_AddStringToObject(params,"device_id",s_device) == NULL) {
        cJSON_Delete(input); cJSON_Delete(output); cJSON_Delete(object);
        return ROOM_ERR_PROTOCOL;
    }
    if (!cJSON_AddItemToObject(params,"input_audio",input)) {
        cJSON_Delete(input); cJSON_Delete(output); cJSON_Delete(object); return ROOM_ERR_PROTOCOL;
    }
    if (!cJSON_AddItemToObject(params,"output_audio",output)) {
        cJSON_Delete(output); cJSON_Delete(object); return ROOM_ERR_PROTOCOL;
    }
    return room_send_json(object);
}

static void room_connect_result(int error, tirtc_conn_t connection, void *user) {
    liot_rtos_enter_critical();
    if (user == &s_connect_context && s_connect_context.epoch == s_epoch) {
        s_connect_pending = false;
        s_connect_error = error != 0 ? error : (connection != NULL ? 0 : ROOM_ERR_PROTOCOL);
        s_connect_event = true;
        if (s_connect_error == 0 && !s_stopping && s_enabled && s_foreground &&
            s_call == DEMO_GROUP_CALL_NONE) s_wire_connection = connection;
    }
    liot_rtos_exit_critical();
    room_wake();
}
static void room_connection_error(tirtc_conn_t connection, int error) {
    liot_rtos_enter_critical();
    if (!s_media_idle && (connection == s_wire_connection || s_connect_pending)) {
        s_callback_error = error != 0 ? error : TIRTC_E_CONN_REMOTECLOSE;
        s_callback_epoch = s_epoch;
        s_mic_gate = false;
        s_accept_audio = false;
    }
    liot_rtos_exit_critical();
    room_wake();
}
static void room_disconnected(tirtc_conn_t connection) {
    room_connection_error(connection, TIRTC_E_CONN_REMOTECLOSE);
}
static int room_subscribe(tirtc_conn_t connection, uint8_t stream) {
    bool accept;
    liot_rtos_enter_critical();
    accept = !s_stopping && !s_media_idle && stream == ROOM_STREAM &&
             connection == s_wire_connection;
    liot_rtos_exit_critical();
    return accept ? 0 : TIRTC_E_INVALID_HANDLE;
}
static void room_on_audio(tirtc_conn_t connection,
                          const TIRTCFRAMEINFO *frame, void *data) {
    unsigned int slot;
    uint32_t epoch;
    if (frame == NULL || data == NULL || frame->stream_id != ROOM_STREAM ||
        frame->media != TIRTC_AUDIO_ALAW || frame->flags != TIRTC_AUDIOSAMPLE_8K16B1C ||
        frame->length == 0U || frame->length > ROOM_RX_MAX) return;
    liot_rtos_enter_critical();
    if (!s_accept_audio || connection != s_wire_connection) {
        liot_rtos_exit_critical(); return;
    }
    for (slot=0U; slot<ROOM_RX_SLOTS && s_rx[slot].used; ++slot) {}
    if (slot == ROOM_RX_SLOTS) {
        ++s_snapshot.rx_dropped; liot_rtos_exit_critical(); return;
    }
    s_rx[slot].used = true;
    ++s_producers;
    epoch = s_epoch;
    liot_rtos_exit_critical();
    memcpy(s_rx[slot].bytes,data,frame->length);
    s_rx[slot].length = (uint16_t)frame->length;
    s_rx[slot].epoch = epoch;
    s_rx[slot].received_ms = liot_rtos_get_running_time();
    liot_rtos_enter_critical();
    if (s_accept_audio && epoch == s_epoch && s_rx_count < ROOM_RX_SLOTS &&
        s_rx_bytes + frame->length <= 1280U) {
        s_rx_ring[s_rx_write] = (uint8_t)slot;
        s_rx_write = (uint8_t)((s_rx_write + 1U) % ROOM_RX_SLOTS);
        ++s_rx_count;
        s_rx_bytes += frame->length;
    } else { s_rx[slot].used=false; ++s_snapshot.rx_dropped; }
    --s_producers;
    liot_rtos_exit_critical();
    room_wake();
}
static void room_on_command(tirtc_conn_t connection, uint32_t command,
                            const void *data, uint32_t length) {
    unsigned int slot;
    uint32_t epoch;
    if (command != ROOM_COMMAND || data == NULL || length == 0U) return;
    liot_rtos_enter_critical();
    if (!s_accept_commands || connection != s_wire_connection) {
        liot_rtos_exit_critical(); return;
    }
    for (slot=0U; slot<ROOM_COMMAND_SLOTS && s_commands[slot].used; ++slot) {}
    if (slot == ROOM_COMMAND_SLOTS || length > ROOM_COMMAND_MAX) {
        ++s_snapshot.rx_dropped;
        if (length>ROOM_COMMAND_MAX) ++s_command_oversize;
        else s_command_gap=true;
        liot_rtos_exit_critical(); room_wake(); return;
    }
    s_commands[slot].used=true;
    ++s_producers;
    epoch=s_epoch;
    liot_rtos_exit_critical();
    memcpy(s_commands[slot].bytes,data,length);
    s_commands[slot].bytes[length]='\0';
    s_commands[slot].length=(uint16_t)length;
    s_commands[slot].epoch=epoch;
    liot_rtos_enter_critical();
    if (s_accept_commands && epoch == s_epoch && s_cmd_count < ROOM_COMMAND_SLOTS &&
        memchr(s_commands[slot].bytes,'\0',length) == NULL) {
        s_cmd_ring[s_cmd_write]=(uint8_t)slot;
        s_cmd_write=(uint8_t)((s_cmd_write+1U)%ROOM_COMMAND_SLOTS);
        ++s_cmd_count;
    } else { s_commands[slot].used=false; ++s_snapshot.rx_dropped; }
    --s_producers;
    liot_rtos_exit_critical();
    room_wake();
}
static const demo_tirtc_listener_t s_listener = {
    .on_conn_error=room_connection_error,
    .on_disconnected=room_disconnected,
    .on_audio=room_on_audio,
    .on_command=room_on_command,
    .on_subscribe_audio=room_subscribe
};

static void room_mqtt(demo_formal_mqtt_message_kind_e kind, const char *type,
                       const char *channel, const cJSON *payload, void *user) {
    uint32_t version;
    const cJSON *expires;
    time_t now;
    (void)channel; (void)user;
    if (kind != DEMO_FORMAL_MQTT_COMMAND || type == NULL ||
        strcmp(type,"room_assignment_changed") != 0) return;
    /* The adapter passes the full object for this top-level Room event.
     * Older adapters may supply no payload; a hint still triggers a query. */
    if (payload == NULL) {
        liot_rtos_enter_critical(); s_sync_sequence=room_next(s_sync_sequence); liot_rtos_exit_critical();
        room_wake(); return;
    }
    if (!room_number(payload,"assignment_version",&version,false)) return;
    expires=cJSON_GetObjectItemCaseSensitive(payload,"expires_at");
    now=time(NULL);
    if (cJSON_IsNumber(expires) && now > 0 && expires->valuedouble <= (double)now) return;
    liot_rtos_enter_critical();
    if (version >= s_assignment.version) s_sync_sequence=room_next(s_sync_sequence);
    liot_rtos_exit_critical();
    room_wake();
}

static cJSON *room_job_body(const room_job_t *job) {
    cJSON *body=cJSON_CreateObject();
    if (body == NULL || cJSON_AddStringToObject(body,"room_id",job->room_id) == NULL ||
        cJSON_AddNumberToObject(body,"assignment_version",job->version) == NULL ||
        cJSON_AddStringToObject(body,"session_id",job->session_id) == NULL ||
        (job->kind != ROOM_JOB_TOKEN &&
         cJSON_AddStringToObject(body,"state",job->presence) == NULL)) {
        cJSON_Delete(body); return NULL;
    }
    return body;
}
static bool room_parse_assignment(const cJSON *data, room_assignment_t *assignment) {
    unsigned int i;
    const char *desired=room_string(data,"desired_state");
    if (!cJSON_IsObject(data) ||
        !room_number(data,"assignment_version",&assignment->version,false) ||
        !room_copy(data,"room_id",assignment->room_id,sizeof(assignment->room_id),false) ||
        !room_copy(data,"room_code",assignment->room_code,sizeof(assignment->room_code),false)) return false;
    assignment->closed=strcmp(room_string(data,"state"),"room_closed") == 0;
    assignment->desired=strcmp(desired,"joined") == 0 && !assignment->closed;
    if (!assignment->desired) return desired[0] == '\0' || strcmp(desired,"left") == 0 || assignment->closed;
    if (assignment->version == 0U || assignment->room_id[0] == '\0' ||
        strlen(assignment->room_code) != 6U) return false;
    for (i=0U;i<6U;++i) if (assignment->room_code[i]<'0' || assignment->room_code[i]>'9') return false;
    return true;
}
static void room_execute_http(void) {
    cJSON *body=NULL, *root=NULL;
    const cJSON *data, *item;
    char *json=NULL;
    const char *path;
    int status=0, ret;
    uint32_t heartbeat, lease;
    bool submit;
    room_zero(&s_http_result,sizeof(s_http_result));
    s_http_result.code=ROOM_ERR_PROTOCOL;
    s_http_result.started_ms=liot_rtos_get_running_time();
    if (s_http_job.kind == ROOM_JOB_TOKEN) {
        liot_rtos_enter_critical();
        submit=s_claimed && s_epoch == s_http_job.epoch && !s_stopping &&
               s_enabled && s_foreground && s_call == DEMO_GROUP_CALL_NONE &&
               s_intent == s_http_job.intent;
        liot_rtos_exit_critical();
        if (!submit) { s_http_result.code=TIRTC_E_BUSY; return; }
    }
    if (s_http_job.kind == ROOM_JOB_CONNECT) {
        liot_rtos_enter_critical();
        submit=s_claimed && s_epoch == s_http_job.epoch && !s_stopping &&
               s_enabled && s_foreground && s_call == DEMO_GROUP_CALL_NONE &&
               s_intent == s_http_job.intent;
        s_sdk_submit_busy=submit;
        liot_rtos_exit_critical();
        ret=submit ? demo_tirtc_whip_connect(DEMO_TIRTC_OWNER_GROUP_ROOM,
              s_generation,s_http_job.peer,s_http_job.token,
              room_connect_result,&s_connect_context) : TIRTC_E_BUSY;
        liot_rtos_enter_critical();
        s_sdk_submit_busy=false;
        if (ret != 0 && s_epoch == s_http_job.epoch) s_connect_pending=false;
        liot_rtos_exit_critical();
        s_http_result.code=ret;
        return;
    }
    if (s_http_job.kind != ROOM_JOB_ASSIGNMENT) {
        body=room_job_body(&s_http_job);
        if (body != NULL) json=cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (json == NULL) return;
    }
    path=s_http_job.kind == ROOM_JOB_ASSIGNMENT ? "/v1/call/group/device/assignment" :
         s_http_job.kind == ROOM_JOB_TOKEN ? "/v1/call/group/device/connect-token" :
         "/v1/call/group/device/presence";
    ret=demo_binding_service_request_timeout(DEMO_BIND_SERVICE_CALL,
        s_http_job.kind == ROOM_JOB_ASSIGNMENT ? DEMO_BIND_HTTP_GET : DEMO_BIND_HTTP_POST,
        path,json,s_http_response,sizeof(s_http_response),&status,s_http_job.timeout_ms);
    if (json != NULL) { room_zero(json,strlen(json)); cJSON_free(json); }
    if (ret != 0 || status != 200) {
        s_http_result.code=status != 0 && status != 200 ? status : ret != 0 ? ret : ROOM_ERR_PROTOCOL;
        goto done;
    }
    root=room_parse_json(s_http_response,strlen(s_http_response));
    item=cJSON_GetObjectItemCaseSensitive(root,"code");
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)item->valueint) goto done;
    s_http_result.code=item->valueint;
    if (s_http_result.code != 200) goto done;
    data=cJSON_GetObjectItemCaseSensitive(root,"data");
    if (s_http_job.kind == ROOM_JOB_ASSIGNMENT) {
        if (!room_parse_assignment(data,&s_http_result.assignment)) s_http_result.code=ROOM_ERR_PROTOCOL;
    } else if (s_http_job.kind == ROOM_JOB_TOKEN) {
        if (!cJSON_IsObject(data) ||
            !room_copy(data,"peer_id",s_http_result.peer,sizeof(s_http_result.peer),true) ||
            !room_copy(data,"token",s_http_result.token,sizeof(s_http_result.token),true) ||
            !room_number(data,"heartbeat_seconds",&heartbeat,true) ||
            !room_number(data,"lease_seconds",&lease,true) ||
            heartbeat > 3600U || lease > 86400U || lease <= 2U*heartbeat) {
            s_http_result.code=ROOM_ERR_PROTOCOL;
        } else {
            s_http_result.heartbeat_ms=heartbeat*1000U;
            s_http_result.lease_ms=lease*1000U;
            item=cJSON_GetObjectItemCaseSensitive(data,"expires_at");
            if (cJSON_IsNumber(item) && time(NULL)>0 && item->valuedouble <= (double)time(NULL))
                s_http_result.code=ROOM_ERR_LEASE;
        }
    }
done:
    cJSON_Delete(root);
    room_zero(s_http_response,sizeof(s_http_response));
}
static void room_http_task(void *unused) {
    (void)unused;
    for (;;) {
        (void)liot_rtos_semaphore_wait(s_http_wake,LIOT_WAIT_FOREVER);
        if (!s_initialized) continue;
        liot_rtos_enter_critical();
        bool run=s_http_busy && !s_http_done;
        liot_rtos_exit_critical();
        if (!run) continue;
        room_execute_http();
        liot_rtos_enter_critical();
        s_http_done=true;
        liot_rtos_exit_critical();
        room_wake();
    }
}
static bool room_http_available(void) {
    bool available;
    liot_rtos_enter_critical(); available=!s_http_busy; liot_rtos_exit_critical();
    return available;
}
static bool room_schedule(room_job_kind_e kind, const char *presence) {
    if (!room_http_available()) return false;
    room_zero(&s_http_job,sizeof(s_http_job));
    s_http_job.kind=kind;
    s_http_job.intent=s_seen_intent;
    s_http_job.epoch=s_epoch;
    liot_rtos_enter_critical();
    s_http_job.sync_sequence=s_sync_sequence;
    liot_rtos_exit_critical();
    s_http_job.timeout_ms=ROOM_HTTP_TIMEOUT;
    s_http_job.version=s_assignment.version;
    memcpy(s_http_job.room_id,s_assignment.room_id,sizeof(s_http_job.room_id));
    memcpy(s_http_job.session_id,s_session,sizeof(s_http_job.session_id));
    if (presence != NULL) (void)snprintf(s_http_job.presence,sizeof(s_http_job.presence),"%s",presence);
    liot_rtos_enter_critical();
    s_http_busy=true; s_http_done=false;
    liot_rtos_exit_critical();
    (void)liot_rtos_semaphore_release(s_http_wake);
    return true;
}
static void room_close(demo_group_state_e after, int error, const char *presence) {
    if (s_stopping) return;
    liot_rtos_enter_critical();
    s_stopping=true;
    s_mic_gate=false; s_mic_dirty=false;
    s_accept_audio=false; s_accept_commands=false;
    liot_rtos_exit_critical();
    s_after_cleanup=after;
    s_mic_retry_at=0U;
    s_cleanup_error=error;
    s_cleanup_sent=false;
    s_stopped_audio=false;
    if (s_token_requested && s_session[0] != '\0' && !s_terminal_pending) {
        room_zero(&s_terminal,sizeof(s_terminal));
        s_terminal.kind=ROOM_JOB_TERMINAL;
        s_terminal.epoch=s_epoch;
        s_terminal.intent=s_seen_intent;
        s_terminal.version=s_assignment.version;
        s_terminal.timeout_ms=ROOM_HTTP_TIMEOUT;
        memcpy(s_terminal.room_id,s_assignment.room_id,sizeof(s_terminal.room_id));
        memcpy(s_terminal.session_id,s_session,sizeof(s_terminal.session_id));
        (void)snprintf(s_terminal.presence,sizeof(s_terminal.presence),"%s",presence);
        s_terminal_pending=true;
    }
    room_state(after == DEMO_GROUP_SUSPENDED ? DEMO_GROUP_SUSPENDING : DEMO_GROUP_STOPPING,error);
}
static void room_fail(int error) {
    uint32_t delay;
    bool lease_conflict=error == 40921;
    bool permanent=error == 40000 || error == 40300 || error == 40301 ||
        error == ROOM_ERR_PROTOCOL || error == ROOM_ERR_AUDIO;
    if (lease_conflict && !s_conflict_wait) {
        /* A prior device/session can retain the server lease after a lost
         * terminal report. Establish one budget, never extend it per retry. */
        s_conflict_wait=true;
        s_conflict_deadline=liot_rtos_get_running_time()+
            (s_lease_ms != 0U ? s_lease_ms : ROOM_UNKNOWN_LEASE_WAIT);
    }
    ++s_failures;
    s_manual_error=permanent || (lease_conflict ? room_due(s_conflict_deadline) :
        s_failures >= ROOM_MAX_RETRIES);
    delay=1000U << (s_failures > 5U ? 5U : (s_failures-1U));
    if (delay>30000U) delay=30000U;
    s_retry_at=liot_rtos_get_running_time()+delay+(liot_true_rand()%501U);
    if (lease_conflict && !s_manual_error &&
        (int32_t)(s_retry_at-s_conflict_deadline)>0) s_retry_at=s_conflict_deadline;
    room_close(s_manual_error ? DEMO_GROUP_ERROR : DEMO_GROUP_RECONNECTING,error,"connect_failed");
}
static void room_drain_pools(void) {
    liot_rtos_enter_critical();
    if (s_producers == 0U) {
        memset(s_rx,0,sizeof(s_rx)); memset(s_commands,0,sizeof(s_commands));
        s_rx_read=s_rx_write=s_rx_count=0U; s_rx_bytes=0U;
        s_cmd_read=s_cmd_write=s_cmd_count=0U;
    }
    liot_rtos_exit_critical();
}
static bool room_cleanup(void) {
    bool drained;
    int ret;
    if (!s_cleanup_sent) {
        if (s_joined) { (void)room_send_mic(false); (void)room_send_json(room_rpc("leave_room")); }
        s_cleanup_sent=true;
    }
    if (!s_stopped_audio) {
        if (demo_ai_audio_lease_is_valid(&s_audio)) (void)demo_ai_audio_stop(&s_audio);
        s_stopped_audio=true;
    }
    room_drain_pools();
    liot_rtos_enter_critical();
    drained=s_producers == 0U && !s_sdk_submit_busy;
    liot_rtos_exit_critical();
    if (!drained) return false;
    if (s_claimed) {
        (void)demo_tirtc_disconnect(DEMO_TIRTC_OWNER_GROUP_ROOM,s_generation);
        ret=demo_tirtc_session_release(DEMO_TIRTC_OWNER_GROUP_ROOM,s_generation);
        if (ret != 0) return false;
        s_claimed=false;
    }
    if (demo_ai_audio_lease_is_valid(&s_audio) && demo_ai_audio_release(&s_audio) != 0) return false;
    s_generation=0U;
    s_joined=false; s_token_requested=false; s_waiting_token=false;
    s_heartbeat_at=s_lease_deadline=0U;
    room_zero(s_session,sizeof(s_session));
    room_zero(s_media_session,sizeof(s_media_session));
    room_zero(s_wire_room,sizeof(s_wire_room));
    room_zero(s_capture,sizeof(s_capture)); room_zero(s_alaw,sizeof(s_alaw));
    liot_rtos_enter_critical();
    s_wire_connection=NULL;
    s_connect_event=false; s_callback_error=0;
    s_media_idle=true; s_stopping=false;
    liot_rtos_exit_critical();
    room_state(s_after_cleanup,s_cleanup_error);
    return true;
}

static bool room_match_wire(const char *wire) {
    const char *colon;
    if (wire == NULL || wire[0] == '\0' || strlen(wire)>=sizeof(s_wire_room)) return false;
    if (strcmp(wire,s_assignment.room_id) == 0) return true;
    if (s_wire_room[0] != '\0') return strcmp(wire,s_wire_room) == 0;
    colon=strchr(wire,':');
    if (colon == NULL || colon == wire || strcmp(colon+1,s_assignment.room_id) != 0) return false;
    memcpy(s_wire_room,wire,strlen(wire)+1U);
    return true;
}
static void room_signal_json(const cJSON *root) {
    const cJSON *result, *params, *error, *members;
    const char *method;
    uint32_t id;
    if (!cJSON_IsObject(root) || strcmp(room_string(root,"jsonrpc"),"2.0") != 0) return;
    if (!s_joined && room_number(root,"id",&id,true) && id == s_join_id) {
        result=cJSON_GetObjectItemCaseSensitive(root,"result");
        error=cJSON_GetObjectItemCaseSensitive(root,"error");
        if (error != NULL || !cJSON_IsObject(result) ||
            !room_copy(result,"session_id",s_media_session,sizeof(s_media_session),true) ||
            !room_format(cJSON_GetObjectItemCaseSensitive(result,"input_audio")) ||
            !room_format(cJSON_GetObjectItemCaseSensitive(result,"output_audio"))) {
            const cJSON *code=cJSON_GetObjectItemCaseSensitive(error,"code");
            room_fail(cJSON_IsNumber(code) ? code->valueint : ROOM_ERR_PROTOCOL); return;
        }
        if (room_string(result,"room_id")[0] != '\0' && !room_match_wire(room_string(result,"room_id"))) {
            room_fail(ROOM_ERR_PROTOCOL); return;
        }
        if (room_due(s_lease_deadline)) return;
        liot_rtos_enter_critical();
        if (!s_enabled || !s_foreground || s_call != DEMO_GROUP_CALL_NONE ||
            s_stopping || s_intent != s_seen_intent) {
            liot_rtos_exit_critical(); return;
        }
        s_joined=true; s_heartbeat_at=0U;
        s_mic_gate=false; s_mic_dirty=true; s_accept_audio=true;
        liot_rtos_exit_critical();
        s_mic_retry_at=0U;
        room_state(DEMO_GROUP_LISTENING,0);
        (void)room_send_json(room_rpc("get_room_snapshot"));
    }
    method=room_string(root,"method");
    params=cJSON_GetObjectItemCaseSensitive(root,"params");
    if (method[0] == '\0' || !cJSON_IsObject(params) || !room_match_wire(room_string(params,"room_id"))) return;
    if (strcmp(method,"room_closed") == 0) {
        s_assignment.closed=true; s_assignment.desired=false;
        room_close(DEMO_GROUP_NO_ROOM,40400,"left");
    } else if (strcmp(method,"room_snapshot") == 0) {
        members=cJSON_GetObjectItemCaseSensitive(params,"participants");
        if (!cJSON_IsArray(members)) {
            room_fail(ROOM_ERR_PROTOCOL); return;
        }
        liot_trace("[ROOM] snapshot participants=%d\r\n",cJSON_GetArraySize(members));
    } else if (strcmp(method,"participant_joined") == 0 ||
               strcmp(method,"participant_left") == 0 ||
               strcmp(method,"participant_mic_state_changed") == 0) {
        liot_trace("[ROOM] event=%s\r\n",method);
    }
}
static void room_process_commands(void) {
    uint8_t slot;
    cJSON *root;
    unsigned int count;
    for (count=0U;count<ROOM_COMMAND_SLOTS;++count) {
        liot_rtos_enter_critical();
        if (s_cmd_count == 0U) { liot_rtos_exit_critical(); break; }
        slot=s_cmd_ring[s_cmd_read];
        s_cmd_read=(uint8_t)((s_cmd_read+1U)%ROOM_COMMAND_SLOTS); --s_cmd_count;
        liot_rtos_exit_critical();
        if (!s_stopping && s_commands[slot].epoch == s_epoch) {
            root=room_parse_json(s_commands[slot].bytes,s_commands[slot].length);
            if (root != NULL) room_signal_json(root);
            else room_fail(ROOM_ERR_PROTOCOL);
            cJSON_Delete(root);
        }
        liot_rtos_enter_critical(); s_commands[slot].used=false; liot_rtos_exit_critical();
    }
}
static void room_audio_tick(void) {
    uint8_t slot;
    unsigned int count, i;
    uint32_t epoch;
    bool on, active;
    size_t used;
    int ret;
    TIRTCFRAMEINFO frame;
    for (count=0U;count<ROOM_RX_SLOTS;++count) {
        liot_rtos_enter_critical();
        if (s_rx_count == 0U) { liot_rtos_exit_critical(); break; }
        slot=s_rx_ring[s_rx_read]; s_rx_read=(uint8_t)((s_rx_read+1U)%ROOM_RX_SLOTS); --s_rx_count;
        s_rx_bytes -= s_rx[slot].length;
        active=s_accept_audio;
        liot_rtos_exit_critical();
        if (active && s_rx[slot].epoch == s_epoch &&
            liot_rtos_get_running_time()-s_rx[slot].received_ms <= ROOM_RX_MAX_AGE) {
            for (i=0U;i<s_rx[slot].length;++i) {
                int16_t sample=demo_g711_alaw_decode_sample(s_rx[slot].bytes[i]);
                s_play[2U*i]=sample; s_play[2U*i+1U]=sample;
            }
            ret=demo_ai_audio_play(&s_audio,s_play,2U*s_rx[slot].length);
        } else ret=-1;
        liot_rtos_enter_critical();
        s_rx[slot].used=false;
        if (ret != 0) ++s_snapshot.rx_dropped;
        liot_rtos_exit_critical();
    }
    liot_rtos_enter_critical(); on=s_mic_gate && s_accept_audio; epoch=s_epoch; liot_rtos_exit_critical();
    if (!on) return;
    ret=demo_ai_audio_record_20ms(&s_audio,s_capture);
    liot_rtos_enter_critical();
    on=s_mic_gate && s_accept_audio && epoch == s_epoch && s_enabled && s_foreground && s_call == DEMO_GROUP_CALL_NONE;
    liot_rtos_exit_critical();
    if (!on) { room_zero(s_capture,sizeof(s_capture)); return; }
    if (ret != 0) { room_fail(ROOM_ERR_AUDIO); return; }
    for (i=0U;i<160U;++i) s_alaw[i]=demo_g711_alaw_encode_sample(
        (int16_t)(((int32_t)s_capture[2U*i]+s_capture[2U*i+1U])/2));
    ret=demo_tirtc_get_send_buffer_used(DEMO_TIRTC_OWNER_GROUP_ROOM,s_generation,&used);
    if (ret == 0 && used > ROOM_SEND_HIGH_WATER) ret=TIRTC_E_BUSY;
    else if (ret == 0) {
        memset(&frame,0,sizeof(frame));
        frame.stream_id=ROOM_STREAM; frame.media=TIRTC_AUDIO_ALAW;
        frame.flags=TIRTC_AUDIOSAMPLE_8K16B1C; frame.length=sizeof(s_alaw);
        frame.ts=liot_rtos_get_running_time()-s_session_started;
        liot_rtos_enter_critical(); on=s_mic_gate && s_accept_audio && epoch == s_epoch; liot_rtos_exit_critical();
        if (!on) return;
        ret=demo_tirtc_send_audio(DEMO_TIRTC_OWNER_GROUP_ROOM,s_generation,&frame,s_alaw);
    }
    if (ret == TIRTC_E_BUSY) {
        liot_rtos_enter_critical(); ++s_snapshot.tx_dropped; liot_rtos_exit_critical();
    } else if (ret < 0) room_fail(ret);
}

static void room_consume_http(void) {
    bool done, valid;
    room_job_kind_e kind;
    int code;
    liot_rtos_enter_critical(); done=s_http_busy && s_http_done; liot_rtos_exit_critical();
    if (!done) return;
    kind=s_http_job.kind; code=s_http_result.code;
    valid=s_http_job.intent == s_seen_intent && s_http_job.epoch == s_epoch && !s_stopping;
    liot_trace("[ROOM] service kind=%u code=%d epoch=%u\r\n",(unsigned int)kind,code,(unsigned int)s_http_job.epoch);
    /* Consume before clearing the mailbox; helper cannot overwrite it yet. */
    if (kind == ROOM_JOB_ASSIGNMENT && s_http_job.intent == s_seen_intent && !s_stopping) {
        if (code == 200) {
            /* A stale response still completes this sync request. Only a new
             * hint/periodic sync should query again, never a busy retry loop. */
            s_sync_done=s_http_job.sync_sequence;
            bool changed=s_http_result.assignment.version != s_assignment.version ||
                strcmp(s_http_result.assignment.room_id,s_assignment.room_id) != 0 ||
                s_http_result.assignment.desired != s_assignment.desired;
            if (s_http_result.assignment.version >= s_assignment.version) {
                if (changed && !s_media_idle) {
                    /* Capture the old tuple before replacing assignment. */
                    room_close(DEMO_GROUP_RECONNECTING,0,"left");
                    s_retry_at=liot_rtos_get_running_time();
                }
                liot_rtos_enter_critical();
                s_assignment=s_http_result.assignment;
                memcpy(s_snapshot.room_code,s_assignment.room_code,sizeof(s_snapshot.room_code));
                liot_rtos_exit_critical();
                if (changed) { s_manual_error=false; s_failures=0U; s_conflict_wait=false; }
                s_sync_done=s_http_job.sync_sequence;
                if (s_media_idle && !s_stopping && s_enabled && !s_manual_error) room_state(
                    s_assignment.desired ? DEMO_GROUP_WAIT_RESOURCE : DEMO_GROUP_NO_ROOM,
                    s_assignment.closed ? 40400 : 0);
                s_deadline=liot_rtos_get_running_time()+ROOM_RESOURCE_TIMEOUT;
            }
        } else if (s_enabled && s_media_idle) {
            s_sync_done=s_http_job.sync_sequence;
            room_fail(code);
        } else {
            /* A background failure does not retry every 20/100 ms and cannot
             * monopolize the shared HTTP client. The next hint/online edge
             * (or enabled periodic sync) starts a fresh attempt. */
            s_sync_done=s_http_job.sync_sequence;
        }
    } else if (valid && kind == ROOM_JOB_TOKEN) {
        s_waiting_token=false;
        if (code != 200) room_fail(code);
        else if (room_allowed() && s_claimed) {
            s_conflict_wait=false;
            s_heartbeat_ms=s_http_result.heartbeat_ms;
            s_lease_ms=s_http_result.lease_ms;
            s_lease_deadline=s_http_result.started_ms+s_lease_ms;
            if (room_due(s_lease_deadline)) room_fail(ROOM_ERR_LEASE);
            else {
                /* Reuse mailbox for SDK submission only after all token
                 * fields have been copied. Keep it busy across this handoff. */
                memcpy(s_http_job.peer,s_http_result.peer,sizeof(s_http_job.peer));
                memcpy(s_http_job.token,s_http_result.token,sizeof(s_http_job.token));
                s_http_job.kind=ROOM_JOB_CONNECT;
                s_connect_context.epoch=s_epoch;
                liot_rtos_enter_critical();
                s_connect_pending=true; s_connect_event=false;
                s_http_done=false;
                liot_rtos_exit_critical();
                room_zero(&s_http_result,sizeof(s_http_result));
                s_deadline=liot_rtos_get_running_time()+ROOM_CONNECT_TIMEOUT;
                room_state(DEMO_GROUP_CONNECTING,0);
                (void)liot_rtos_semaphore_release(s_http_wake);
                return;
            }
        }
    } else if (valid && kind == ROOM_JOB_CONNECT && code != 0) room_fail(code);
    else if (valid && kind == ROOM_JOB_PRESENCE && s_joined) {
        if (code == 200) {
            s_lease_deadline=s_http_result.started_ms+s_lease_ms;
            s_heartbeat_at=s_http_result.started_ms+s_heartbeat_ms;
            s_failures=0U;
        } else if (code == 401 || code == 40300 || code == 40921 || code == 40400) room_fail(code);
        else s_heartbeat_at=liot_rtos_get_running_time()+1000U;
    }
    room_zero(&s_http_job,sizeof(s_http_job)); room_zero(&s_http_result,sizeof(s_http_result));
    liot_rtos_enter_critical(); s_http_busy=false; s_http_done=false; liot_rtos_exit_critical();
}
static void room_try_terminal(void) {
    if (!s_terminal_pending || !room_http_available()) return;
    s_http_job=s_terminal;
    room_zero(&s_terminal,sizeof(s_terminal)); s_terminal_pending=false;
    liot_rtos_enter_critical(); s_http_busy=true; s_http_done=false; liot_rtos_exit_critical();
    (void)liot_rtos_semaphore_release(s_http_wake);
}
static bool room_start_session(void) {
    demo_binding_tirtc_identity_t identity;
    uint8_t speaker, mic;
    int ret;
    if (!room_allowed() || !room_http_available() || s_terminal_pending) return false;
    /* session_claim owns its own admission transaction and rejects callers
     * holding the UI admission gate. Mark local preparation busy before the
     * claim so a concurrent incoming call cannot observe a false idle gap. */
    liot_rtos_enter_critical();
    if (!s_enabled || !s_foreground || s_call != DEMO_GROUP_CALL_NONE || s_intent != s_seen_intent) {
        liot_rtos_exit_critical(); return false;
    }
    s_media_idle=false;
    liot_rtos_exit_critical();
    ret=demo_tirtc_session_claim(DEMO_TIRTC_OWNER_GROUP_ROOM,&s_generation);
    if (ret == 0) {
        liot_rtos_enter_critical();
        s_claimed=true;
        s_epoch=room_next(s_epoch);
        speaker=s_speaker; mic=s_mic_level;
        liot_rtos_exit_critical();
    }
    if (ret != 0) {
        liot_rtos_enter_critical(); s_media_idle=true; liot_rtos_exit_critical();
        return false;
    }
    if (!room_allowed()) { room_close(DEMO_GROUP_SUSPENDED,0,"suspended"); return true; }
    s_token_requested=false; s_joined=false;
    s_connect_event=false; s_callback_error=0; s_wire_room[0]='\0';
    memset(&identity,0,sizeof(identity));
    ret=demo_binding_get_tirtc_identity(&identity);
    if (ret == 0) memcpy(s_device,identity.device_id,sizeof(s_device));
    room_zero(&identity,sizeof(identity));
    if (ret != 0 || s_device[0] == '\0') { room_fail(ROOM_ERR_OFFLINE); return true; }
    ret=demo_ai_audio_acquire(DEMO_AI_AUDIO_OWNER_GROUP_ROOM,&s_audio);
    if (ret == 0) ret=demo_ai_audio_prepare_session(&s_audio,speaker,mic);
    if (ret != 0) { room_fail(ROOM_ERR_AUDIO); return true; }
    if (!room_allowed()) { room_close(DEMO_GROUP_SUSPENDED,0,"suspended"); return true; }
    (void)snprintf(s_session,sizeof(s_session),"%08x%08x%08x%08x",
        (unsigned int)liot_true_rand(),(unsigned int)liot_true_rand(),
        (unsigned int)liot_true_rand(),(unsigned int)liot_true_rand());
    s_session_started=liot_rtos_get_running_time();
    s_join_id=room_next(s_join_id);
    s_token_requested=true; s_waiting_token=true;
    room_state(DEMO_GROUP_SYNCING,0);
    if (!room_schedule(ROOM_JOB_TOKEN,NULL)) room_fail(TIRTC_E_BUSY);
    return true;
}
/* One nonblocking owner iteration, also used by host fault-injection tests.
 * The only naturally paced media operation is one 20 ms capture. */
static uint32_t room_step(void) {
    demo_binding_snapshot_t binding;
        bool enabled, foreground, has_call, suspend_requested, ready, event, on, dirty, levels, command_gap;
        uint32_t command_oversize;
        uint32_t intent, sync;
        int error;
        uint8_t speaker, mic;
        if (!s_initialized) return 100U;
        liot_rtos_enter_critical();
        enabled=s_enabled; foreground=s_foreground; has_call=s_call != DEMO_GROUP_CALL_NONE;
        suspend_requested=s_suspend_requested; s_suspend_requested=false;
        intent=s_intent; sync=s_sync_sequence;
        liot_rtos_exit_critical();
        demo_binding_get_snapshot(&binding);
        if (binding.state == DEMO_BIND_BOUND) s_bound_seen=true;
        else if (s_bound_seen && (binding.state == DEMO_BIND_RESTARTING ||
                 binding.state == DEMO_BIND_REPORTING || binding.state == DEMO_BIND_WAIT_GRANT)) {
            /* VERIFYING/ERROR/WAIT_NETWORK/DISCOVERING are normal recovery
             * states. Clear intent only after confirmed unbind/reprovision. */
            s_bound_seen=false; demo_group_intercom_exit(); enabled=false;
            if (!s_media_idle && !s_stopping) room_close(DEMO_GROUP_IDLE,0,"left");
            liot_rtos_enter_critical();
            s_intent=room_next(s_intent);
            memset(&s_assignment,0,sizeof(s_assignment));
            memset(s_snapshot.room_code,0,sizeof(s_snapshot.room_code));
            intent=s_intent;
            liot_rtos_exit_critical();
        }
        ready=binding.state == DEMO_BIND_BOUND && demo_net_time_is_data_ready() &&
              demo_formal_mqtt_is_online() && demo_tirtc_is_ready();
        if (intent != s_seen_intent) {
            s_seen_intent=intent;
            s_manual_error=false; s_failures=0U; s_retry_at=0U; s_conflict_wait=false;
            if (!s_media_idle && !s_stopping) room_close(DEMO_GROUP_IDLE,0,enabled ? "connect_failed" : "left");
            if (enabled) {
                liot_rtos_enter_critical(); s_sync_sequence=room_next(s_sync_sequence); sync=s_sync_sequence; liot_rtos_exit_critical();
                s_deadline=liot_rtos_get_running_time()+ROOM_RESOURCE_TIMEOUT;
            }
        }
        if (ready && !s_transport_was_ready) {
            liot_rtos_enter_critical(); s_sync_sequence=room_next(s_sync_sequence); sync=s_sync_sequence; liot_rtos_exit_critical();
            s_sync_at=liot_rtos_get_running_time()+ROOM_SYNC_INTERVAL;
            if (!s_manual_error) s_retry_at=0U;
        }
        s_transport_was_ready=ready;
        if ((!enabled || has_call || !foreground || suspend_requested || !ready) && !s_media_idle && !s_stopping)
            room_close(!enabled ? DEMO_GROUP_IDLE : has_call || !foreground || suspend_requested ? DEMO_GROUP_SUSPENDED : DEMO_GROUP_RECONNECTING,
                ready || !enabled || has_call || suspend_requested ? 0 : ROOM_ERR_OFFLINE,
                !enabled ? "left" : has_call || !foreground || suspend_requested ? "suspended" : "connect_failed");
        room_consume_http();
        if (s_stopping) (void)room_cleanup();
        room_try_terminal();
        if (s_stopping) return 20U;
        if (!enabled) {
            room_state(DEMO_GROUP_IDLE,0);
            /* Background sync is coalesced and never claims media. */
            if (ready && sync != s_sync_done && !s_terminal_pending) {
                demo_tirtc_connection_snapshot_t connection;
                demo_tirtc_get_connection_snapshot(&connection);
                /* Background notifications must not queue behind active calls
                 * and delay their next token request on the HTTP singleton. */
                if (!has_call && connection.owner == DEMO_TIRTC_OWNER_NONE &&
                    !connection.connect_pending && !connection.disconnect_pending &&
                    !connection.connect_callback_pending && !connection.expected_incoming &&
                    connection.connection_users == 0U)
                    (void)room_schedule(ROOM_JOB_ASSIGNMENT,NULL);
            }
            return 100U;
        }
        if (has_call || !foreground) { room_state(DEMO_GROUP_SUSPENDED,0); return 50U; }
        if (!ready) { room_state(DEMO_GROUP_RECONNECTING,ROOM_ERR_OFFLINE); return 100U; }
        if (room_due(s_sync_at)) {
            liot_rtos_enter_critical(); s_sync_sequence=room_next(s_sync_sequence); sync=s_sync_sequence; liot_rtos_exit_critical();
            s_sync_at=liot_rtos_get_running_time()+ROOM_SYNC_INTERVAL;
        }
        liot_rtos_enter_critical();
        command_gap=s_command_gap; s_command_gap=false;
        command_oversize=s_command_oversize; s_command_oversize=0U;
        event=s_connect_event; error=s_connect_error; s_connect_event=false;
        if (s_callback_error != 0 && s_callback_epoch == s_epoch) { error=s_callback_error; event=true; s_callback_error=0; }
        liot_rtos_exit_critical();
        if (command_oversize != 0U) {
            liot_trace("[ROOM] oversized commands=%u; bounded snapshot omitted, resync assignment\r\n",(unsigned int)command_oversize);
            liot_rtos_enter_critical(); s_sync_sequence=room_next(s_sync_sequence); sync=s_sync_sequence; liot_rtos_exit_critical();
        }
        if (command_gap && !s_media_idle) {
            /* The lost frame might be room_closed. A new authenticated
             * session is safer than renewing an uncertain media session. */
            liot_trace("[ROOM] command queue overflow; reconnecting\r\n");
            room_fail(TIRTC_E_BUSY); return 0U;
        }
        if (event && !s_media_idle) {
            if (error != 0) room_fail(error);
            else {
                liot_rtos_enter_critical();
                bool allow=s_enabled && s_foreground && s_call == DEMO_GROUP_CALL_NONE &&
                    !s_stopping && s_intent == s_seen_intent;
                s_accept_commands=allow;
                liot_rtos_exit_critical();
                if (!allow) return 0U;
                error=demo_tirtc_subscribe_audio(DEMO_TIRTC_OWNER_GROUP_ROOM,s_generation,ROOM_STREAM);
                if (error >= 0) error=room_send_join();
                if (error < 0) room_fail(error);
                else { s_deadline=liot_rtos_get_running_time()+ROOM_JOIN_TIMEOUT; room_state(DEMO_GROUP_JOINING,0); }
            }
        }
        room_process_commands();
        if (s_stopping) return 0U;
        if (!s_media_idle) {
            if (s_lease_deadline != 0U && room_due(s_lease_deadline)) { room_fail(ROOM_ERR_LEASE); return 0U; }
            if (!s_joined && !s_waiting_token && room_due(s_deadline)) { room_fail(TIRTC_E_TIMEOUTED); return 0U; }
            if (s_joined) {
                liot_rtos_enter_critical();
                on=s_mic_gate;
                dirty=s_mic_dirty && (s_mic_retry_at == 0U || room_due(s_mic_retry_at));
                if (dirty) s_mic_dirty=false;
                levels=s_levels_dirty; s_levels_dirty=false; speaker=s_speaker; mic=s_mic_level;
                liot_rtos_exit_critical();
                if (levels) (void)demo_ai_audio_set_levels(&s_audio,speaker,mic);
                if (dirty) {
                    /* State notification failure must not interrupt audio.
                     * Retry the latest gate, at most once per second, and
                     * preserve any key update made during the send. */
                    error=room_send_mic(on);
                    s_mic_retry_at=error < 0 ? liot_rtos_get_running_time()+ROOM_MIC_RETRY_INTERVAL : 0U;
                    if (error < 0) {
                        liot_rtos_enter_critical(); s_mic_dirty=true; liot_rtos_exit_critical();
                    }
                }
                room_state(on ? DEMO_GROUP_MIC_ON : DEMO_GROUP_LISTENING,0);
                if (s_heartbeat_at == 0U || room_due(s_heartbeat_at)) (void)room_schedule(ROOM_JOB_PRESENCE,"joined");
                if (sync != s_sync_done && room_http_available()) (void)room_schedule(ROOM_JOB_ASSIGNMENT,NULL);
                room_audio_tick();
                if (on) return 0U;
            }
        } else if (s_manual_error && sync != s_sync_done) {
            /* A real server relationship change can recover a terminal
             * error; the same assignment cannot silently retry bad media. */
            (void)room_schedule(ROOM_JOB_ASSIGNMENT,NULL);
        } else if (!s_manual_error && (s_retry_at == 0U || room_due(s_retry_at))) {
            if (s_retry_at != 0U) {
                s_retry_at=0U;
                liot_rtos_enter_critical(); s_sync_sequence=room_next(s_sync_sequence); sync=s_sync_sequence; liot_rtos_exit_critical();
            }
            if (sync != s_sync_done) {
                if (room_schedule(ROOM_JOB_ASSIGNMENT,NULL)) room_state(DEMO_GROUP_SYNCING,0);
            } else if (!s_assignment.desired) room_state(DEMO_GROUP_NO_ROOM,s_assignment.closed ? 40400 : 0);
            else if (!room_start_session()) {
                if (room_due(s_deadline)) { s_manual_error=true; room_state(DEMO_GROUP_ERROR,TIRTC_E_BUSY); }
                else room_state(DEMO_GROUP_WAIT_RESOURCE,0);
            }
        }
        return 20U;
}
static void room_task(void *unused) {
    (void)unused;
    for (;;) {
        uint32_t delay=room_step();
        if (delay != 0U) (void)liot_rtos_semaphore_wait(s_wake,delay);
    }
}

int demo_group_intercom_init(void) {
    liot_task_t task=NULL;
    if (s_init_attempted) return s_initialized ? 0 : ROOM_ERR_INIT;
    s_init_attempted=true;
    s_snapshot.state=DEMO_GROUP_IDLE;
    if (liot_rtos_semaphore_create(&s_wake,0U) != LIOT_OSI_SUCCESS ||
        liot_rtos_semaphore_create(&s_http_wake,0U) != LIOT_OSI_SUCCESS) goto failed;
    if (demo_formal_mqtt_register_handler(room_mqtt,NULL) != 0) goto failed;
    demo_tirtc_set_feature_listener(DEMO_TIRTC_FEATURE_GROUP_ROOM,&s_listener);
    if (liot_rtos_task_create(&task,12U*1024U,LIOT_APP_TASK_PRIORITY,
            "room_http",room_http_task,NULL) != LIOT_OSI_SUCCESS) goto unregister;
    task=NULL;
    if (liot_rtos_task_create(&task,24U*1024U,LIOT_APP_TASK_PRIORITY,
            "group_room",room_task,NULL) != LIOT_OSI_SUCCESS) goto unregister;
    s_initialized=true;
    room_wake();
    liot_trace("[ROOM] ready; media stack=24576 service stack=12288 rx=8x640 command=2x4096\r\n");
    return 0;
unregister:
    demo_formal_mqtt_unregister_handler(room_mqtt,NULL);
    demo_tirtc_set_feature_listener(DEMO_TIRTC_FEATURE_GROUP_ROOM,NULL);
failed:
    /* Never delete a task which may already own callbacks/RTOS state. */
    s_snapshot.state=DEMO_GROUP_ERROR; s_snapshot.error=ROOM_ERR_INIT;
    liot_trace("[ROOM] init failed; other features remain available\r\n");
    return ROOM_ERR_INIT;
}
void demo_group_intercom_enter(void) {
    if (!s_initialized) return;
    liot_rtos_enter_critical();
    if (!s_enabled) { s_enabled=true; s_intent=room_next(s_intent); }
    s_mic_gate=false;
    liot_rtos_exit_critical(); room_wake();
}
void demo_group_intercom_exit(void) {
    liot_rtos_enter_critical();
    if (s_enabled) { s_enabled=false; s_intent=room_next(s_intent); }
    s_mic_gate=false; s_accept_audio=false; s_accept_commands=false;
    liot_rtos_exit_critical(); room_wake();
}
void demo_group_intercom_retry(void) {
    if (!s_initialized) return;
    liot_rtos_enter_critical();
    if (s_enabled) s_intent=room_next(s_intent);
    s_mic_gate=false;
    liot_rtos_exit_critical(); room_wake();
}
void demo_group_intercom_toggle_mic(void) {
    uint32_t generation;
    liot_rtos_enter_critical(); generation=s_generation; liot_rtos_exit_critical();
    (void)demo_group_intercom_toggle_mic_for_generation(generation);
}
bool demo_group_intercom_toggle_mic_for_generation(uint32_t generation) {
    bool changed=false;
    liot_rtos_enter_critical();
    if (generation != 0U && generation == s_generation && s_enabled && s_foreground &&
        s_call == DEMO_GROUP_CALL_NONE && s_accept_audio && !s_stopping) {
        s_mic_gate=!s_mic_gate; s_mic_dirty=true; changed=true;
    }
    liot_rtos_exit_critical(); room_wake(); return changed;
}
void demo_group_intercom_set_foreground_allowed(bool allowed) {
    liot_rtos_enter_critical();
    if (s_foreground != allowed) {
        s_foreground=allowed;
        if (allowed) s_sync_sequence=room_next(s_sync_sequence);
        else {
            s_suspend_requested=true;
            s_mic_gate=false; s_accept_audio=false; s_accept_commands=false;
        }
    }
    liot_rtos_exit_critical(); room_wake();
}
void demo_group_intercom_set_audio_levels(uint8_t speaker, uint8_t mic) {
    if (speaker<1U) speaker=1U;
    if (speaker>10U) speaker=10U;
    if (mic<1U) mic=1U;
    if (mic>10U) mic=10U;
    liot_rtos_enter_critical();
    s_speaker=speaker; s_mic_level=mic; s_levels_dirty=true;
    liot_rtos_exit_critical(); room_wake();
}
bool demo_group_intercom_is_idle(void) {
    bool idle;
    liot_rtos_enter_critical(); idle=s_media_idle && s_producers == 0U && !s_sdk_submit_busy; liot_rtos_exit_critical();
    return idle;
}
bool demo_group_intercom_is_enabled(void) {
    bool enabled;
    liot_rtos_enter_critical(); enabled=s_enabled; liot_rtos_exit_critical(); return enabled;
}
void demo_group_intercom_get_snapshot(demo_group_snapshot_t *out) {
    if (out == NULL) return;
    liot_rtos_enter_critical();
    *out=s_snapshot; out->enabled=s_enabled; out->mic_on=s_mic_gate;
    out->generation=s_accept_audio ? s_generation : 0U;
    liot_rtos_exit_critical();
}
int demo_group_intercom_reserve_call(demo_group_call_e caller,uint32_t *ticket) {
    int ret;
    if (ticket == NULL || caller == DEMO_GROUP_CALL_NONE || caller>DEMO_GROUP_CALL_DEVICE) return -1;
    *ticket=0U;
    liot_rtos_enter_critical();
    if (s_call != DEMO_GROUP_CALL_NONE) ret=-1;
    else if (!s_initialized || (!s_enabled && s_media_idle)) ret=0;
    else {
        s_call=caller; s_ticket_sequence=room_next(s_ticket_sequence);
        s_call_ticket=s_ticket_sequence; *ticket=s_call_ticket;
        /* A quick cancel may release this ticket before the owner runs.
         * Keep the stop request so closed media gates cannot strand a join. */
        s_suspend_requested=true;
        s_mic_gate=false; s_accept_audio=false; s_accept_commands=false;
        ret=1;
    }
    liot_rtos_exit_critical(); room_wake(); return ret;
}
bool demo_group_intercom_call_ready(demo_group_call_e caller,uint32_t ticket) {
    bool ready;
    liot_rtos_enter_critical();
    ready=ticket != 0U && s_call == caller && s_call_ticket == ticket &&
          s_media_idle && s_producers == 0U && !s_sdk_submit_busy;
    liot_rtos_exit_critical(); return ready;
}
void demo_group_intercom_release_call(demo_group_call_e caller,uint32_t ticket) {
    liot_rtos_enter_critical();
    if (ticket != 0U && s_call == caller && s_call_ticket == ticket) {
        s_call=DEMO_GROUP_CALL_NONE; s_call_ticket=0U;
        s_sync_sequence=room_next(s_sync_sequence);
    }
    liot_rtos_exit_critical(); room_wake();
}
bool demo_group_intercom_has_call(void) {
    bool occupied;
    liot_rtos_enter_critical(); occupied=s_call != DEMO_GROUP_CALL_NONE; liot_rtos_exit_critical(); return occupied;
}
