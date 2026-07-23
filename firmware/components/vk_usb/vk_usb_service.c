#include "vk_usb.h"

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#include "vk_usb_capabilities.h"
#include "vk_usb_json.h"
#include "vk_usb_asset_protocol.h"
#include "vk_usb_led_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define VK_USB_IDENTITY_MAX_BYTES 64U
#define VK_USB_PROVIDER_QUIESCE_SPINS 3000U
#define VK_USB_INPUT_LIFECYCLE_DEADLINE_MS 3250U
#define VK_USB_LIFECYCLE_PENDING ((esp_err_t)0x7f01)

typedef enum { DISPATCH_CONTINUE = 0, DISPATCH_RESET_EPOCH } dispatch_action_t;
typedef enum { TX_BUTTON = 0, TX_AUDIO } tx_kind_t;
typedef enum { TERMINAL_NONE = 0, TERMINAL_CONTROL, TERMINAL_AUDIO, TERMINAL_INPUT } terminal_kind_t;
typedef struct {
    tx_kind_t kind;
    union {
        vk_usb_button_event_t button;
        struct { uint32_t session_id; uint32_t sequence; uint8_t flags; uint16_t length; uint8_t payload[VK_USB_AUDIO_MAX_PAYLOAD_BYTES]; } audio;
    } value;
} tx_item_t;

struct vk_usb_service {
    vk_usb_transport_ops_t transport;
    vk_usb_service_policy_t policy;
    char hardware[VK_USB_IDENTITY_MAX_BYTES];
    char firmware_version[VK_USB_IDENTITY_MAX_BYTES];
    char device_id[VK_USB_IDENTITY_MAX_BYTES];
    uint8_t receive[VK_USB_MAX_FRAME_BYTES];
    uint8_t dispatch[VK_USB_MAX_FRAME_BYTES];
    uint8_t transmit[VK_USB_MAX_FRAME_BYTES];
    char capability_json[VK_USB_MAX_JSON_BYTES + 1U];
    char protocol_json[VK_USB_MAX_JSON_BYTES + 1U];
    size_t receive_count;
    uint32_t epoch;
    uint64_t lease_deadline_ms;
    bool installed;
    bool stop_requested;
    bool epoch_active;
    vk_usb_capability_provider_registration_t capability_provider;
    vk_usb_capability_snapshot_t capability_snapshot;
    uint32_t capability_epoch;
    uint32_t capability_generation;
    uint32_t capability_provider_in_flight;
    bool capability_provider_admission_open;
    bool capability_valid;
    vk_usb_asset_handler_registration_t asset_handler;
    struct {
        bool authorized;
        bool callback_in_flight;
        uint32_t epoch;
        uint32_t generation;
        uint32_t transfer_id;
        uint32_t total_bytes;
        uint32_t next_offset;
        uint16_t chunk_bytes;
        vk_usb_asset_kind_t kind;
        char sha256[65];
    } asset_transfer;
    vk_usb_screen_handler_registration_t screen_handler;
    vk_usb_widget_handler_registration_t widget_handler;
    vk_usb_update_handler_registration_t update_handler;
    vk_usb_led_handler_registration_t led_handler;
    struct {
        bool pending;
        uint32_t epoch;
        uint32_t generation;
        uint32_t request_id;
        bool enabled;
        uint8_t brightness;
        bool completed;
        char cached_response[256];
        size_t cached_response_length;
    } led_config;
    uint32_t protocol_callbacks_in_flight;
    bool protocol_callback_admission_open;
    vk_usb_input_handler_registration_t input_handler;
    vk_usb_input_lifecycle_registration_t input_lifecycle;
    vk_usb_led_lifecycle_registration_t led_lifecycle;
    struct {
        bool pending;
        bool begin_pending;
        bool ack_published;
        uint8_t expected_ack_mask;
        uint8_t received_ack_mask;
        bool ack_tainted;
        bool tainted;
        uint32_t token;
        uint32_t generation;
        uint32_t old_epoch;
        uint32_t proposed_epoch;
        vk_usb_input_lifecycle_kind_t kind;
        uint64_t deadline_ms;
        uint64_t ack_time_ms;
        vk_usb_input_lifecycle_ack_t ack;
    } input_lifecycle_state;
    vk_usb_json_document_t json_document;
    vk_usb_asset_chunk_t asset_chunk;
    tx_item_t tx_queue[VK_USB_TYPED_TX_QUEUE_CAPACITY];
    tx_item_t tx_scratch[VK_USB_TYPED_TX_QUEUE_CAPACITY];
    size_t tx_head;
    size_t tx_count;
    terminal_kind_t terminal;
    uint32_t terminal_audio_session_id;
    bool audio_truncated;
    uint32_t truncated_audio_session_id;
    bool poll_owner_active;
    bool tx_in_flight;
    tx_item_t in_flight_item;
#ifdef VK_USB_NATIVE_TEST
    size_t consume_bytes;
    size_t parser_steps;
    vk_usb_before_tx_commit_hook_t before_tx_commit_hook;
    void *before_tx_commit_context;
#endif
};

static void state_lock(vk_usb_service_t *s) { if (s->transport.state_lock != NULL) s->transport.state_lock(s->transport.context); }
static void state_unlock(vk_usb_service_t *s) { if (s->transport.state_unlock != NULL) s->transport.state_unlock(s->transport.context); }

static bool copy_identity(char *destination, const char *source)
{
    if (source == NULL) return false;
    size_t length = strnlen(source, VK_USB_IDENTITY_MAX_BYTES);
    if (length == 0U || length >= VK_USB_IDENTITY_MAX_BYTES) return false;
    memcpy(destination, source, length + 1U);
    return true;
}

typedef struct { vk_usb_json_document_t *document; uint16_t node; } json_object_t;

static uint16_t member_node(const json_object_t *object, const char *key)
{
    return vk_usb_json_object_find(object->document, object->node, key);
}

static bool string_copy_value(const json_object_t *object, const char *key, char *value, size_t capacity)
{
    uint16_t node = member_node(object, key);
    return node != VK_USB_JSON_NO_NODE && vk_usb_json_string_copy(object->document, node, value, capacity) == VK_USB_JSON_OK;
}

static bool exact_keys(const json_object_t *object, const char *const *keys, size_t count)
{
    return vk_usb_json_object_exact_keys(object->document, object->node, keys, count);
}

static esp_err_t write_all(vk_usb_service_t*s,const uint8_t*b,size_t n){size_t o=0;while(o<n){int w=s->transport.write(s->transport.context,b+o,n-o,VK_USB_TX_TIMEOUT_MS);if(w<=0||(size_t)w>n-o)return ESP_FAIL;o+=(size_t)w;}return ESP_OK;}
static esp_err_t send_json_bytes(vk_usb_service_t*s,const char*j,size_t n){if(!j||n>VK_USB_MAX_JSON_BYTES)return ESP_ERR_INVALID_SIZE;s->transmit[0]=1;s->transmit[1]=0x10;s->transmit[2]=(uint8_t)n;s->transmit[3]=(uint8_t)(n>>8);memcpy(s->transmit+4,j,n);return write_all(s,s->transmit,n+4);}
static esp_err_t send_json(vk_usb_service_t*s,const char*j){return j?send_json_bytes(s,j,strlen(j)):ESP_ERR_INVALID_ARG;}
static esp_err_t send_error(vk_usb_service_t*s,const char*op,const char*code){char j[320];int n=snprintf(j,sizeof(j),"{\"event\":\"vk_error\",\"operation\":\"%s\",\"code\":\"%s\"}",op,code);return n<0||(size_t)n>=sizeof(j)?ESP_ERR_INVALID_SIZE:send_json(s,j);}

static void clear_asset_transfer_locked(vk_usb_service_t *s)
{
    s->asset_transfer.authorized = false;
    s->asset_transfer.epoch = 0U;
    s->asset_transfer.generation = 0U;
    s->asset_transfer.transfer_id = 0U;
    s->asset_transfer.total_bytes = 0U;
    s->asset_transfer.next_offset = 0U;
    s->asset_transfer.chunk_bytes = 0U;
    s->asset_transfer.kind = VK_USB_ASSET_KIND_IMAGE;
    memset(s->asset_transfer.sha256, 0, sizeof(s->asset_transfer.sha256));
}
static void clear_capability_locked(vk_usb_service_t *s){s->capability_snapshot=(vk_usb_capability_snapshot_t){0};s->capability_epoch=0U;s->capability_valid=false;clear_asset_transfer_locked(s);memset(&s->led_config,0,sizeof(s->led_config));}
static esp_err_t refresh_capability(vk_usb_service_t *s,uint32_t epoch)
{
    vk_usb_capability_provider_registration_t provider;vk_usb_asset_handler_registration_t asset;
    bool policy;
    state_lock(s);
    if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=epoch||
       !s->capability_provider_admission_open){state_unlock(s);return ESP_ERR_INVALID_STATE;}
    provider=s->capability_provider;asset=s->asset_handler;policy=s->policy.update_boot_policy_enabled;
    if(provider.get_snapshot!=NULL)++s->capability_provider_in_flight;
    state_unlock(s);
    vk_usb_capability_snapshot_t candidate={0};
    esp_err_t result=provider.get_snapshot==NULL?ESP_OK:provider.get_snapshot(provider.context,epoch,&candidate);
    bool asset_ready=asset.handle_command!=NULL&&asset.handle_chunk!=NULL;
    if(result!=ESP_OK||!vk_usb_capability_snapshot_validate(&candidate,policy)||
       (candidate.assets.state==VK_USB_CAPABILITY_AVAILABLE&&!asset_ready)||
       candidate.update.state==VK_USB_CAPABILITY_AVAILABLE)candidate=(vk_usb_capability_snapshot_t){0};
    state_lock(s);
    if(provider.get_snapshot!=NULL&&s->capability_provider_in_flight!=0U)--s->capability_provider_in_flight;
    if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=epoch||
       !s->capability_provider_admission_open){clear_capability_locked(s);state_unlock(s);return ESP_ERR_INVALID_STATE;}
    clear_asset_transfer_locked(s);s->capability_snapshot=candidate;s->capability_epoch=epoch;++s->capability_generation;if(s->capability_generation==0U)++s->capability_generation;s->capability_valid=true;
    state_unlock(s);
    return ESP_OK;
}
static esp_err_t send_device_info(vk_usb_service_t*s)
{
    uint32_t epoch;
    state_lock(s);
    epoch=s->epoch;
    int n=snprintf(s->protocol_json,sizeof(s->protocol_json),"{\"event\":\"device_info\",\"hardware\":\"%s\",\"firmware_version\":\"%s\",\"buttons\":[\"k1\",\"k2\",\"k3\",\"k4\"],\"ui_states\":[\"ready\",\"thinking\",\"listening\",\"processing\",\"error\"],\"interaction_modes\":[\"hold_to_talk\",\"click_to_talk\"],\"device_id\":\"%s\",\"provisioned\":true,\"replacement_protocol\":1}",s->hardware,s->firmware_version,s->device_id);
    state_unlock(s);
    if(n<0||(size_t)n>=sizeof(s->protocol_json))return ESP_ERR_INVALID_SIZE;
    esp_err_t e=send_json_bytes(s,s->protocol_json,(size_t)n);if(e!=ESP_OK)return e;
    e=refresh_capability(s,epoch);if(e!=ESP_OK)return e;
    vk_usb_capability_snapshot_t snapshot;state_lock(s);if(!s->capability_valid||s->capability_epoch!=epoch){state_unlock(s);return ESP_ERR_INVALID_STATE;}snapshot=s->capability_snapshot;state_unlock(s);
    size_t length=0;e=vk_usb_capability_snapshot_encode(&snapshot,s->capability_json,sizeof(s->capability_json),&length);if(e!=ESP_OK)return e;
    return send_json_bytes(s,s->capability_json,length);
}

