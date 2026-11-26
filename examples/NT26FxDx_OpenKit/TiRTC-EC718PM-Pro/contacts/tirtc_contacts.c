/* ThingConnect contacts, using device and VOIP lists on separate services.
 * Protocol provenance and licenses are recorded in this module's README. */
#include "tirtc_contacts.h"
#include "../platform/formal_mqtt.h"
#include "../platform/json_guard.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include "liot_os.h"
#include <string.h>
#include <stdio.h>
#define CONTACT_RESPONSE_MAX 8192U
#define CONTACT_REFRESH_MS 30000U
#define CONTACT_RESOLVE_MS 11000U
#define CONTACT_HTTP_MS 10000U
#define CONTACT_EXPIRE_MS 60000U
#define CONTACT_RETRY_MIN_MS 5000U
#define CONTACT_RETRY_MAX_MS 60000U
#define CONTACT_ERROR_JSON (-6101)
#define CONTACT_ERROR_HTTP (-6102)
#define CONTACT_ERROR_AUTH (-6103)
#define CONTACT_ERROR_STALE (-6104)

static tirtc_contact_t s_contacts[TIRTC_UI_CONTACT_MAX];
static tirtc_contact_t s_work[2][TIRTC_UI_CONTACT_MAX]; /* single HTTP owner, separate scopes */
static char s_response[CONTACT_RESPONSE_MAX];
static demo_binding_service_context_t s_context;
static uint8_t s_count;
static struct {
    uint32_t sample_ms;
    bool known;
} s_scope_cache[2];
static bool s_loading, s_stale = true, s_limited;
static bool s_started, s_starting, s_handler_registered;
static liot_task_t s_task;
static int s_error, s_reported_error;
static uint32_t s_request = 1U, s_completed_request;
static uint32_t s_retry_ms = CONTACT_RETRY_MIN_MS, s_next_at, s_attempt_at;
static bool s_scheduled, s_attempted;
static struct {
    uint32_t ticket, refresh, started;
    bool active;
    tirtc_contact_scope_t scope;
    tirtc_contact_result_t result;
    char target[257];
    demo_binding_service_context_t context;
    tirtc_contact_t contact;
} s_resolve;
static uint32_t s_resolve_sequence;

static bool same_context(const demo_binding_service_context_t *a,
                         const demo_binding_service_context_t *b)
{
    return a->credential_epoch == b->credential_epoch &&
        a->binding_generation == b->binding_generation &&
        a->route_generation == b->route_generation && a->sim_id == b->sim_id;
}

static bool scope_fresh_locked(bool wx, uint32_t now)
{
    return s_scope_cache[wx].known && now - s_scope_cache[wx].sample_ms <= CONTACT_EXPIRE_MS;
}

static void clear_contacts_locked(void)
{
    memset(s_contacts,0,sizeof(s_contacts));
    memset(s_scope_cache,0,sizeof(s_scope_cache));
    s_count=0U;
}

static void invalidate_scope_locked(bool wx)
{
    unsigned keep=0U;
    for(unsigned i=0U;i<s_count;++i){
        if(s_contacts[i].wechat==wx)continue;
        if(keep!=i)s_contacts[keep]=s_contacts[i];
        ++keep;
    }
    memset(s_contacts+keep,0,sizeof(s_contacts[0])*(s_count-keep));
    s_count=(uint8_t)keep;
    memset(&s_scope_cache[wx],0,sizeof(s_scope_cache[wx]));
}

/* Validate the complete name, then truncate only at a UTF-8 boundary. Control
 * characters become spaces. Surrogates/overlong encodings cannot enter LVGL. */