static uint64_t add_deadline(uint64_t now,uint64_t delta){return now>UINT64_MAX-delta?UINT64_MAX:now+delta;}
static uint64_t deadline(uint64_t now){return add_deadline(now,VK_USB_LEASE_MS);}
static uint32_t next_nonzero(uint32_t value){++value;return value==0U?1U:value;}
static void clear_queue_locked(vk_usb_service_t*s){s->tx_head=0;s->tx_count=0;s->tx_in_flight=false;}
static void begin_epoch_locked(vk_usb_service_t*s,uint32_t epoch,uint64_t now){s->epoch=epoch;s->epoch_active=true;s->protocol_callback_admission_open=true;s->lease_deadline_ms=deadline(now);clear_capability_locked(s);clear_queue_locked(s);s->terminal=TERMINAL_NONE;s->terminal_audio_session_id=0;s->audio_truncated=false;s->truncated_audio_session_id=0;}

static bool input_lifecycle_publish(void *context,const vk_usb_input_lifecycle_ack_t *ack)
{
    uintptr_t tagged=(uintptr_t)context;
    vk_usb_service_t *s=(vk_usb_service_t *)(tagged&~(uintptr_t)3U);
    uint8_t participant=(uint8_t)(tagged&3U);
    if(s==NULL||participant==0U||ack==NULL)return false;
    uint64_t published=s->transport.now_ms(s->transport.context);
    state_lock(s);
    bool accepted=s->input_lifecycle_state.pending&&
        (s->input_lifecycle_state.expected_ack_mask&participant)!=0U&&
        (s->input_lifecycle_state.received_ack_mask&participant)==0U&&
        ack->token==s->input_lifecycle_state.token&&
        ack->lifecycle_generation==s->input_lifecycle_state.generation&&
        published<s->input_lifecycle_state.deadline_ms;
    if(accepted){
        s->input_lifecycle_state.ack=*ack;
        s->input_lifecycle_state.ack_time_ms=published;
        s->input_lifecycle_state.ack_tainted|=ack->result!=VK_USB_INPUT_LIFECYCLE_QUIESCENT;
        s->input_lifecycle_state.received_ack_mask|=participant;
        s->input_lifecycle_state.ack_published=
            s->input_lifecycle_state.received_ack_mask==s->input_lifecycle_state.expected_ack_mask;
    }
    state_unlock(s);
    return accepted;
}

static esp_err_t start_input_lifecycle(vk_usb_service_t *s,vk_usb_input_lifecycle_kind_t kind,
                                       uint32_t old_epoch,uint32_t proposed_epoch,uint64_t now)
{
    vk_usb_input_lifecycle_registration_t registration;
    vk_usb_led_lifecycle_registration_t led_registration;
    vk_usb_input_lifecycle_request_t request;
    vk_usb_input_lifecycle_sink_t input_sink={.publish=input_lifecycle_publish,
        .context=(void *)((uintptr_t)s|1U)};
    vk_usb_input_lifecycle_sink_t led_sink={.publish=input_lifecycle_publish,
        .context=(void *)((uintptr_t)s|2U)};
    state_lock(s);
    if(s->input_lifecycle_state.tainted){state_unlock(s);return ESP_ERR_INVALID_STATE;}
    if(s->input_lifecycle_state.pending){
        bool same=s->input_lifecycle_state.kind==kind&&s->input_lifecycle_state.old_epoch==old_epoch&&
                  s->input_lifecycle_state.proposed_epoch==proposed_epoch;
        if(same){state_unlock(s);return ESP_OK;}
        if(kind!=VK_USB_INPUT_LIFECYCLE_STOPPING||s->input_lifecycle_state.old_epoch!=old_epoch){state_unlock(s);return ESP_ERR_INVALID_STATE;}
    }
    s->epoch_active=false;s->protocol_callback_admission_open=false;clear_capability_locked(s);clear_queue_locked(s);s->receive_count=0;
    s->input_lifecycle_state.pending=true;s->input_lifecycle_state.begin_pending=true;
    s->input_lifecycle_state.ack_published=false;
    s->input_lifecycle_state.received_ack_mask=0U;
    s->input_lifecycle_state.ack_tainted=false;
    s->input_lifecycle_state.token=next_nonzero(s->input_lifecycle_state.token);
    s->input_lifecycle_state.generation=next_nonzero(s->input_lifecycle_state.generation);
    s->input_lifecycle_state.old_epoch=old_epoch;s->input_lifecycle_state.proposed_epoch=proposed_epoch;
    s->input_lifecycle_state.kind=kind;s->input_lifecycle_state.deadline_ms=add_deadline(now,VK_USB_INPUT_LIFECYCLE_DEADLINE_MS);
    request=(vk_usb_input_lifecycle_request_t){.kind=kind,.token=s->input_lifecycle_state.token,
        .old_epoch=old_epoch,.proposed_epoch=proposed_epoch,.lifecycle_generation=s->input_lifecycle_state.generation,
        .absolute_deadline_ms=s->input_lifecycle_state.deadline_ms};
    registration=s->input_lifecycle;
    led_registration=s->led_lifecycle;
    s->input_lifecycle_state.expected_ack_mask=(uint8_t)((registration.begin!=NULL?1U:0U)|(led_registration.begin!=NULL?2U:0U));
    if(s->input_lifecycle_state.expected_ack_mask==0U)s->input_lifecycle_state.expected_ack_mask=1U;
    s->protocol_callbacks_in_flight+=(uint32_t)((registration.begin!=NULL?1U:0U)+(led_registration.begin!=NULL?1U:0U));
    state_unlock(s);
    if(registration.begin==NULL&&led_registration.begin==NULL){
        vk_usb_input_lifecycle_ack_t ack={.token=request.token,.lifecycle_generation=request.lifecycle_generation,.result=VK_USB_INPUT_LIFECYCLE_QUIESCENT};
        (void)input_lifecycle_publish(input_sink.context,&ack);
        state_lock(s);s->input_lifecycle_state.begin_pending=false;state_unlock(s);return ESP_OK;
    }
    vk_usb_input_lifecycle_begin_result_t result=VK_USB_INPUT_LIFECYCLE_ACCEPTED;
    if(registration.begin!=NULL)result=registration.begin(registration.context,&request,&input_sink);
    vk_usb_input_lifecycle_begin_result_t led_result=VK_USB_INPUT_LIFECYCLE_ACCEPTED;
    if(led_registration.begin!=NULL)led_result=led_registration.begin(led_registration.context,&request,&led_sink);
    state_lock(s);
    uint32_t callbacks=(uint32_t)((registration.begin!=NULL?1U:0U)+(led_registration.begin!=NULL?1U:0U));
    s->protocol_callbacks_in_flight=s->protocol_callbacks_in_flight>=callbacks?s->protocol_callbacks_in_flight-callbacks:0U;
    bool current=s->input_lifecycle_state.pending&&s->input_lifecycle_state.token==request.token&&
        s->input_lifecycle_state.generation==request.lifecycle_generation;
    if(current){s->input_lifecycle_state.begin_pending=false;if(result==VK_USB_INPUT_LIFECYCLE_TAINTED||led_result==VK_USB_INPUT_LIFECYCLE_TAINTED)s->input_lifecycle_state.tainted=true;}
    state_unlock(s);
    return result==VK_USB_INPUT_LIFECYCLE_ACCEPTED&&led_result==VK_USB_INPUT_LIFECYCLE_ACCEPTED?ESP_OK:ESP_ERR_INVALID_STATE;
}

esp_err_t vk_usb_service_process_lifecycle(vk_usb_service_t *s)
{
    if(s==NULL)return ESP_ERR_INVALID_ARG;
    uint64_t now=s->transport.now_ms(s->transport.context);
    state_lock(s);
    if(!s->input_lifecycle_state.pending){bool tainted=s->input_lifecycle_state.tainted;state_unlock(s);return tainted?ESP_ERR_INVALID_STATE:ESP_OK;}
    bool timed_out=now>=s->input_lifecycle_state.deadline_ms;
    bool complete=s->input_lifecycle_state.ack_published||timed_out||s->input_lifecycle_state.tainted;
    if(!complete){state_unlock(s);return VK_USB_LIFECYCLE_PENDING;}
    bool quiescent=s->input_lifecycle_state.ack_published&&
        !s->input_lifecycle_state.ack_tainted&&
        s->input_lifecycle_state.ack_time_ms<s->input_lifecycle_state.deadline_ms;
    vk_usb_input_lifecycle_kind_t kind=s->input_lifecycle_state.kind;
    uint32_t proposed=s->input_lifecycle_state.proposed_epoch;
    s->input_lifecycle_state.pending=false;s->input_lifecycle_state.ack_published=false;
    if(!quiescent){s->input_lifecycle_state.tainted=true;s->epoch_active=false;s->protocol_callback_admission_open=false;state_unlock(s);return ESP_ERR_TIMEOUT;}
    if(kind==VK_USB_INPUT_LIFECYCLE_NEW_EPOCH){begin_epoch_locked(s,proposed,now);}
    state_unlock(s);
    return ESP_OK;
}

bool vk_usb_service_is_tainted(vk_usb_service_t *s){if(!s)return true;state_lock(s);bool v=s->input_lifecycle_state.tainted;state_unlock(s);return v;}

static esp_err_t send_asset_event_now(vk_usb_service_t *s, const vk_usb_asset_event_t *event)
{
    size_t length = 0U;
    esp_err_t result = vk_usb_asset_event_encode(event, s->protocol_json, sizeof(s->protocol_json), &length);
    return result == ESP_OK ? send_json_bytes(s, s->protocol_json, length) : result;
}

static esp_err_t send_asset_error(vk_usb_service_t *s, const char *code, uint32_t transfer_id,
                                  bool has_next_offset, uint32_t next_offset)
{
    vk_usb_protocol_error_t error = {
        .operation = VK_USB_ERROR_ASSET,
        .code = code,
        .has_transfer_id = transfer_id != 0U,
        .transfer_id = transfer_id,
        .has_next_offset = has_next_offset,
        .next_offset = next_offset,
    };
    size_t length = 0U;
    esp_err_t result = vk_usb_protocol_error_encode(&error, s->protocol_json, sizeof(s->protocol_json), &length);
    return result == ESP_OK ? send_json_bytes(s, s->protocol_json, length) : result;
}

static esp_err_t send_asset_backend_error(vk_usb_service_t *service,
                                          const vk_usb_asset_handler_registration_t *handler,
                                          const char *phase,
                                          esp_err_t backend_result, uint32_t transfer_id,
                                          bool has_next_offset, uint32_t next_offset)
{
    char detail[96] = {0};
    if (handler != NULL && handler->error_detail != NULL) {
        (void)handler->error_detail(handler->context, detail, sizeof(detail));
    }
    char message[160];
    int written = detail[0] == '\0'
        ? snprintf(message, sizeof(message), "phase=%s;esp_err=0x%08" PRIx32,
                   phase, (uint32_t)backend_result)
        : snprintf(message, sizeof(message), "phase=%s;%s", phase, detail);
    if (written <= 0 || (size_t)written >= sizeof(message)) {
        return send_asset_error(service, "write_failed", transfer_id,
                                has_next_offset, next_offset);
    }
    vk_usb_protocol_error_t error = {
        .operation = VK_USB_ERROR_ASSET,
        .code = backend_result == ESP_ERR_NOT_SUPPORTED ? "unavailable" : "write_failed",
        .message = message,
        .has_transfer_id = transfer_id != 0U,
        .transfer_id = transfer_id,
        .has_next_offset = has_next_offset,
        .next_offset = next_offset,
    };
    size_t length = 0U;
    esp_err_t result = vk_usb_protocol_error_encode(
        &error, service->protocol_json, sizeof(service->protocol_json), &length);
    return result == ESP_OK
        ? send_json_bytes(service, service->protocol_json, length)
        : result;
}

static esp_err_t send_screen_error(
    vk_usb_service_t *service,
    const vk_usb_screen_handler_registration_t *handler,
    const char *code, const char *phase, esp_err_t cause)
{
    char detail[48] = {0};
    if (handler != NULL && handler->error_detail != NULL) {
        (void)handler->error_detail(
            handler->context, detail, sizeof(detail));
    }
    char message[96];
    int written = detail[0] == '\0'
        ? snprintf(message, sizeof(message),
                   "phase=%s;esp_err=0x%08" PRIx32,
                   phase, (uint32_t)cause)
        : snprintf(message, sizeof(message),
                   "phase=%s;%s;esp_err=0x%08" PRIx32,
                   phase, detail, (uint32_t)cause);
    if (written <= 0 || (size_t)written >= sizeof(message)) {
        return send_error(service, "screen", code);
    }
    vk_usb_protocol_error_t error = {
        .operation = VK_USB_ERROR_SCREEN,
        .code = code,
        .message = message,
    };
    size_t length = 0U;
    esp_err_t result = vk_usb_protocol_error_encode(
        &error, service->protocol_json, sizeof(service->protocol_json), &length);
    return result == ESP_OK
        ? send_json_bytes(service, service->protocol_json, length)
        : result;
}

static const char *asset_command_phase(vk_usb_asset_command_kind_t kind)
{
    switch (kind) {
    case VK_USB_ASSET_BEGIN: return "begin";
    case VK_USB_ASSET_QUERY: return "query";
    case VK_USB_ASSET_END: return "end";
    case VK_USB_ASSET_ABORT: return "abort";
    case VK_USB_ASSET_LIST: return "list";
    case VK_USB_ASSET_DELETE: return "delete";
    default: return "command";
    }
}

static esp_err_t dispatch_asset(vk_usb_service_t*s,const json_object_t*o,const char*event)
{
    (void)event;
    vk_usb_asset_handler_registration_t handler;vk_usb_assets_capability_t assets;uint32_t epoch,generation;bool authorized;
    state_lock(s);handler=s->asset_handler;assets=s->capability_snapshot.assets;epoch=s->epoch;generation=s->capability_generation;
    authorized=s->installed&&!s->stop_requested&&s->epoch_active&&s->protocol_callback_admission_open&&s->capability_valid&&
               s->capability_epoch==epoch&&assets.state==VK_USB_CAPABILITY_AVAILABLE&&handler.handle_command!=NULL;
    state_unlock(s);if(!authorized)return send_error(s,"asset","unsupported");
    vk_usb_asset_command_t command;
    esp_err_t decoded=vk_usb_asset_command_decode(o->document,o->node,&assets,epoch,generation,&command);
    if(decoded!=ESP_OK)return send_error(s,"asset",decoded==ESP_ERR_NOT_SUPPORTED?"unsupported":"invalid_request");
    bool ready=assets.storage_state==VK_USB_STORAGE_READY;
    if(command.kind==VK_USB_ASSET_BEGIN){uint32_t maximum=assets.upload_max_bytes<assets.max_asset_bytes?assets.upload_max_bytes:assets.max_asset_bytes;if(!ready||maximum==0U||command.total_bytes>maximum)return send_error(s,"asset","unavailable");}
    else if(command.kind==VK_USB_ASSET_LIST||command.kind==VK_USB_ASSET_DELETE){if(!ready)return send_error(s,"asset","unavailable");}
    else if(command.kind!=VK_USB_ASSET_QUERY&&command.kind!=VK_USB_ASSET_END&&command.kind!=VK_USB_ASSET_ABORT)return send_error(s,"asset","unsupported");

    state_lock(s);
    if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=epoch||!s->capability_valid||
       s->capability_generation!=generation||!s->protocol_callback_admission_open){state_unlock(s);return send_asset_error(s,"wrong_epoch",command.transfer_id,false,0U);}
    bool has_transfer=s->asset_transfer.authorized&&s->asset_transfer.epoch==epoch&&s->asset_transfer.generation==generation;
    if(command.kind==VK_USB_ASSET_BEGIN&&has_transfer){
        bool duplicate=s->asset_transfer.transfer_id==command.transfer_id&&s->asset_transfer.total_bytes==command.total_bytes&&
            s->asset_transfer.kind==command.asset_kind_value&&strcmp(s->asset_transfer.sha256,command.sha256)==0;
        vk_usb_asset_event_t existing={.kind=VK_USB_ASSET_EVENT_READY,.transfer_id=s->asset_transfer.transfer_id,
            .total_bytes=s->asset_transfer.total_bytes,.next_offset=s->asset_transfer.next_offset,.chunk_bytes=s->asset_transfer.chunk_bytes,
            .asset_kind=s->asset_transfer.kind};memcpy(existing.sha256,s->asset_transfer.sha256,sizeof(existing.sha256));
        state_unlock(s);return duplicate?send_asset_event_now(s,&existing):send_asset_error(s,"busy",command.transfer_id,false,0U);
    }
    if((command.kind==VK_USB_ASSET_QUERY||command.kind==VK_USB_ASSET_END||command.kind==VK_USB_ASSET_ABORT)&&
       (!has_transfer||s->asset_transfer.transfer_id!=command.transfer_id)){
        if(command.kind==VK_USB_ASSET_QUERY&&handler.get_transfer_state!=NULL){
            ++s->protocol_callbacks_in_flight;state_unlock(s);
            vk_usb_asset_command_t restored={0};uint32_t restored_offset=0U;
            esp_err_t restore=handler.get_transfer_state(handler.context,command.transfer_id,epoch,generation,&restored,&restored_offset);
            state_lock(s);if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;
            bool still_current=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&
                s->capability_generation==generation&&s->protocol_callback_admission_open;
            if(!still_current){state_unlock(s);return ESP_ERR_INVALID_STATE;}
            if(restore==ESP_OK&&restored.transfer_id==command.transfer_id&&restored.total_bytes!=0U&&
               restored_offset<=restored.total_bytes){
                s->asset_transfer.authorized=true;s->asset_transfer.epoch=epoch;s->asset_transfer.generation=generation;
                s->asset_transfer.transfer_id=restored.transfer_id;s->asset_transfer.total_bytes=restored.total_bytes;
                s->asset_transfer.next_offset=restored_offset;s->asset_transfer.chunk_bytes=handler.chunk_bytes<assets.chunk_bytes?handler.chunk_bytes:assets.chunk_bytes;
                s->asset_transfer.kind=restored.asset_kind_value;memcpy(s->asset_transfer.sha256,restored.sha256,sizeof(s->asset_transfer.sha256));
                vk_usb_asset_event_t response={.kind=VK_USB_ASSET_EVENT_READY,.transfer_id=restored.transfer_id,
                    .total_bytes=restored.total_bytes,.next_offset=restored_offset,.chunk_bytes=s->asset_transfer.chunk_bytes,
                    .asset_kind=restored.asset_kind_value};memcpy(response.sha256,restored.sha256,sizeof(response.sha256));
                state_unlock(s);return send_asset_event_now(s,&response);
            }
        }
        state_unlock(s);
        if(command.kind==VK_USB_ASSET_ABORT){vk_usb_asset_event_t aborted={.kind=VK_USB_ASSET_EVENT_ABORTED,.transfer_id=command.transfer_id};return send_asset_event_now(s,&aborted);}
        return send_asset_error(s,"not_found",command.transfer_id,false,0U);
    }
    if(has_transfer&&s->asset_transfer.callback_in_flight){state_unlock(s);return send_asset_error(s,"busy",command.transfer_id,false,0U);}
    if(command.kind==VK_USB_ASSET_END&&
       (s->asset_transfer.total_bytes!=command.total_bytes||s->asset_transfer.kind!=command.asset_kind_value||
        strcmp(s->asset_transfer.sha256,command.sha256)!=0)){
        uint32_t next=s->asset_transfer.next_offset;state_unlock(s);return send_asset_error(s,"conflict",command.transfer_id,true,next);
    }
    if(command.kind==VK_USB_ASSET_QUERY&&has_transfer){
        vk_usb_asset_event_t response={.kind=VK_USB_ASSET_EVENT_READY,.transfer_id=s->asset_transfer.transfer_id,
            .total_bytes=s->asset_transfer.total_bytes,.next_offset=s->asset_transfer.next_offset,.chunk_bytes=s->asset_transfer.chunk_bytes,
            .asset_kind=s->asset_transfer.kind};memcpy(response.sha256,s->asset_transfer.sha256,sizeof(response.sha256));
        state_unlock(s);return send_asset_event_now(s,&response);
    }
    if(command.kind==VK_USB_ASSET_END&&s->asset_transfer.next_offset!=s->asset_transfer.total_bytes){uint32_t next=s->asset_transfer.next_offset;state_unlock(s);return send_asset_error(s,"incomplete",command.transfer_id,true,next);}
    ++s->protocol_callbacks_in_flight;s->asset_transfer.callback_in_flight=true;state_unlock(s);
    esp_err_t result=handler.handle_command(handler.context,&command);
    vk_usb_asset_command_t durable_tuple={0};uint32_t durable_offset=0U;
    if(result==ESP_OK&&command.kind==VK_USB_ASSET_BEGIN&&handler.get_transfer_state!=NULL){
        result=handler.get_transfer_state(handler.context,command.transfer_id,epoch,generation,&durable_tuple,&durable_offset);
        if(result==ESP_OK&&(durable_tuple.transfer_id!=command.transfer_id||durable_tuple.total_bytes!=command.total_bytes||
           durable_tuple.asset_kind_value!=command.asset_kind_value||strcmp(durable_tuple.sha256,command.sha256)!=0||
           durable_offset>command.total_bytes))result=ESP_ERR_INVALID_STATE;
    }
    state_lock(s);s->asset_transfer.callback_in_flight=false;if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;
    bool current=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&s->capability_generation==generation&&s->protocol_callback_admission_open;
    if(!current){state_unlock(s);return ESP_ERR_INVALID_STATE;}
    if(result!=ESP_OK){state_unlock(s);return send_asset_backend_error(s,&handler,asset_command_phase(command.kind),result,command.transfer_id,false,0U);}
    vk_usb_asset_event_t response={0};
    if(command.kind==VK_USB_ASSET_BEGIN){
        s->asset_transfer.authorized=true;s->asset_transfer.epoch=epoch;s->asset_transfer.generation=generation;
        s->asset_transfer.transfer_id=command.transfer_id;s->asset_transfer.total_bytes=command.total_bytes;s->asset_transfer.next_offset=durable_offset;
        s->asset_transfer.chunk_bytes=handler.chunk_bytes<assets.chunk_bytes?handler.chunk_bytes:assets.chunk_bytes;
        s->asset_transfer.kind=command.asset_kind_value;memcpy(s->asset_transfer.sha256,command.sha256,sizeof(s->asset_transfer.sha256));
        response.kind=VK_USB_ASSET_EVENT_READY;response.transfer_id=command.transfer_id;response.total_bytes=command.total_bytes;
        response.next_offset=durable_offset;response.chunk_bytes=s->asset_transfer.chunk_bytes;response.asset_kind=command.asset_kind_value;memcpy(response.sha256,command.sha256,sizeof(response.sha256));
    }else if(command.kind==VK_USB_ASSET_END){
        response.kind=VK_USB_ASSET_EVENT_STORED;response.transfer_id=s->asset_transfer.transfer_id;response.total_bytes=s->asset_transfer.total_bytes;
        response.asset_kind=s->asset_transfer.kind;memcpy(response.sha256,s->asset_transfer.sha256,sizeof(response.sha256));clear_asset_transfer_locked(s);
    }else if(command.kind==VK_USB_ASSET_ABORT){response.kind=VK_USB_ASSET_EVENT_ABORTED;response.transfer_id=command.transfer_id;clear_asset_transfer_locked(s);}
    state_unlock(s);
    if(command.kind==VK_USB_ASSET_LIST||command.kind==VK_USB_ASSET_DELETE){
        if(handler.build_event==NULL)return ESP_OK;
        vk_usb_asset_event_t backend_event={0};esp_err_t built=handler.build_event(handler.context,&command,&backend_event);
        return built==ESP_OK?send_asset_event_now(s,&backend_event):send_asset_error(s,"internal",command.transfer_id,false,0U);
    }
    return send_asset_event_now(s,&response);
}
static esp_err_t dispatch_storage(vk_usb_service_t*s,const json_object_t*o,const char*event)
{
    (void)o;
    /* Format requires a fresh, separately verified-erased current-epoch token. */
    return strcmp(event,"vk_storage_format")==0?send_error(s,"storage","not_erased"):send_error(s,"storage","invalid_request");
}