static bool display_name(char *out, size_t capacity, const char *value)
{
    size_t input = 0U, used = 0U;
    bool full = false;
    if (!out || capacity == 0U || !value) return false;
    out[0] = '\0';
    while (value[input]) {
        unsigned char c = (unsigned char)value[input];
        uint32_t point;
        unsigned length, j;
        if (c < 0x80U) { length = 1U; point = c; }
        else if (c >= 0xc2U && c <= 0xdfU) { length = 2U; point = c & 31U; }
        else if (c >= 0xe0U && c <= 0xefU) { length = 3U; point = c & 15U; }
        else if (c >= 0xf0U && c <= 0xf4U) { length = 4U; point = c & 7U; }
        else return false;
        for (j = 1U; j < length; ++j) {
            unsigned char tail = (unsigned char)value[input+j];
            if ((tail & 0xc0U) != 0x80U) return false;
            point = (point << 6U) | (tail & 63U);
        }
        if ((length == 2U && point < 0x80U) || (length == 3U && point < 0x800U) ||
            (length == 4U && point < 0x10000U) || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU)) return false;
        if (!full && used + length < capacity) {
            if (length == 1U && (point < 32U || point == 127U)) out[used++] = ' ';
            else { memcpy(out+used,value+input,length); used += length; }
        } else full = true;
        input += length;
    }
    out[used] = '\0';
    return true;
}

static bool copy_id(const cJSON *object, const char *key, char *out, size_t capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object,key);
    size_t i, length;
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    length = strlen(item->valuestring);
    if (!length || length >= capacity) return false;
    for (i=0U;i<length;++i) {
        unsigned char c=(unsigned char)item->valuestring[i];
        if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') ||
              c=='_' || c=='-' || c=='.' || c==':')) return false;
    }
    memcpy(out,item->valuestring,length+1U);
    return true;
}

static bool duplicate_keys(const cJSON *item)
{
    const cJSON *a,*b;
    for (a=item->child;a;a=a->next) {
        if (cJSON_IsObject(item))
            for (b=a->next;b;b=b->next)
                if (a->string && b->string && !strcmp(a->string,b->string)) return true;
        if ((cJSON_IsObject(a) || cJSON_IsArray(a)) && duplicate_keys(a)) return true;
    }
    return false;
}

static bool json_has_nul_escape(const char *text)
{
    size_t i;
    for (i=0U;text[i];++i) {
        if (text[i]=='\\' && text[i+1U]) {
            if (!strncmp(text+i,"\\u0000",6U)) return true;
            ++i; /* escaped slash is not a unicode escape */
        }
    }
    return false;
}

static int parse_contacts(const char *text, bool wx, tirtc_contact_t *output,
                          uint8_t *count, bool *limited)
{
    cJSON *root;
    const cJSON *code,*data,*list,*item;
    unsigned skipped=0U,valid=0U;
    *count=0U;
    *limited=false;
    if (json_has_nul_escape(text)) return CONTACT_ERROR_JSON;
    root=demo_json_parse_with_length_opts(text,strlen(text)+1U,NULL,true);
    if (!root) return CONTACT_ERROR_JSON;
    code=cJSON_GetObjectItemCaseSensitive(root,"code");
    data=cJSON_GetObjectItemCaseSensitive(root,"data");
    list=cJSON_IsObject(data)?cJSON_GetObjectItemCaseSensitive(data,"contacts"):NULL;
    if (!cJSON_IsObject(root) || duplicate_keys(root) || !cJSON_IsNumber(code) ||
        (code->valuedouble!=0.0 && (wx || code->valuedouble!=200.0)) || !cJSON_IsArray(list)) {
        cJSON_Delete(root);return CONTACT_ERROR_JSON;
    }
    cJSON_ArrayForEach(item,list) {
        const cJSON *type,*name=NULL;
        tirtc_contact_t contact;
        const char *keys[]={"remark","device_name","name","nickname"};
        unsigned i;
        memset(&contact,0,sizeof(contact));
        type=cJSON_GetObjectItemCaseSensitive(item,"type");
        if (!cJSON_IsObject(item)) { ++skipped;continue; }
        if (!wx) {
            if (cJSON_IsString(type) && type->valuestring && !strcmp(type->valuestring,"voip")) continue;
            if (!cJSON_IsString(type) || !type->valuestring || strcmp(type->valuestring,"device")) {
                ++skipped;continue;
            }
        }
        contact.wechat=wx;
        if (!copy_id(item,wx?"wx_open_id":"device_id",contact.id,wx?sizeof(contact.id):64U) ||
            (wx && (!copy_id(item,"wx_app_id",contact.wx_app_id,sizeof(contact.wx_app_id)) ||
                    !copy_id(item,"wx_model_id",contact.wx_model_id,sizeof(contact.wx_model_id))))) {
            ++skipped;continue;
        }
        for (i=0U;i<sizeof(keys)/sizeof(keys[0]);++i) {
            name=cJSON_GetObjectItemCaseSensitive(item,keys[i]);
            if (cJSON_IsString(name) && name->valuestring && name->valuestring[0]) break;
        }
        if (i<sizeof(keys)/sizeof(keys[0])) {
            if (!display_name(contact.name,sizeof(contact.name),name->valuestring)) { ++skipped;continue; }
        } else {
            (void)display_name(contact.name,sizeof(contact.name),contact.wechat?"微信联系人":contact.id);
        }
        contact.online=!contact.wechat && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item,"online"));
        for (i=0U;i<*count;++i)
            if (!strcmp(output[i].id,contact.id)) break;
        if (i<*count) { cJSON_Delete(root);return CONTACT_ERROR_JSON; }
        ++valid;
        if (*count>=TIRTC_UI_CONTACT_MAX) { *limited=true;continue; }
        output[(*count)++]=contact;
    }
    cJSON_Delete(root);
    /* A wholly malformed list must not masquerade as the server deleting all
     * contacts. Valid empty lists remain authoritative. */
    if (skipped && !valid) return CONTACT_ERROR_JSON;
    if (skipped) *limited=true;
    return 0;
}