static esp_err_t dispatch_screen(vk_usb_service_t*s,const json_object_t*o,const char*event)
{
    (void)event;
    vk_usb_screen_handler_registration_t handler;vk_usb_screen_capability_t screen;uint32_t epoch,generation;bool authorized;
    state_lock(s);handler=s->screen_handler;screen=s->capability_snapshot.screen;epoch=s->epoch;generation=s->capability_generation;
    authorized=s->installed&&!s->stop_requested&&s->epoch_active&&s->protocol_callback_admission_open&&s->capability_valid&&
               s->capability_epoch==epoch&&screen.state==VK_USB_CAPABILITY_AVAILABLE&&handler.handle_command!=NULL;
    state_unlock(s);if(!authorized)return send_error(s,"screen","unavailable");
    vk_usb_screen_command_t command;esp_err_t decoded=vk_usb_screen_command_decode(o->document,o->node,&screen,428U,142U,epoch,generation,&command);
    if(decoded!=ESP_OK)return send_screen_error(s,&handler,decoded==ESP_ERR_NOT_SUPPORTED?"unavailable":"invalid_request","decode",decoded);
    state_lock(s);if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=epoch||!s->capability_valid||
       s->capability_generation!=generation||!s->protocol_callback_admission_open){state_unlock(s);return send_error(s,"screen","wrong_epoch");}
    ++s->protocol_callbacks_in_flight;state_unlock(s);
    vk_usb_screen_event_t response={0};esp_err_t result=handler.handle_command(handler.context,&command,&response);
    state_lock(s);if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;
    bool current=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&s->capability_generation==generation;
    state_unlock(s);if(!current)return ESP_ERR_INVALID_STATE;
    return result==ESP_OK?vk_usb_service_send_screen_event_for_epoch(s,epoch,generation,&response):send_screen_error(s,&handler,result==ESP_ERR_NOT_SUPPORTED?"unavailable":"invalid_request","commit",result);
}

static esp_err_t dispatch_widget(vk_usb_service_t*s,const json_object_t*o)
{
    vk_usb_widget_handler_registration_t handler;vk_usb_screen_capability_t screen;uint32_t epoch,generation;bool authorized;
    state_lock(s);handler=s->widget_handler;screen=s->capability_snapshot.screen;epoch=s->epoch;generation=s->capability_generation;
    authorized=s->installed&&!s->stop_requested&&s->epoch_active&&s->protocol_callback_admission_open&&s->capability_valid&&
               s->capability_epoch==epoch&&screen.state==VK_USB_CAPABILITY_AVAILABLE&&handler.handle_command!=NULL;
    state_unlock(s);if(!authorized)return send_error(s,"widget","invalid_state");
    vk_usb_widget_command_t command={0};esp_err_t decoded=vk_usb_widget_command_decode(o->document,o->node,&screen,epoch,generation,&command);
    if(decoded!=ESP_OK)return send_error(s,"widget","invalid_request");
    state_lock(s);if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=epoch||!s->capability_valid||
       s->capability_generation!=generation||!s->protocol_callback_admission_open){state_unlock(s);return send_error(s,"widget","wrong_epoch");}
    ++s->protocol_callbacks_in_flight;state_unlock(s);
    vk_usb_widget_event_t response={0};esp_err_t result=handler.handle_command(handler.context,&command,&response);
    state_lock(s);if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;
    bool current=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&s->capability_generation==generation;
    state_unlock(s);if(!current)return ESP_ERR_INVALID_STATE;
    if(result!=ESP_OK){const char*code=result==ESP_ERR_NOT_FOUND?"not_found":result==ESP_ERR_INVALID_STATE?"invalid_state":result==ESP_ERR_INVALID_SIZE?"too_large":"type_mismatch";vk_usb_protocol_error_t error={.operation=VK_USB_ERROR_WIDGET,.code=code};return vk_usb_service_send_protocol_error_for_epoch(s,epoch,generation,&error);}
    return vk_usb_service_send_widget_event_for_epoch(s,epoch,generation,&response);
}

static esp_err_t send_led_error_now(vk_usb_service_t*s,uint32_t request_id,vk_usb_led_error_t code)
{
    vk_usb_led_error_event_t error={.request_id=request_id,.code=code};size_t length=0;
    esp_err_t result=vk_usb_led_error_encode(&error,s->protocol_json,sizeof(s->protocol_json),&length);
    return result==ESP_OK?send_json_bytes(s,s->protocol_json,length):result;
}
static esp_err_t dispatch_led(vk_usb_service_t*s,const json_object_t*o)
{
    vk_usb_led_handler_registration_t handler;vk_usb_led_capability_t capability;
    uint32_t epoch,generation;bool authorized;
    state_lock(s);handler=s->led_handler;capability=s->capability_snapshot.led;epoch=s->epoch;
    generation=s->capability_generation;
    authorized=s->installed&&!s->stop_requested&&s->epoch_active&&s->protocol_callback_admission_open&&
        s->capability_valid&&s->capability_epoch==epoch;
    state_unlock(s);
    vk_usb_led_command_t command={0};
    esp_err_t decoded=vk_usb_led_command_decode(o->document,o->node,&capability,&command);
    uint32_t recoverable_id=0U;uint16_t request_node=member_node(o,"request_id");
    if(request_node!=VK_USB_JSON_NO_NODE)(void)vk_usb_json_uint32(o->document,request_node,UINT32_MAX,true,&recoverable_id);
    if(!authorized)return send_led_error_now(s,recoverable_id,VK_USB_LED_ERROR_WRONG_EPOCH);
    if(decoded==ESP_ERR_NOT_SUPPORTED){
        if(command.kind==VK_USB_LED_QUERY){vk_usb_led_state_event_t state={.source=VK_USB_LED_STATE_QUERY,
            .request_id=command.request_id,.available=false,.unavailable_reason=capability.unavailable_reason};
            size_t length=0;esp_err_t result=vk_usb_led_state_encode(&state,s->protocol_json,sizeof(s->protocol_json),&length);
            return result==ESP_OK?send_json_bytes(s,s->protocol_json,length):result;}
        return send_led_error_now(s,command.request_id,VK_USB_LED_ERROR_UNAVAILABLE);
    }
    if(decoded!=ESP_OK)return send_led_error_now(s,recoverable_id,VK_USB_LED_ERROR_INVALID_REQUEST);
    command.expected_epoch=epoch;command.snapshot_generation=generation;
    if(handler.handle_command==NULL){
        if(command.kind==VK_USB_LED_QUERY){vk_usb_led_state_event_t state={.source=VK_USB_LED_STATE_QUERY,
            .request_id=command.request_id,.available=false,.unavailable_reason=VK_USB_LED_REASON_CALIBRATION_REQUIRED};
            size_t length=0;esp_err_t result=vk_usb_led_state_encode(&state,s->protocol_json,sizeof(s->protocol_json),&length);
            return result==ESP_OK?send_json_bytes(s,s->protocol_json,length):result;}
        return send_led_error_now(s,command.request_id,VK_USB_LED_ERROR_UNAVAILABLE);
    }
    state_lock(s);
    if(command.kind==VK_USB_LED_CONFIG){
        if(s->led_config.pending){bool same=s->led_config.request_id==command.request_id&&
            s->led_config.enabled==command.enabled&&s->led_config.brightness==command.brightness&&
            s->led_config.epoch==epoch&&s->led_config.generation==generation;
            if(same&&s->led_config.completed){size_t length=s->led_config.cached_response_length;
                memcpy(s->protocol_json,s->led_config.cached_response,length);state_unlock(s);
                return send_json_bytes(s,s->protocol_json,length);}
            state_unlock(s);return send_led_error_now(s,command.request_id,same?VK_USB_LED_ERROR_BUSY:VK_USB_LED_ERROR_INVALID_REQUEST);}
        s->led_config.pending=true;s->led_config.completed=false;s->led_config.epoch=epoch;
        s->led_config.generation=generation;s->led_config.request_id=command.request_id;
        s->led_config.enabled=command.enabled;s->led_config.brightness=command.brightness;
    }
    ++s->protocol_callbacks_in_flight;state_unlock(s);
    vk_usb_led_state_event_t response={0};esp_err_t result=handler.handle_command(handler.context,&command,&response);
    state_lock(s);if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;
    bool current=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&
        s->capability_generation==generation&&s->protocol_callback_admission_open;
    if(!current){memset(&s->led_config,0,sizeof(s->led_config));state_unlock(s);return ESP_ERR_INVALID_STATE;}
    state_unlock(s);
    if(result!=ESP_OK){state_lock(s);if(command.kind==VK_USB_LED_CONFIG)memset(&s->led_config,0,sizeof(s->led_config));state_unlock(s);
        return send_led_error_now(s,command.request_id,result==ESP_ERR_NO_MEM?VK_USB_LED_ERROR_QUEUE_OVERFLOW:
            (result==ESP_ERR_NOT_SUPPORTED?VK_USB_LED_ERROR_UNAVAILABLE:VK_USB_LED_ERROR_HARDWARE_FAILED));}
    if(response.request_id!=command.request_id||response.source!=(command.kind==VK_USB_LED_QUERY?VK_USB_LED_STATE_QUERY:VK_USB_LED_STATE_APPLIED)){
        state_lock(s);if(command.kind==VK_USB_LED_CONFIG)memset(&s->led_config,0,sizeof(s->led_config));state_unlock(s);
        return send_led_error_now(s,command.request_id,VK_USB_LED_ERROR_INVALID_REQUEST);}
    size_t length=0;result=vk_usb_led_state_encode(&response,s->protocol_json,sizeof(s->protocol_json),&length);
    if(result==ESP_OK&&command.kind==VK_USB_LED_CONFIG){state_lock(s);if(length<=sizeof(s->led_config.cached_response)){
        memcpy(s->led_config.cached_response,s->protocol_json,length);s->led_config.cached_response_length=length;s->led_config.completed=true;}state_unlock(s);}
    return result==ESP_OK?send_json_bytes(s,s->protocol_json,length):result;
}
static esp_err_t dispatch_update(vk_usb_service_t*s,const json_object_t*o,const char*event)
{
    (void)o;(void)event;
    /* Update availability is intentionally unrepresentable until the reviewed
     * bootloader/update owner supplies an evidence-bound target tuple. */
    return send_error(s,"update","unsupported");
}