void tirtc_contacts_refresh(void)
{
    liot_rtos_enter_critical();
    if (++s_request==0U) ++s_request;
    liot_rtos_exit_critical();
}

static void contacts_mqtt(demo_formal_mqtt_message_kind_e kind,const char *type,
    const char *channel,const cJSON *payload,void *user)
{
    (void)kind;(void)channel;(void)payload;(void)user;
    if (type && (!strcmp(type,"callers_update") || !strcmp(type,"contacts_update")))
        tirtc_contacts_refresh();
}

void tirtc_contacts_publish(void)
{
    tirtc_ui_contacts_t ui;
    demo_binding_service_context_t context;
    demo_binding_snapshot_t binding;
    uint32_t now;
    bool fresh[2],limited;
    int error;
    memset(&ui,0,sizeof(ui));
    liot_rtos_enter_critical();
    now=liot_rtos_get_running_time();
    fresh[0]=scope_fresh_locked(false,now);fresh[1]=scope_fresh_locked(true,now);
    ui.known=s_scope_cache[0].known||s_scope_cache[1].known;
    ui.stale=s_stale||(!fresh[0]&&!fresh[1]);
    ui.loading=s_loading&&ui.stale;
    context=s_context;limited=s_limited;error=s_error;
    for (unsigned i=0U;i<s_count;++i) {
        const tirtc_contact_t *entry=&s_contacts[i];
        if(!ui.stale&&!fresh[entry->wechat])continue;
        tirtc_ui_contact_t *display=&ui.contacts[ui.count++];
        memcpy(display->id,entry->id,sizeof(display->id));
        memcpy(display->name,entry->name,sizeof(display->name));
        display->online=entry->online;display->wechat=entry->wechat;
    }
    liot_rtos_exit_critical();
    demo_binding_get_snapshot(&binding);
    if (binding.state!=DEMO_BIND_BOUND || !context.credential_epoch ||
        binding.credential_epoch!=context.credential_epoch) {
        ui.count=0U;ui.stale=true;ui.loading=false;
        memset(ui.contacts,0,sizeof(ui.contacts));
        (void)snprintf(ui.message,sizeof(ui.message),"请先绑定设备");
    } else if (!demo_binding_service_context_current(&context)) {
        ui.stale=true;ui.loading=false;
        (void)snprintf(ui.message,sizeof(ui.message),"等待网络连接");
    } else if (ui.loading) {
        (void)snprintf(ui.message,sizeof(ui.message),"正在同步联系人");
    } else if (ui.stale && (error || ui.known)) {
        (void)snprintf(ui.message,sizeof(ui.message),"同步失败，请重试");
    } else if (limited) {
        (void)snprintf(ui.message,sizeof(ui.message),"部分联系人未显示");
    } else if (ui.known && !ui.count) {
        (void)snprintf(ui.message,sizeof(ui.message),"暂无联系人");
    } else {
        (void)snprintf(ui.message,sizeof(ui.message),"通讯录已更新");
    }
    if (ui.stale) for(unsigned i=0U;i<ui.count;++i) ui.contacts[i].online=false;
    (void)tirtc_ui_publish_contacts(&ui);
}

bool tirtc_contacts_lookup(const char *id,bool wechat,tirtc_contact_t *out)
{
    unsigned i;
    size_t length;
    bool found=false;
    if (!out) return false;
    memset(out,0,sizeof(*out));
    if (!id || !id[0]) return false;
    for(length=0U;length<sizeof(out->id)&&id[length];++length){}
    if(length==sizeof(out->id))return false;
    liot_rtos_enter_critical();
    if (!s_stale && scope_fresh_locked(wechat,liot_rtos_get_running_time())) {
        for(i=0U;i<s_count;++i) {
            if(s_contacts[i].wechat==wechat && !strcmp(s_contacts[i].id,id)) {
                *out=s_contacts[i];found=true;break;
            }
        }
    }
    liot_rtos_exit_critical();
    if (!found || !demo_binding_service_context_current(&out->context)) {
        memset(out,0,sizeof(*out));return false;
    }
    return true;
}

uint32_t tirtc_contacts_resolve_begin(const char *target, tirtc_contact_scope_t scope)
{
    demo_binding_service_context_t context;
    uint32_t ticket=0; size_t n;
    if (!target || !(n=strlen(target)) || n>=sizeof(s_resolve.target) ||
        (unsigned)scope>TIRTC_CONTACT_DEVICE || !demo_binding_service_context(&context)) return 0;
    liot_rtos_enter_critical();
    if (s_started && !s_resolve.active) {
        if (!++s_resolve_sequence) ++s_resolve_sequence;
        if (!++s_request) ++s_request;
        memset(&s_resolve,0,sizeof(s_resolve));
        s_resolve.active=true; s_resolve.ticket=ticket=s_resolve_sequence;
        s_resolve.refresh=s_request; s_resolve.context=context; s_resolve.scope=scope;
        s_resolve.started=liot_rtos_get_running_time();
        memcpy(s_resolve.target,target,n+1);
    }
    liot_rtos_exit_critical(); return ticket;
}
void tirtc_contacts_resolve_cancel(uint32_t ticket)
{
    liot_rtos_enter_critical();
    if (ticket && s_resolve.ticket==ticket) memset(&s_resolve,0,sizeof(s_resolve));
    liot_rtos_exit_critical();
}
tirtc_contact_result_t tirtc_contacts_resolve_poll(uint32_t ticket, tirtc_contact_t *out)
{
    demo_binding_service_context_t context;
    tirtc_contact_result_t result=TIRTC_RESOLVE_UNAVAILABLE;
    bool valid;
    if (!out) return result;
    memset(out,0,sizeof(*out));
    liot_rtos_enter_critical();
    valid=ticket && s_resolve.active && s_resolve.ticket==ticket;
    context=s_resolve.context;
    if (valid) { result=s_resolve.result; if(result==TIRTC_RESOLVE_FOUND)*out=s_resolve.contact; }
    liot_rtos_exit_critical();
    if (!valid || !demo_binding_service_context_current(&context)) {
        memset(out,0,sizeof(*out)); return TIRTC_RESOLVE_UNAVAILABLE;
    }
    return result;
}
/* Match original full server names, never the clipped/sanitized UI label.
 * Parsing was already validated by parse_contacts. An incomplete list is
 * insufficient to prove uniqueness, so it cannot authorize a voice call. */