static esp_err_t dispatch_json(vk_usb_service_t*s,const uint8_t*b,size_t n,dispatch_action_t*a)
{
    *a=DISPATCH_CONTINUE;vk_usb_json_status_t json_status=vk_usb_json_parse(&s->json_document,b,n);uint16_t root=vk_usb_json_root(&s->json_document);if(json_status!=VK_USB_JSON_OK||root==VK_USB_JSON_NO_NODE||vk_usb_json_kind(&s->json_document,root)!=VK_USB_JSON_OBJECT)return send_error(s,"json","invalid_request");json_object_t o={.document=&s->json_document,.node=root};char event_storage[VK_USB_JSON_MAX_STRING_BYTES+1U];if(!string_copy_value(&o,"event",event_storage,sizeof(event_storage)))return send_error(s,"json","invalid_request");const char*event=event_storage;
    if(strcmp(event,"transport")==0){
        const char*const k[]={"event","kind"};char kind[8];
        if(!string_copy_value(&o,"kind",kind,sizeof(kind))||!exact_keys(&o,k,2)||strcmp(kind,"usb")!=0)return send_error(s,"transport","invalid_request");
        uint64_t now=s->transport.now_ms(s->transport.context);uint32_t old_epoch,proposed;
        state_lock(s);if(!s->installed||s->stop_requested||s->input_lifecycle_state.tainted){state_unlock(s);return ESP_ERR_INVALID_STATE;}
        if(s->input_lifecycle_state.pending){state_unlock(s);return send_error(s,"transport","busy");}
        old_epoch=s->epoch_active?s->epoch:0U;proposed=next_nonzero(s->epoch);state_unlock(s);
        esp_err_t lifecycle=start_input_lifecycle(s,VK_USB_INPUT_LIFECYCLE_NEW_EPOCH,old_epoch,proposed,now);
        if(lifecycle!=ESP_OK)return send_error(s,"transport","busy");
        lifecycle=vk_usb_service_process_lifecycle(s);
        if(lifecycle==ESP_OK||lifecycle==VK_USB_LIFECYCLE_PENDING){*a=DISPATCH_RESET_EPOCH;return ESP_OK;}
        return lifecycle;
    }
    state_lock(s);bool active=s->installed&&!s->stop_requested&&s->epoch_active;state_unlock(s);if(!active)return send_error(s,event,"wrong_epoch");
    if(strcmp(event,"ping")==0){const char*const k[]={"event"};if(!exact_keys(&o,k,1))return send_error(s,"ping","invalid_request");uint64_t now=s->transport.now_ms(s->transport.context);state_lock(s);if(s->epoch_active)s->lease_deadline_ms=deadline(now);state_unlock(s);return ESP_OK;}
    if(strcmp(event,"get_device_info")==0){const char*const k[]={"event"};return exact_keys(&o,k,1)?send_device_info(s):send_error(s,"get_device_info","invalid_request");}
    if(strcmp(event,"ui_state")==0){const char*const k[]={"event","state","text"};char state[16];bool has_state=string_copy_value(&o,"state",state,sizeof(state));uint16_t text_node=member_node(&o,"text");bool valid=has_state&&(strcmp(state,"ready")==0||strcmp(state,"thinking")==0||strcmp(state,"listening")==0||strcmp(state,"processing")==0||strcmp(state,"error")==0);return exact_keys(&o,k,3)&&valid&&text_node!=VK_USB_JSON_NO_NODE&&vk_usb_json_kind(o.document,text_node)==VK_USB_JSON_STRING?ESP_OK:send_error(s,"ui_state","invalid_request");}
    if(strcmp(event,"interaction_mode")==0||strcmp(event,"voice_key")==0){
        vk_usb_input_command_t c={.expected_epoch=0};
        if(strcmp(event,"interaction_mode")==0){const char*const k[]={"event","mode"};char v[20];if(!string_copy_value(&o,"mode",v,sizeof(v))||!exact_keys(&o,k,2))return send_error(s,"input","invalid_request");c.kind=VK_USB_INPUT_COMMAND_MODE;if(strcmp(v,"hold_to_talk")==0)c.mode=VK_USB_INPUT_MODE_HOLD_TO_TALK;else if(strcmp(v,"click_to_talk")==0)c.mode=VK_USB_INPUT_MODE_CLICK_TO_TALK;else return send_error(s,"input","invalid_request");}
        else{const char*const k[]={"event","key"};char v[5];if(!string_copy_value(&o,"key",v,sizeof(v))||!exact_keys(&o,k,2))return send_error(s,"input","invalid_request");c.kind=VK_USB_INPUT_COMMAND_VOICE_KEY;if(strcmp(v,"none")==0)c.key=VK_USB_KEY_NONE;else if(strlen(v)==2U&&v[0]=='k'&&v[1]>='1'&&v[1]<='4')c.key=(vk_usb_key_t)(v[1]-'1');else return send_error(s,"input","invalid_request");}
        vk_usb_input_handler_registration_t h;state_lock(s);bool admitted=s->installed&&!s->stop_requested&&s->epoch_active&&s->protocol_callback_admission_open&&s->input_handler.handle_command!=NULL;h=s->input_handler;c.expected_epoch=s->epoch;if(admitted)++s->protocol_callbacks_in_flight;state_unlock(s);if(!admitted)return send_error(s,"input",h.handle_command==NULL?"unsupported":"busy");
        esp_err_t e=h.handle_command(h.context,&c);state_lock(s);if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;state_unlock(s);return e==ESP_OK?ESP_OK:send_error(s,"input",e==ESP_ERR_INVALID_STATE?"busy":"invalid_request");
    }
    if(strcmp(event,"voice_gain")==0)return send_error(s,"input","unsupported");
    if(strncmp(event,"vk_asset_",9)==0)return dispatch_asset(s,&o,event);
    if(strncmp(event,"vk_storage_",11)==0)return dispatch_storage(s,&o,event);
    if(strncmp(event,"vk_screen_",10)==0)return dispatch_screen(s,&o,event);
    if(strcmp(event,"vk_widget_update")==0)return dispatch_widget(s,&o);
    if(strcmp(event,"vk_led_query")==0||strcmp(event,"vk_led_config")==0)return dispatch_led(s,&o);
    if(strncmp(event,"vk_update_",10)==0)return dispatch_update(s,&o,event);
    return send_error(s,"command","unsupported");
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static esp_err_t dispatch_chunk(vk_usb_service_t*s,uint8_t type,const uint8_t*b,size_t n)
{
    if(type!=VK_USB_FRAME_TYPE_ASSET_CHUNK)return send_error(s,"update","unsupported");
    if(b==NULL||n<9U||n>8U+VK_USB_ASSET_CHUNK_MAX_BYTES)return send_asset_error(s,"bad_size",0U,false,0U);
    uint32_t transfer_id=read_u32_le(b),offset=read_u32_le(b+4U);size_t payload=n-8U;
    vk_usb_asset_handler_registration_t handler;vk_usb_asset_chunk_t *chunk=&s->asset_chunk;memset(chunk,0,sizeof(*chunk));uint32_t epoch,generation,total,next;bool allowed;
    state_lock(s);handler=s->asset_handler;epoch=s->epoch;generation=s->capability_generation;total=s->asset_transfer.total_bytes;next=s->asset_transfer.next_offset;
    allowed=s->installed&&!s->stop_requested&&s->epoch_active&&s->protocol_callback_admission_open&&s->capability_valid&&
        s->asset_transfer.authorized&&!s->asset_transfer.callback_in_flight&&s->asset_transfer.epoch==epoch&&
        s->asset_transfer.generation==generation&&handler.handle_chunk!=NULL;
    if(!allowed){state_unlock(s);return send_asset_error(s,"unavailable",transfer_id,false,0U);}
    if(transfer_id==0U||transfer_id!=s->asset_transfer.transfer_id){state_unlock(s);return send_asset_error(s,"not_found",transfer_id,false,0U);}
    if(offset!=next){state_unlock(s);return send_asset_error(s,"bad_offset",transfer_id,true,next);}
    if(payload==0U||payload>s->asset_transfer.chunk_bytes||offset>total||payload>(size_t)(total-offset)){
        state_unlock(s);return send_asset_error(s,"bad_size",transfer_id,true,next);
    }
    chunk->expected_epoch=epoch;chunk->snapshot_generation=generation;chunk->transfer_id=transfer_id;chunk->offset=offset;chunk->payload_length=(uint16_t)payload;memcpy(chunk->payload,b+8U,payload);
    ++s->protocol_callbacks_in_flight;s->asset_transfer.callback_in_flight=true;state_unlock(s);
    esp_err_t result=handler.handle_chunk(handler.context,chunk);
    state_lock(s);s->asset_transfer.callback_in_flight=false;if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;
    bool current=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&
        s->capability_generation==generation&&s->asset_transfer.authorized&&s->asset_transfer.transfer_id==transfer_id&&
        s->asset_transfer.next_offset==offset&&s->protocol_callback_admission_open;
    if(!current){state_unlock(s);return ESP_ERR_INVALID_STATE;}
    if(result!=ESP_OK){next=s->asset_transfer.next_offset;state_unlock(s);return send_asset_backend_error(s,&handler,"chunk",result,transfer_id,true,next);}
    s->asset_transfer.next_offset=offset+(uint32_t)payload;next=s->asset_transfer.next_offset;state_unlock(s);
    vk_usb_asset_event_t progress={.kind=VK_USB_ASSET_EVENT_PROGRESS,.transfer_id=transfer_id,.next_offset=next};
    return send_asset_event_now(s,&progress);
}
static esp_err_t consume(vk_usb_service_t*s,const uint8_t*b,size_t n)
{
    for(size_t index=0;index<n;++index){state_lock(s);
#ifdef VK_USB_NATIVE_TEST
        ++s->consume_bytes;
#endif
        if(s->receive_count==VK_USB_MAX_FRAME_BYTES){memmove(s->receive,s->receive+1,VK_USB_MAX_FRAME_BYTES-1);--s->receive_count;}s->receive[s->receive_count++]=b[index];
        while(s->receive_count>=4){
#ifdef VK_USB_NATIVE_TEST
            ++s->parser_steps;
#endif
            uint8_t type=s->receive[1];if(s->receive[0]!=1||(type!=0x10&&type!=0x40&&type!=0x41)){memmove(s->receive,s->receive+1,--s->receive_count);continue;}size_t body=(size_t)s->receive[2]|((size_t)s->receive[3]<<8);size_t total=body+4;if(total>VK_USB_MAX_FRAME_BYTES){memmove(s->receive,s->receive+1,--s->receive_count);continue;}if(s->receive_count<total)break;memcpy(s->dispatch,s->receive,total);size_t remain=s->receive_count-total;memmove(s->receive,s->receive+total,remain);s->receive_count=remain;state_unlock(s);dispatch_action_t action=DISPATCH_CONTINUE;esp_err_t e=type==0x10?dispatch_json(s,s->dispatch+4,body,&action):dispatch_chunk(s,type,s->dispatch+4,body);if(action==DISPATCH_RESET_EPOCH){state_lock(s);s->receive_count=0;state_unlock(s);return e;}if(e!=ESP_OK)return e;state_lock(s);}
        state_unlock(s);
    }return ESP_OK;
}

static esp_err_t encode_item(vk_usb_service_t*s,const tx_item_t*item,size_t*total){if(item->kind==TX_BUTTON){static const char*const names[]={"button_down","button_up","button_click"};char j[192];int n;if(item->value.button.has_session_id&&item->value.button.has_duration_ms)n=snprintf(j,sizeof(j),"{\"event\":\"%s\",\"button\":\"k%u\",\"session_id\":%" PRIu32 ",\"duration_ms\":%" PRIu32 "}",names[item->value.button.kind],(unsigned)item->value.button.key+1,item->value.button.session_id,item->value.button.duration_ms);else if(item->value.button.has_session_id)n=snprintf(j,sizeof(j),"{\"event\":\"%s\",\"button\":\"k%u\",\"session_id\":%" PRIu32 "}",names[item->value.button.kind],(unsigned)item->value.button.key+1,item->value.button.session_id);else if(item->value.button.has_duration_ms)n=snprintf(j,sizeof(j),"{\"event\":\"%s\",\"button\":\"k%u\",\"duration_ms\":%" PRIu32 "}",names[item->value.button.kind],(unsigned)item->value.button.key+1,item->value.button.duration_ms);else n=snprintf(j,sizeof(j),"{\"event\":\"%s\",\"button\":\"k%u\"}",names[item->value.button.kind],(unsigned)item->value.button.key+1);if(n<0||(size_t)n>=sizeof(j))return ESP_ERR_INVALID_SIZE;size_t length=(size_t)n;s->transmit[0]=1;s->transmit[1]=0x10;s->transmit[2]=(uint8_t)length;s->transmit[3]=(uint8_t)(length>>8);memcpy(s->transmit+4,j,length);*total=length+4U;return ESP_OK;}*total=16U+item->value.audio.length;uint8_t*out=s->transmit;out[0]=1;out[1]=1;out[2]=0x10;out[3]=0;for(size_t i=0;i<4;++i){out[4+i]=(uint8_t)(item->value.audio.session_id>>(8*i));out[8+i]=(uint8_t)(item->value.audio.sequence>>(8*i));}out[12]=item->value.audio.flags;out[13]=0;out[14]=(uint8_t)item->value.audio.length;out[15]=(uint8_t)(item->value.audio.length>>8);memcpy(out+16,item->value.audio.payload,item->value.audio.length);return ESP_OK;}
static esp_err_t encode_terminal(vk_usb_service_t*s,terminal_kind_t terminal,uint32_t id,size_t*total){char j[160];int n;if(terminal==TERMINAL_CONTROL)n=snprintf(j,sizeof(j),"{\"event\":\"vk_error\",\"operation\":\"session\",\"code\":\"control_queue_overflow\"}");else if(terminal==TERMINAL_INPUT)n=snprintf(j,sizeof(j),"{\"event\":\"vk_error\",\"operation\":\"input\",\"code\":\"input_queue_overflow\"}");else n=snprintf(j,sizeof(j),"{\"event\":\"vk_error\",\"operation\":\"audio\",\"code\":\"audio_queue_overflow\",\"session_id\":%" PRIu32 "}",id);if(n<0||(size_t)n>=sizeof(j))return ESP_ERR_INVALID_SIZE;size_t length=(size_t)n;s->transmit[0]=1;s->transmit[1]=0x10;s->transmit[2]=(uint8_t)length;s->transmit[3]=(uint8_t)(length>>8);memcpy(s->transmit+4,j,length);*total=length+4U;return ESP_OK;}
static esp_err_t write_committed_locked(vk_usb_service_t*s,size_t total){int written=s->transport.write(s->transport.context,s->transmit,total,VK_USB_TX_TIMEOUT_MS);if(written<=0||(size_t)written>total){state_unlock(s);return ESP_FAIL;}state_unlock(s);size_t offset=(size_t)written;while(offset<total){written=s->transport.write(s->transport.context,s->transmit+offset,total-offset,VK_USB_TX_TIMEOUT_MS);if(written<=0||(size_t)written>total-offset)return ESP_FAIL;offset+=(size_t)written;}return ESP_OK;}

size_t vk_usb_service_size(void){return sizeof(vk_usb_service_t);}
esp_err_t vk_usb_service_init(vk_usb_service_t*s,const vk_usb_transport_ops_t*t,const vk_usb_identity_t*i,const vk_usb_service_policy_t*p){if(!s||!t||!i||!p||!t->install||!t->uninstall||!t->read||!t->write||!t->now_ms||((t->state_lock==NULL)!=(t->state_unlock==NULL)))return ESP_ERR_INVALID_ARG;memset(s,0,sizeof(*s));s->transport=*t;s->policy=*p;if(!copy_identity(s->hardware,i->hardware)||!copy_identity(s->firmware_version,i->firmware_version)||!copy_identity(s->device_id,i->device_id)){memset(s,0,sizeof(*s));return ESP_ERR_INVALID_ARG;}return ESP_OK;}
esp_err_t vk_usb_service_start(vk_usb_service_t*s){if(!s)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->capability_provider_in_flight!=0U||s->protocol_callbacks_in_flight!=0U){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->stop_requested=false;s->capability_provider_admission_open=true;s->protocol_callback_admission_open=true;state_unlock(s);esp_err_t e=s->transport.install(s->transport.context);state_lock(s);if(e==ESP_OK)s->installed=true;else{s->capability_provider_admission_open=false;s->protocol_callback_admission_open=false;}state_unlock(s);return e;}
esp_err_t vk_usb_service_poll(vk_usb_service_t*s){if(!s)return ESP_ERR_INVALID_ARG;uint64_t now=s->transport.now_ms(s->transport.context);
    esp_err_t lifecycle=vk_usb_service_process_lifecycle(s);
    if(lifecycle==VK_USB_LIFECYCLE_PENDING)return ESP_OK;
    if(lifecycle!=ESP_OK)return lifecycle;
    state_lock(s);bool expired=s->epoch_active&&now>=s->lease_deadline_ms;uint32_t expired_epoch=s->epoch;state_unlock(s);
    if(expired){esp_err_t e=start_input_lifecycle(s,VK_USB_INPUT_LIFECYCLE_LEASE_EXPIRED,expired_epoch,0U,now);if(e!=ESP_OK)return e;lifecycle=vk_usb_service_process_lifecycle(s);if(lifecycle==VK_USB_LIFECYCLE_PENDING)return ESP_OK;if(lifecycle!=ESP_OK)return lifecycle;}
#ifdef VK_USB_NATIVE_TEST
vk_usb_before_tx_commit_hook_t hook=NULL;void*hook_context=NULL;
#endif
state_lock(s);if(!s->installed||s->stop_requested||s->poll_owner_active){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->poll_owner_active=true;if(s->terminal==TERMINAL_NONE&&s->tx_count!=0){s->in_flight_item=s->tx_queue[s->tx_head];s->tx_head=(s->tx_head+1U)%VK_USB_TYPED_TX_QUEUE_CAPACITY;--s->tx_count;s->tx_in_flight=true;
#ifdef VK_USB_NATIVE_TEST
hook=s->before_tx_commit_hook;hook_context=s->before_tx_commit_context;
#endif
}state_unlock(s);
#ifdef VK_USB_NATIVE_TEST
if(hook!=NULL)hook(hook_context);
#endif
state_lock(s);now=s->transport.now_ms(s->transport.context);if(s->epoch_active&&now>=s->lease_deadline_ms){uint32_t old=s->epoch;s->tx_in_flight=false;s->poll_owner_active=false;state_unlock(s);esp_err_t le=start_input_lifecycle(s,VK_USB_INPUT_LIFECYCLE_LEASE_EXPIRED,old,0U,now);if(le!=ESP_OK)return le;le=vk_usb_service_process_lifecycle(s);return le==VK_USB_LIFECYCLE_PENDING?ESP_OK:le;}if(!s->installed||s->stop_requested){s->tx_in_flight=false;s->poll_owner_active=false;state_unlock(s);return ESP_ERR_INVALID_STATE;}terminal_kind_t terminal=s->terminal;uint32_t terminal_id=s->terminal_audio_session_id;size_t total=0;esp_err_t e=ESP_OK;if(terminal!=TERMINAL_NONE){s->tx_in_flight=false;e=encode_terminal(s,terminal,terminal_id,&total);if(e==ESP_OK){s->terminal=TERMINAL_NONE;e=write_committed_locked(s,total);}else state_unlock(s);}else if(s->tx_in_flight){e=encode_item(s,&s->in_flight_item,&total);if(e==ESP_OK)e=write_committed_locked(s,total);else state_unlock(s);}else state_unlock(s);if(e!=ESP_OK){state_lock(s);s->tx_in_flight=false;s->poll_owner_active=false;state_unlock(s);return e;}state_lock(s);s->tx_in_flight=false;if(s->stop_requested||!s->installed){s->poll_owner_active=false;state_unlock(s);return ESP_ERR_INVALID_STATE;}state_unlock(s);uint8_t b[VK_USB_IO_CHUNK_BYTES];int n=s->transport.read(s->transport.context,b,sizeof(b),VK_USB_RX_TIMEOUT_MS);if(n<0||(size_t)n>sizeof(b))e=ESP_FAIL;else if(n!=0)e=consume(s,b,(size_t)n);state_lock(s);s->poll_owner_active=false;state_unlock(s);return e;}
void vk_usb_service_request_stop(vk_usb_service_t*s){
    if(!s)return;
    uint64_t now=s->transport.now_ms(s->transport.context);uint32_t old_epoch;
    state_lock(s);old_epoch=s->epoch_active?s->epoch:0U;s->capability_provider_admission_open=false;s->protocol_callback_admission_open=false;state_unlock(s);
    (void)start_input_lifecycle(s,VK_USB_INPUT_LIFECYCLE_STOPPING,old_epoch,0U,now);
}
esp_err_t vk_usb_service_stop(vk_usb_service_t*s){
    if(!s)return ESP_ERR_INVALID_ARG;
    vk_usb_service_request_stop(s);
    bool lifecycle_done=false;
    for(uint32_t spin=0U;spin<VK_USB_PROVIDER_QUIESCE_SPINS;++spin){
        esp_err_t lifecycle=vk_usb_service_process_lifecycle(s);
        if(lifecycle==ESP_OK){lifecycle_done=true;break;}
        if(lifecycle!=VK_USB_LIFECYCLE_PENDING)return lifecycle;
    }
    if(!lifecycle_done)return ESP_ERR_TIMEOUT;
    state_lock(s);s->stop_requested=true;s->epoch_active=false;clear_capability_locked(s);s->receive_count=0;clear_queue_locked(s);s->terminal=TERMINAL_NONE;state_unlock(s);
    for(uint32_t spin=0U;spin<VK_USB_PROVIDER_QUIESCE_SPINS;++spin){
        state_lock(s);bool callback_active=s->capability_provider_in_flight!=0U||s->protocol_callbacks_in_flight!=0U;state_unlock(s);
        if(!callback_active)break;
    }
    state_lock(s);if(s->capability_provider_in_flight!=0U||s->protocol_callbacks_in_flight!=0U){state_unlock(s);return ESP_ERR_TIMEOUT;}if(s->poll_owner_active){state_unlock(s);return ESP_ERR_INVALID_STATE;}
    bool installed=s->installed;state_unlock(s);if(!installed)return ESP_OK;
    esp_err_t e=s->transport.uninstall(s->transport.context);if(e==ESP_OK){state_lock(s);s->installed=false;state_unlock(s);}return e;
}
bool vk_usb_service_has_epoch(vk_usb_service_t*s){if(!s)return false;state_lock(s);bool v=s->epoch_active;state_unlock(s);return v;}uint32_t vk_usb_service_epoch(vk_usb_service_t*s){if(!s)return 0;state_lock(s);uint32_t v=s->epoch;state_unlock(s);return v;}
#ifdef VK_USB_NATIVE_TEST
void vk_usb_service_set_epoch_for_test(vk_usb_service_t*s,uint32_t e){if(s){state_lock(s);s->epoch=e;state_unlock(s);}}size_t vk_usb_service_tx_queue_count_for_test(vk_usb_service_t*s){if(!s)return 0;state_lock(s);size_t n=s->tx_count;state_unlock(s);return n;}esp_err_t vk_usb_service_consume_for_test(vk_usb_service_t*s,const uint8_t*b,size_t n){return !s||(!b&&n)?ESP_ERR_INVALID_ARG:consume(s,b,n);}void vk_usb_service_set_before_tx_commit_hook_for_test(vk_usb_service_t*s,vk_usb_before_tx_commit_hook_t hook,void*context){if(s){state_lock(s);s->before_tx_commit_hook=hook;s->before_tx_commit_context=context;state_unlock(s);}}size_t vk_usb_service_consume_bytes_for_test(vk_usb_service_t*s){if(!s)return 0;state_lock(s);size_t n=s->consume_bytes;state_unlock(s);return n;}size_t vk_usb_service_parser_steps_for_test(vk_usb_service_t*s){if(!s)return 0;state_lock(s);size_t n=s->parser_steps;state_unlock(s);return n;}
#endif
esp_err_t vk_usb_service_register_capability_provider(vk_usb_service_t*s,const vk_usb_capability_provider_registration_t*r){if(!s||!r||!r->get_snapshot)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->capability_provider.get_snapshot){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->capability_provider=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_asset_handler(vk_usb_service_t*s,const vk_usb_asset_handler_registration_t*r){if(!s||!r||!r->handle_command||!r->handle_chunk||r->chunk_bytes==0||r->chunk_bytes>VK_USB_ASSET_CHUNK_MAX_BYTES||r->max_asset_bytes==0)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->asset_handler.handle_command){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->asset_handler=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_screen_handler(vk_usb_service_t*s,const vk_usb_screen_handler_registration_t*r){if(!s||!r||!r->handle_command)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->screen_handler.handle_command){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->screen_handler=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_widget_handler(vk_usb_service_t*s,const vk_usb_widget_handler_registration_t*r){if(!s||!r||!r->handle_command)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->widget_handler.handle_command){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->widget_handler=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_update_handler(vk_usb_service_t*s,const vk_usb_update_handler_registration_t*r){if(!s||!r)return ESP_ERR_INVALID_ARG;(void)s;(void)r;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t vk_usb_service_register_led_handler(vk_usb_service_t*s,const vk_usb_led_handler_registration_t*r){if(!s||!r||!r->handle_command)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->led_handler.handle_command){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->led_handler=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_input_handler(vk_usb_service_t*s,const vk_usb_input_handler_registration_t*r){if(!s||!r||!r->handle_command)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->input_handler.handle_command){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->input_handler=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_input_lifecycle(vk_usb_service_t*s,const vk_usb_input_lifecycle_registration_t*r){if(!s||!r||!r->begin)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->input_lifecycle.begin){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->input_lifecycle=*r;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_register_led_lifecycle(vk_usb_service_t*s,const vk_usb_led_lifecycle_registration_t*r){if(!s||!r||!r->begin)return ESP_ERR_INVALID_ARG;state_lock(s);if(s->installed||s->led_lifecycle.begin){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->led_lifecycle=*r;state_unlock(s);return ESP_OK;}
static void remove_audio_session_locked(vk_usb_service_t*s,uint32_t id){size_t count=0;for(size_t i=0;i<s->tx_count;++i){tx_item_t item=s->tx_queue[(s->tx_head+i)%VK_USB_TYPED_TX_QUEUE_CAPACITY];if(item.kind!=TX_AUDIO||item.value.audio.session_id!=id)s->tx_scratch[count++]=item;}memcpy(s->tx_queue,s->tx_scratch,count*sizeof(*s->tx_scratch));s->tx_head=0;s->tx_count=count;}
static bool protocol_use_begin(vk_usb_service_t*s,uint32_t epoch,uint32_t generation)
{
    state_lock(s);bool ok=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==epoch&&s->capability_valid&&s->capability_generation==generation&&s->protocol_callback_admission_open;
    if(ok) ++s->protocol_callbacks_in_flight;
    state_unlock(s);return ok;
}
static void protocol_use_end(vk_usb_service_t*s){state_lock(s);if(s->protocol_callbacks_in_flight!=0U)--s->protocol_callbacks_in_flight;state_unlock(s);}
esp_err_t vk_usb_service_send_asset_event_for_epoch(vk_usb_service_t*s,uint32_t epoch,uint32_t generation,const vk_usb_asset_event_t*event){if(!s||epoch==0U||generation==0U||!event)return ESP_ERR_INVALID_ARG;if(!protocol_use_begin(s,epoch,generation))return ESP_ERR_INVALID_STATE;size_t length=0;esp_err_t result=vk_usb_asset_event_encode(event,s->protocol_json,sizeof(s->protocol_json),&length);if(result==ESP_OK)result=send_json_bytes(s,s->protocol_json,length);protocol_use_end(s);return result;}
esp_err_t vk_usb_service_send_screen_event_for_epoch(vk_usb_service_t*s,uint32_t epoch,uint32_t generation,const vk_usb_screen_event_t*event){if(!s||epoch==0U||generation==0U||!event)return ESP_ERR_INVALID_ARG;if(!protocol_use_begin(s,epoch,generation))return ESP_ERR_INVALID_STATE;size_t length=0;esp_err_t result=vk_usb_screen_event_encode(event,s->protocol_json,sizeof(s->protocol_json),&length);if(result==ESP_OK)result=send_json_bytes(s,s->protocol_json,length);protocol_use_end(s);return result;}
esp_err_t vk_usb_service_send_widget_event_for_epoch(vk_usb_service_t*s,uint32_t epoch,uint32_t generation,const vk_usb_widget_event_t*event){if(!s||epoch==0U||generation==0U||!event)return ESP_ERR_INVALID_ARG;if(!protocol_use_begin(s,epoch,generation))return ESP_ERR_INVALID_STATE;size_t length=0;esp_err_t result=vk_usb_widget_event_encode(event,s->protocol_json,sizeof(s->protocol_json),&length);if(result==ESP_OK)result=send_json_bytes(s,s->protocol_json,length);protocol_use_end(s);return result;}
esp_err_t vk_usb_service_send_protocol_error_for_epoch(vk_usb_service_t*s,uint32_t epoch,uint32_t generation,const vk_usb_protocol_error_t*error){if(!s||epoch==0U||generation==0U||!error)return ESP_ERR_INVALID_ARG;if(!protocol_use_begin(s,epoch,generation))return ESP_ERR_INVALID_STATE;size_t length=0;esp_err_t result=vk_usb_protocol_error_encode(error,s->protocol_json,sizeof(s->protocol_json),&length);if(result==ESP_OK)result=send_json_bytes(s,s->protocol_json,length);protocol_use_end(s);return result;}
esp_err_t vk_usb_service_send_led_state_for_epoch(vk_usb_service_t*s,uint32_t epoch,uint32_t generation,const vk_usb_led_state_event_t*event){if(!s||epoch==0U||generation==0U||!event)return ESP_ERR_INVALID_ARG;if(!protocol_use_begin(s,epoch,generation))return ESP_ERR_INVALID_STATE;size_t length=0;esp_err_t result=vk_usb_led_state_encode(event,s->protocol_json,sizeof(s->protocol_json),&length);if(result==ESP_OK)result=send_json_bytes(s,s->protocol_json,length);protocol_use_end(s);return result;}
esp_err_t vk_usb_service_send_led_error_for_epoch(vk_usb_service_t*s,uint32_t epoch,uint32_t generation,const vk_usb_led_error_event_t*event){if(!s||epoch==0U||generation==0U||!event)return ESP_ERR_INVALID_ARG;if(!protocol_use_begin(s,epoch,generation))return ESP_ERR_INVALID_STATE;size_t length=0;esp_err_t result=vk_usb_led_error_encode(event,s->protocol_json,sizeof(s->protocol_json),&length);if(result==ESP_OK)result=send_json_bytes(s,s->protocol_json,length);protocol_use_end(s);return result;}

static bool valid_button(const vk_usb_button_event_t*e){return e&&e->key<=VK_USB_KEY_K4&&e->kind<=VK_USB_BUTTON_CLICK&&((e->kind==VK_USB_BUTTON_DOWN&&!e->has_duration_ms)||(e->kind!=VK_USB_BUTTON_DOWN&&e->has_duration_ms))&&(!e->has_session_id||e->session_id!=0U);}
vk_usb_handoff_result_t vk_usb_service_send_button_for_epoch(vk_usb_service_t*s,uint32_t expected_epoch,const vk_usb_button_event_t*e){if(!s||expected_epoch==0U||!valid_button(e))return VK_USB_HANDOFF_EPOCH_CLOSED;tx_item_t i={.kind=TX_BUTTON};i.value.button=*e;state_lock(s);if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=expected_epoch){state_unlock(s);return VK_USB_HANDOFF_EPOCH_CLOSED;}if(s->terminal!=TERMINAL_NONE){state_unlock(s);return VK_USB_HANDOFF_OVERFLOW;}if(s->tx_count==VK_USB_TYPED_TX_QUEUE_CAPACITY){state_unlock(s);return VK_USB_HANDOFF_RETRY;}size_t at=(s->tx_head+s->tx_count)%VK_USB_TYPED_TX_QUEUE_CAPACITY;s->tx_queue[at]=i;++s->tx_count;state_unlock(s);return VK_USB_HANDOFF_ACCEPTED;}
esp_err_t vk_usb_service_send_button(vk_usb_service_t*s,const vk_usb_button_event_t*e){if(!s||!e||e->key>VK_USB_KEY_K4||e->kind>VK_USB_BUTTON_CLICK)return ESP_ERR_INVALID_ARG;tx_item_t i={.kind=TX_BUTTON};i.value.button=*e;state_lock(s);if(!s->installed||s->stop_requested||!s->epoch_active||s->terminal!=TERMINAL_NONE){state_unlock(s);return ESP_ERR_INVALID_STATE;}if(s->tx_count==VK_USB_TYPED_TX_QUEUE_CAPACITY){clear_queue_locked(s);s->terminal=TERMINAL_CONTROL;s->terminal_audio_session_id=0;s->epoch_active=false;clear_capability_locked(s);state_unlock(s);return ESP_ERR_NO_MEM;}size_t at=(s->tx_head+s->tx_count)%VK_USB_TYPED_TX_QUEUE_CAPACITY;s->tx_queue[at]=i;++s->tx_count;state_unlock(s);return ESP_OK;}
static const char*input_error_name(vk_usb_input_error_t e){static const char*const n[]={"invalid_request","wrong_epoch","busy","input_queue_overflow","audio_start_failed","audio_stop_failed","audio_runtime_failed","tainted"};return e<=VK_USB_INPUT_ERROR_TAINTED?n[e]:NULL;}
esp_err_t vk_usb_service_send_input_state_for_epoch(vk_usb_service_t*s,uint32_t expected_epoch,vk_usb_input_mode_t mode,vk_usb_key_t key){if(!s||expected_epoch==0U||mode>VK_USB_INPUT_MODE_CLICK_TO_TALK||key>VK_USB_KEY_NONE)return ESP_ERR_INVALID_ARG;state_lock(s);bool ok=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==expected_epoch;state_unlock(s);if(!ok)return ESP_ERR_INVALID_STATE;char key_name[5];if(key==VK_USB_KEY_NONE)snprintf(key_name,sizeof(key_name),"none");else snprintf(key_name,sizeof(key_name),"k%u",(unsigned)key+1U);char j[160];int n=snprintf(j,sizeof(j),"{\"event\":\"vk_input_state\",\"interaction_mode\":\"%s\",\"voice_key\":\"%s\"}",mode==VK_USB_INPUT_MODE_HOLD_TO_TALK?"hold_to_talk":"click_to_talk",key_name);return n<0||(size_t)n>=sizeof(j)?ESP_ERR_INVALID_SIZE:send_json(s,j);}
esp_err_t vk_usb_service_send_input_error_for_epoch(vk_usb_service_t*s,uint32_t expected_epoch,vk_usb_input_error_t error){const char*code=input_error_name(error);if(!s||expected_epoch==0U||code==NULL)return ESP_ERR_INVALID_ARG;state_lock(s);bool ok=s->installed&&!s->stop_requested&&s->epoch_active&&s->epoch==expected_epoch;state_unlock(s);return ok?send_error(s,"input",code):ESP_ERR_INVALID_STATE;}
esp_err_t vk_usb_service_fail_input_epoch(vk_usb_service_t*s,uint32_t expected_epoch,vk_usb_input_error_t error){const char*code=input_error_name(error);if(!s||expected_epoch==0U||code==NULL)return ESP_ERR_INVALID_ARG;state_lock(s);if(!s->installed||s->stop_requested||!s->epoch_active||s->epoch!=expected_epoch){state_unlock(s);return ESP_ERR_INVALID_STATE;}s->epoch_active=false;clear_capability_locked(s);clear_queue_locked(s);s->terminal=TERMINAL_INPUT;state_unlock(s);return ESP_OK;}
static esp_err_t send_audio(vk_usb_service_t*s,uint32_t expected_epoch,bool require_epoch,const vk_usb_audio_frame_t*f){if(!s||!f||f->session_id==0U||f->payload_length>VK_USB_AUDIO_MAX_PAYLOAD_BYTES||(f->payload_length&&f->payload==NULL))return ESP_ERR_INVALID_ARG;tx_item_t i={.kind=TX_AUDIO};i.value.audio.session_id=f->session_id;i.value.audio.sequence=f->sequence;i.value.audio.flags=f->flags;i.value.audio.length=f->payload_length;if(f->payload_length)memcpy(i.value.audio.payload,f->payload,f->payload_length);state_lock(s);if(!s->installed||s->stop_requested||!s->epoch_active||(require_epoch&&s->epoch!=expected_epoch)||s->terminal!=TERMINAL_NONE||(s->audio_truncated&&s->truncated_audio_session_id==f->session_id)){state_unlock(s);return ESP_ERR_INVALID_STATE;}if(s->tx_count==VK_USB_TYPED_TX_QUEUE_CAPACITY){remove_audio_session_locked(s,f->session_id);s->audio_truncated=true;s->truncated_audio_session_id=f->session_id;s->terminal=TERMINAL_AUDIO;s->terminal_audio_session_id=f->session_id;state_unlock(s);return ESP_ERR_NO_MEM;}size_t at=(s->tx_head+s->tx_count)%VK_USB_TYPED_TX_QUEUE_CAPACITY;s->tx_queue[at]=i;++s->tx_count;state_unlock(s);return ESP_OK;}
esp_err_t vk_usb_service_send_audio(vk_usb_service_t*s,const vk_usb_audio_frame_t*f){return send_audio(s,0U,false,f);}
esp_err_t vk_usb_service_send_audio_for_epoch(vk_usb_service_t*s,uint32_t expected_epoch,const vk_usb_audio_frame_t*f){if(expected_epoch==0U)return ESP_ERR_INVALID_ARG;return send_audio(s,expected_epoch,true,f);}