static tirtc_contact_result_t resolve_response(const char *text, const char *target,
    bool wx, uint8_t count, bool limited, tirtc_contact_t *out)
{
    cJSON *root; const cJSON *list,*item; unsigned matches=0,i,k;
    static const char *const keys[]={"remark","device_name","name","nickname"};
    if (limited) return TIRTC_RESOLVE_UNAVAILABLE;
    root=demo_json_parse_with_length_opts(text,strlen(text)+1U,NULL,true);
    if (!root) return TIRTC_RESOLVE_UNAVAILABLE;
    list=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"data"),"contacts");
    cJSON_ArrayForEach(item,list) {
        const cJSON *id=cJSON_GetObjectItemCaseSensitive(item,wx?"wx_open_id":"device_id");
        const cJSON *type=cJSON_GetObjectItemCaseSensitive(item,"type"), *name=NULL;
        if (!cJSON_IsString(id) || !id->valuestring ||
            (!wx&&(!cJSON_IsString(type)||!type->valuestring||strcmp(type->valuestring,"device")))) continue;
        for (k=0;k<4;k++) { name=cJSON_GetObjectItemCaseSensitive(item,keys[k]);
            if(cJSON_IsString(name) && name->valuestring[0]) break; }
        if (strcmp(target,id->valuestring) && (k==4 || strcmp(target,name->valuestring))) continue;
        for(i=0U;i<count;i++) if(!strcmp(s_work[wx][i].id,id->valuestring)) {
            *out=s_work[wx][i]; ++matches; break;
        }
    }
    cJSON_Delete(root);
    if (matches>1) return TIRTC_RESOLVE_AMBIGUOUS;
    if (!matches) return TIRTC_RESOLVE_NOT_FOUND;
    return out->wechat || out->online ? TIRTC_RESOLVE_FOUND : TIRTC_RESOLVE_OFFLINE;
}
static int fetch_contacts(bool wx,const demo_binding_service_context_t *context,
    uint8_t *count,bool *limited,const char *target,tirtc_contact_t *resolved,
    tirtc_contact_result_t *outcome,uint32_t budget)
{
    int status=0,result;
    *count=0U;
    if(!demo_binding_service_context_current(context)) return CONTACT_ERROR_STALE;
    if(budget<100U) return CONTACT_ERROR_HTTP;
    memset(s_response,0,sizeof(s_response));
    result=demo_binding_service_request_guarded(wx?DEMO_BIND_SERVICE_VOIP:DEMO_BIND_SERVICE_CALL,
        DEMO_BIND_HTTP_GET,wx?"/v1/voip/device/contacts":"/v1/call/device/contacts",
        NULL,s_response,sizeof(s_response),&status,budget,context);
    if (!result&&status!=200) result=(status==401||status==403)?CONTACT_ERROR_AUTH:CONTACT_ERROR_HTTP;
    if (!result&&s_response[sizeof(s_response)-1U]!='\0') result=CONTACT_ERROR_JSON;
    if (!result) result=parse_contacts(s_response,wx,s_work[wx],count,limited);
    if (result) *count=0U;
    else if (target) *outcome=resolve_response(s_response,target,wx,*count,*limited,resolved);
    memset(s_response,0,sizeof(s_response));
    return result;
}

/* A query across both services must establish uniqueness across both. Each
 * individual scope can still resolve when the other service is unavailable. */
static tirtc_contact_result_t merge_resolution(tirtc_contact_result_t dev,tirtc_contact_result_t wx)
{
    if(dev==TIRTC_RESOLVE_UNAVAILABLE||wx==TIRTC_RESOLVE_UNAVAILABLE)
        return TIRTC_RESOLVE_UNAVAILABLE;
    if(dev==TIRTC_RESOLVE_NOT_FOUND) return wx;
    if(wx==TIRTC_RESOLVE_NOT_FOUND) return dev;
    return TIRTC_RESOLVE_AMBIGUOUS;
}

static void finish_resolution(uint32_t ticket,const demo_binding_service_context_t *context,
    tirtc_contact_result_t result,tirtc_contact_t *resolved)
{
    if(!ticket)return;
    if(!demo_binding_service_context_current(context))result=TIRTC_RESOLVE_UNAVAILABLE;
    liot_rtos_enter_critical();
    if(s_resolve.active&&s_resolve.ticket==ticket){
        s_resolve.result=result;
        if(result==TIRTC_RESOLVE_FOUND){
            resolved->context=*context;s_resolve.contact=*resolved;
        }
    }
    liot_rtos_exit_critical();
}

/* Retain the Pro UI limit while allowing both contact types to appear even
 * when one service alone returned a full list. */
static bool publish_contacts_locked(const demo_binding_service_context_t *context,
    const int result[2],uint8_t count[2],const uint32_t sampled[2],uint32_t now)
{
    for(unsigned wx=0U;wx<2U;++wx){
        if(!result[wx]){
            s_scope_cache[wx].known=true;
            s_scope_cache[wx].sample_ms=sampled[wx];
        }else{
            count[wx]=0U;
            if(!scope_fresh_locked(wx!=0U,now))continue;
            /* Preserve only this scope's previously visible manual contacts. */
            for(unsigned i=0U;i<s_count;++i)
                if(s_contacts[i].wechat==(wx!=0U))
                    s_work[wx][count[wx]++]=s_contacts[i];
        }
    }
    unsigned keep[2]={count[0],count[1]};
    const unsigned total=keep[0]+keep[1];
    if(total>TIRTC_UI_CONTACT_MAX){
        if(result[0])keep[1]=TIRTC_UI_CONTACT_MAX-keep[0];
        else if(result[1])keep[0]=TIRTC_UI_CONTACT_MAX-keep[1];
        else {
            if(keep[0]>TIRTC_UI_CONTACT_MAX/2U)keep[0]=TIRTC_UI_CONTACT_MAX/2U;
            if(keep[1]>TIRTC_UI_CONTACT_MAX-keep[0])keep[1]=TIRTC_UI_CONTACT_MAX-keep[0];
            keep[0]=TIRTC_UI_CONTACT_MAX-keep[1];
        }
    }
    s_count=0U;
    for(unsigned wx=0U;wx<2U;++wx)
        for(unsigned i=0U;i<keep[wx];++i){
            s_contacts[s_count]=s_work[wx][i];
            s_contacts[s_count++].context=*context;
        }
    if(s_count<TIRTC_UI_CONTACT_MAX)
        memset(s_contacts+s_count,0,sizeof(s_contacts[0])*(TIRTC_UI_CONTACT_MAX-s_count));
    return total>TIRTC_UI_CONTACT_MAX;
}

static void contacts_step(void)
{
    demo_binding_service_context_t context;
    demo_binding_snapshot_t binding;
    uint32_t now=liot_rtos_get_running_time(),request,resolve_ticket=0,resolve_started=0;
    char resolve_target[257]; tirtc_contact_scope_t resolve_scope=TIRTC_CONTACT_ANY;
    tirtc_contact_t resolved[2];
    tirtc_contact_result_t matches[2]={TIRTC_RESOLVE_UNAVAILABLE,TIRTC_RESOLVE_UNAVAILABLE};
    tirtc_contact_result_t resolve_result;
    uint8_t count[2]={0U,0U};
    uint32_t sampled[2];
    bool limited[2]={false,false},changed,current;
    int result[2],error;
    unsigned first_source=0U;
    if (!demo_binding_service_context(&context)) {
        demo_binding_get_snapshot(&binding);
        liot_rtos_enter_critical();
        changed=binding.state!=DEMO_BIND_BOUND || binding.credential_epoch!=s_context.credential_epoch;
        if(changed){clear_contacts_locked();s_context.credential_epoch=0U;}
        s_stale=true;s_loading=false;
        liot_rtos_exit_critical();
        tirtc_contacts_publish();return;
    }
    liot_rtos_enter_critical();
    changed=!same_context(&context,&s_context);
    if(changed){clear_contacts_locked();s_stale=true;
        s_context=context;s_scheduled=false;s_attempted=false;s_retry_ms=CONTACT_RETRY_MIN_MS;}
    request=s_request;
    liot_rtos_exit_critical();
    if(s_attempted && now-s_attempt_at<1000U)return;
    if(s_scheduled && request==s_completed_request && (int32_t)(now-s_next_at)<0)return;
    s_attempted=true;s_attempt_at=now;
    liot_rtos_enter_critical();s_loading=true;
    if(s_resolve.active && s_resolve.result==TIRTC_RESOLVE_PENDING &&
       (int32_t)(request-s_resolve.refresh)>=0 && same_context(&context,&s_resolve.context)) {
        resolve_ticket=s_resolve.ticket;resolve_scope=s_resolve.scope;
        resolve_started=s_resolve.started;first_source=resolve_scope==TIRTC_CONTACT_WECHAT;
        memcpy(resolve_target,s_resolve.target,sizeof(resolve_target));
    }
    liot_rtos_exit_critical();
    tirtc_contacts_publish();
    for(unsigned i=0U;i<2U;++i){
        if(i!=0U){
            /* A lookup requested during the first fetch precedes the next
             * background request. The in-flight request still runs to completion. */
            bool lookup_waiting;
            liot_rtos_enter_critical();
            lookup_waiting=s_resolve.active&&s_resolve.result==TIRTC_RESOLVE_PENDING&&
                (int32_t)(request-s_resolve.refresh)<0&&same_context(&context,&s_resolve.context);
            liot_rtos_exit_critical();
            if(lookup_waiting)return;
        }
        unsigned wx=first_source^i;
        bool resolve=resolve_ticket&&(resolve_scope==TIRTC_CONTACT_ANY||i==0U);
        uint32_t elapsed=liot_rtos_get_running_time()-resolve_started;
        uint32_t budget=resolve?(elapsed<CONTACT_RESOLVE_MS?CONTACT_RESOLVE_MS-elapsed:0U):CONTACT_HTTP_MS;
        result[wx]=fetch_contacts(wx!=0U,&context,&count[wx],&limited[wx],
            resolve?resolve_target:NULL,&resolved[wx],&matches[wx],budget);
        sampled[wx]=liot_rtos_get_running_time();
        if(result[wx]==CONTACT_ERROR_AUTH){
            /* An explicit denial invalidates this scope before another HTTP
             * request or an AI lookup preempts the remaining refresh. */
            liot_rtos_enter_critical();
            invalidate_scope_locked(wx!=0U);
            liot_rtos_exit_critical();
            tirtc_contacts_publish();
        }
        if(resolve&&resolve_scope!=TIRTC_CONTACT_ANY){
            finish_resolution(resolve_ticket,&context,matches[wx],&resolved[wx]);
            resolve_ticket=0U;
        }
    }
    if(resolve_ticket){
        resolve_result=merge_resolution(matches[0],matches[1]);
        finish_resolution(resolve_ticket,&context,resolve_result,&resolved[matches[0]==TIRTC_RESOLVE_NOT_FOUND]);
    }
    current=demo_binding_service_context_current(&context);
    error=!current?CONTACT_ERROR_STALE:(result[0]?result[0]:result[1]);
    now=liot_rtos_get_running_time();
    liot_rtos_enter_critical();
    s_loading=false;s_completed_request=request;s_error=error;
    s_stale=!current||(result[0]&&result[1]);
    if(!s_stale){
        bool capped=publish_contacts_locked(&context,result,count,sampled,now);
        s_limited=error||limited[0]||limited[1]||capped;
    }
    liot_rtos_exit_critical();
    s_scheduled=true;
    if(!error){s_next_at=now+CONTACT_REFRESH_MS;s_retry_ms=CONTACT_RETRY_MIN_MS;}
    else{s_next_at=now+s_retry_ms;s_retry_ms=s_retry_ms<CONTACT_RETRY_MAX_MS/2U?s_retry_ms*2U:CONTACT_RETRY_MAX_MS;}
    if((error&&error!=s_reported_error)||(!error&&TIRTC_ENABLE_DEBUG_LOG)){
        liot_trace("[contacts] sync dev=%d wx=%d count=%u\r\n",result[0],result[1],
            (unsigned)s_count);
    }
    s_reported_error=error;
    tirtc_contacts_publish();
}

static void contacts_worker(void *argument)
{
    (void)argument;
    liot_trace("[contacts] worker started\r\n");
    for(;;){contacts_step();liot_rtos_task_sleep_ms(500U);}
}

int tirtc_contacts_start(void)
{
    int result;
    liot_rtos_enter_critical();
    if(s_started || s_starting){liot_rtos_exit_critical();return 0;}
    s_starting=true;liot_rtos_exit_critical();
    if(!s_handler_registered){
        result=demo_formal_mqtt_register_handler(contacts_mqtt,NULL);
        if(result)goto fail;
        s_handler_registered=true;
    }
    result=liot_rtos_task_create(&s_task,16384U,8U,"contacts",contacts_worker,NULL,0);
    if(result!=LIOT_OSI_SUCCESS)goto fail;
    liot_rtos_enter_critical();s_started=true;s_starting=false;liot_rtos_exit_critical();
    return 0;
fail:
    liot_rtos_enter_critical();s_starting=false;liot_rtos_exit_critical();
    liot_trace("[contacts] start failed result=%d\r\n",result);
    return -1;
}

int tirtc_contacts_action(const tirtc_ui_action_t *action)
{
    if(!action)return -1;
    if(action->type==TIRTC_ACTION_REFRESH_CONTACTS ||
        (action->type==TIRTC_ACTION_ENTER_PAGE && action->page==TIRTC_PAGE_CONTACTS)){
        tirtc_contacts_refresh();return 0;
    }
    return -1;
}
