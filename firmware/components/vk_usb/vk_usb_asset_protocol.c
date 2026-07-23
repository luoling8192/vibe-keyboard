#include "vk_usb_asset_protocol.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_WIDTH 428U
#define DISPLAY_HEIGHT 142U

typedef struct { char *bytes; size_t capacity; size_t length; bool failed; } builder_t;

static void append(builder_t *b, const char *format, ...)
{
    if (b->failed || b->length >= b->capacity) { b->failed = true; return; }
    va_list arguments; va_start(arguments, format);
    int count = vsnprintf(b->bytes + b->length, b->capacity - b->length, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= b->capacity - b->length) { b->failed = true; return; }
    b->length += (size_t)count;
}

static bool exact(const vk_usb_json_document_t *d, uint16_t node, const char *const *keys, size_t count)
{ return vk_usb_json_object_exact_keys(d, node, keys, count); }
static uint16_t find(const vk_usb_json_document_t *d, uint16_t node, const char *key)
{ return vk_usb_json_object_find(d, node, key); }
static bool string(const vk_usb_json_document_t *d, uint16_t node, char *value, size_t capacity)
{ return node != VK_USB_JSON_NO_NODE && vk_usb_json_string_copy(d, node, value, capacity) == VK_USB_JSON_OK; }
static bool uint32_value(const vk_usb_json_document_t *d, uint16_t node, uint32_t maximum, bool nonzero, uint32_t *value)
{ return node != VK_USB_JSON_NO_NODE && vk_usb_json_uint32(d, node, maximum, nonzero, value) == VK_USB_JSON_OK; }
static bool valid_sha(const char *value)
{
    if (value == NULL || strlen(value) != 64U) return false;
    for (size_t index = 0U; index < 64U; ++index)
        if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
    return true;
}
static bool number_milli(const vk_usb_json_document_t *d, uint16_t node, int64_t *value)
{
    size_t start, end;
    if (d == NULL || value == NULL || vk_usb_json_kind(d, node) != VK_USB_JSON_NUMBER ||
        !vk_usb_json_node_range(d, node, &start, &end) || start >= end) return false;
    bool negative = d->bytes[start] == '-';
    if (negative && ++start == end) return false;
    uint64_t whole = 0U, fraction = 0U;uint8_t scale = 0U;size_t offset = start;
    if (d->bytes[offset] == '0') ++offset;
    else {
        if (d->bytes[offset] < '1' || d->bytes[offset] > '9') return false;
        while (offset < end && d->bytes[offset] >= '0' && d->bytes[offset] <= '9') {
            uint8_t digit = (uint8_t)(d->bytes[offset++] - '0');
            if (whole > (UINT64_MAX - digit) / 10U) return false;
            whole = whole * 10U + digit;
        }
    }
    if (offset < end && d->bytes[offset] == '.') {
        ++offset;
        while (offset < end && d->bytes[offset] >= '0' && d->bytes[offset] <= '9' && scale < 3U) {
            fraction = fraction * 10U + (uint8_t)(d->bytes[offset++] - '0');++scale;
        }
        if (scale == 0U) return false;
    }
    if (offset != end || whole > (UINT64_MAX - fraction) / 1000U) return false;
    while (scale < 3U) { fraction *= 10U;++scale; }
    uint64_t magnitude = whole * 1000U + fraction;
    if ((!negative && magnitude > INT64_MAX) || (negative && magnitude > (uint64_t)INT64_MAX + 1U)) return false;
    *value = negative ? (magnitude == (uint64_t)INT64_MAX + 1U ? INT64_MIN : -(int64_t)magnitude) : (int64_t)magnitude;
    return true;
}

static bool valid_identifier(const char *value)
{
    size_t length = value == NULL ? 0U : strlen(value);
    if (length == 0U || length > 32U) return false;
    for (size_t index = 0U; index < length; ++index) {
        char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_')) return false;
    }
    return true;
}
static bool kind_from_node(const vk_usb_json_document_t *d, uint16_t node, vk_usb_asset_kind_t *kind)
{
    char value[17];
    if (!string(d, node, value, sizeof(value))) return false;
    if (strcmp(value, "image") == 0) *kind = VK_USB_ASSET_KIND_IMAGE;
    else if (strcmp(value, "animation") == 0) *kind = VK_USB_ASSET_KIND_ANIMATION;
    else if (strcmp(value, "glyph_bitmap") == 0) *kind = VK_USB_ASSET_KIND_GLYPH_BITMAP;
    else return false;
    return true;
}
static const char *kind_name(vk_usb_asset_kind_t kind)
{
    static const char *const names[] = {"image", "animation", "glyph_bitmap"};
    return kind <= VK_USB_ASSET_KIND_GLYPH_BITMAP ? names[kind] : NULL;
}
static bool mode_from_node(const vk_usb_json_document_t *d, uint16_t node, vk_usb_screen_configured_mode_t *mode)
{
    char value[16];
    if (!string(d, node, value, sizeof(value))) return false;
    if (strcmp(value, "image") == 0) *mode = VK_USB_SCREEN_IMAGE;
    else if (strcmp(value, "pet") == 0) *mode = VK_USB_SCREEN_PET;
    else if (strcmp(value, "dashboard") == 0) *mode = VK_USB_SCREEN_DASHBOARD;
    else if (strcmp(value, "custom") == 0) *mode = VK_USB_SCREEN_CUSTOM;
    else return false;
    return true;
}
static const char *mode_name(vk_usb_screen_configured_mode_t mode)
{
    static const char *const names[] = {"image", "pet", "dashboard", "custom"};
    return mode <= VK_USB_SCREEN_CUSTOM ? names[mode] : NULL;
}

esp_err_t vk_usb_asset_command_decode(const vk_usb_json_document_t *d, uint16_t root,
                                      const vk_usb_assets_capability_t *capability,
                                      uint32_t epoch, uint32_t generation,
                                      vk_usb_asset_command_t *command)
{
    if (d == NULL || capability == NULL || command == NULL || epoch == 0U || generation == 0U ||
        vk_usb_json_kind(d, root) != VK_USB_JSON_OBJECT) return ESP_ERR_INVALID_ARG;
    memset(command, 0, sizeof(*command)); command->expected_epoch = epoch; command->snapshot_generation = generation;
    char event[32];
    if (!string(d, find(d, root, "event"), event, sizeof(event))) return ESP_ERR_INVALID_ARG;
    if (strcmp(event, "vk_asset_begin") == 0 || strcmp(event, "vk_asset_end") == 0) {
        const char *const keys[] = {"event", "transfer_id", "sha256", "total_bytes", "kind"};
        if (!exact(d, root, keys, 5U) || !uint32_value(d, find(d, root, "transfer_id"), UINT32_MAX, true, &command->transfer_id) ||
            !uint32_value(d, find(d, root, "total_bytes"), UINT32_MAX, true, &command->total_bytes) ||
            !string(d, find(d, root, "sha256"), command->sha256, sizeof(command->sha256)) || !valid_sha(command->sha256) ||
            !string(d, find(d, root, "kind"), command->asset_kind, sizeof(command->asset_kind)) ||
            !kind_from_node(d, find(d, root, "kind"), &command->asset_kind_value)) return ESP_ERR_INVALID_ARG;
        command->kind = strcmp(event, "vk_asset_begin") == 0 ? VK_USB_ASSET_BEGIN : VK_USB_ASSET_END;
        return ESP_OK;
    }
    if (strcmp(event, "vk_asset_query") == 0 || strcmp(event, "vk_asset_abort") == 0) {
        const char *const keys[] = {"event", "transfer_id"};
        if (!exact(d, root, keys, 2U) || !uint32_value(d, find(d, root, "transfer_id"), UINT32_MAX, true, &command->transfer_id)) return ESP_ERR_INVALID_ARG;
        command->kind = strcmp(event, "vk_asset_query") == 0 ? VK_USB_ASSET_QUERY : VK_USB_ASSET_ABORT;
        return ESP_OK;
    }
    if (strcmp(event, "vk_asset_list") == 0) {
        const char *const keys[] = {"event", "snapshot_id", "cursor", "limit"}; uint32_t limit;
        if (!exact(d, root, keys, 4U) || !uint32_value(d, find(d, root, "snapshot_id"), UINT32_MAX, false, &command->snapshot_id) ||
            !uint32_value(d, find(d, root, "cursor"), UINT32_MAX, false, &command->cursor) ||
            !uint32_value(d, find(d, root, "limit"), 64U, true, &limit) || (command->snapshot_id == 0U && command->cursor != 0U)) return ESP_ERR_INVALID_ARG;
        command->limit = (uint8_t)limit; command->kind = VK_USB_ASSET_LIST; return ESP_OK;
    }
    if (strcmp(event, "vk_asset_delete") == 0) {
        const char *const keys[] = {"event", "sha256", "expected_revision"};
        if (!exact(d, root, keys, 3U) || !string(d, find(d, root, "sha256"), command->sha256, sizeof(command->sha256)) || !valid_sha(command->sha256) ||
            !uint32_value(d, find(d, root, "expected_revision"), UINT32_MAX, false, &command->expected_revision)) return ESP_ERR_INVALID_ARG;
        command->kind = VK_USB_ASSET_DELETE; return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool validate_asset_entries(const vk_usb_json_document_t *d, uint16_t assets, uint16_t maximum)
{
    if (vk_usb_json_kind(d, assets) != VK_USB_JSON_ARRAY || vk_usb_json_array_count(d, assets) > maximum) return false;
    const char *const keys[] = {"bytes", "kind", "sha256"};
    char previous[65] = {0}; size_t count = 0U;
    for (uint16_t node = vk_usb_json_first_child(d, assets); node != VK_USB_JSON_NO_NODE; node = vk_usb_json_next_sibling(d, node)) {
        char hash[65]; uint32_t bytes; vk_usb_asset_kind_t kind;
        if (!exact(d, node, keys, 3U) || !uint32_value(d, find(d, node, "bytes"), UINT32_MAX, true, &bytes) ||
            !kind_from_node(d, find(d, node, "kind"), &kind) || !string(d, find(d, node, "sha256"), hash, sizeof(hash)) ||
            !valid_sha(hash) || (count != 0U && strcmp(previous, hash) >= 0)) return false;
        (void)bytes; (void)kind; memcpy(previous, hash, sizeof(previous)); ++count;
    }
    return true;
}

static bool validate_image(const vk_usb_json_document_t *d, uint16_t image)
{
    const char *const keys[] = {"background_rgb888", "fit", "sha256"}; char fit[10], hash[65]; uint32_t rgb;
    return exact(d, image, keys, 3U) && uint32_value(d, find(d, image, "background_rgb888"), 0xFFFFFFU, false, &rgb) &&
           string(d, find(d, image, "fit"), fit, sizeof(fit)) &&
           (strcmp(fit, "contain") == 0 || strcmp(fit, "cover") == 0 || strcmp(fit, "stretch") == 0 || strcmp(fit, "center") == 0) &&
           string(d, find(d, image, "sha256"), hash, sizeof(hash)) && valid_sha(hash);
}

static bool validate_pet(const vk_usb_json_document_t *d, uint16_t pet, uint8_t maximum)
{
    const char *const keys[] = {"id", "states", "version"}; char id[33]; uint32_t version;
    uint16_t states = find(d, pet, "states");
    if (!exact(d, pet, keys, 3U) || !string(d, find(d, pet, "id"), id, sizeof(id)) || !valid_identifier(id) ||
        !uint32_value(d, find(d, pet, "version"), 1U, true, &version) || version != 1U || vk_usb_json_kind(d, states) != VK_USB_JSON_OBJECT) return false;
    size_t state_count = 0U; bool idle = false;
    for (uint16_t key = vk_usb_json_first_child(d, states); key != VK_USB_JSON_NO_NODE;) {
        uint16_t value = vk_usb_json_next_sibling(d, key); char name[16], hash[65], fallback[8];
        if (!vk_usb_json_node_is_object_key(d, key) || value == VK_USB_JSON_NO_NODE || !string(d, key, name, sizeof(name))) return false;
        bool allowed = strcmp(name,"idle")==0||strcmp(name,"active")==0||strcmp(name,"recording")==0||strcmp(name,"thinking")==0||strcmp(name,"success")==0||strcmp(name,"error")==0;
        if (!allowed) return false;
        const char *const sha_keys[]={"sha256"}; const char *const fallback_keys[]={"fallback"};
        bool sha = exact(d,value,sha_keys,1U)&&string(d,find(d,value,"sha256"),hash,sizeof(hash))&&valid_sha(hash);
        bool fb = exact(d,value,fallback_keys,1U)&&string(d,find(d,value,"fallback"),fallback,sizeof(fallback))&&strcmp(fallback,"idle")==0;
        if ((!sha && !fb) || (strcmp(name,"idle")==0 && !sha)) return false;
        idle |= strcmp(name,"idle")==0; ++state_count; key=vk_usb_json_next_sibling(d,value);
    }
    return idle && state_count <= maximum;
}

static bool validate_layout_envelope(const vk_usb_json_document_t *d, uint16_t layout,
                                     const vk_usb_screen_capability_t *capability,
                                     vk_usb_screen_configured_mode_t mode, uint32_t revision)
{
    const char *const keys[]={"background_rgb888","mode","objects","revision","version","widgets"};
    uint32_t rgb, r, version; char layout_mode[12]; uint16_t objects=find(d,layout,"objects"), widgets=find(d,layout,"widgets");
    size_t start,end;
    if (!exact(d,layout,keys,6U)||!uint32_value(d,find(d,layout,"background_rgb888"),0xFFFFFFU,false,&rgb)||
        !string(d,find(d,layout,"mode"),layout_mode,sizeof(layout_mode))||strcmp(layout_mode,mode==VK_USB_SCREEN_DASHBOARD?"dashboard":"custom")!=0||
        !uint32_value(d,find(d,layout,"revision"),UINT32_MAX,true,&r)||r!=revision||
        !uint32_value(d,find(d,layout,"version"),1U,true,&version)||version!=1U||
        vk_usb_json_kind(d,objects)!=VK_USB_JSON_ARRAY||vk_usb_json_kind(d,widgets)!=VK_USB_JSON_ARRAY||
        vk_usb_json_array_count(d,objects)>capability->max_objects||vk_usb_json_array_count(d,widgets)>capability->max_widgets||
        !vk_usb_json_node_range(d,layout,&start,&end)||end-start>capability->max_layout_bytes) return false;
    (void)rgb;
    /* Full semantic object/widget validation belongs to firmware-screen; this ABI gate
     * still rejects every unknown top-level field and enforces negotiated structural budgets. */
    return true;
}

esp_err_t vk_usb_screen_command_decode(const vk_usb_json_document_t *d, uint16_t root,
                                       const vk_usb_screen_capability_t *capability,
                                       uint32_t display_width, uint32_t display_height,
                                       uint32_t epoch, uint32_t generation,
                                       vk_usb_screen_command_t *command)
{
    if (d==NULL||capability==NULL||command==NULL||epoch==0U||generation==0U||display_width!=DISPLAY_WIDTH||display_height!=DISPLAY_HEIGHT) return ESP_ERR_INVALID_ARG;
    memset(command,0,sizeof(*command)); command->expected_epoch=epoch; command->snapshot_generation=generation; command->document=d;
    char event[32]; if(!string(d,find(d,root,"event"),event,sizeof(event)))return ESP_ERR_INVALID_ARG;
    if(strcmp(event,"vk_screen_query")==0){const char*const keys[]={"event"};if(!exact(d,root,keys,1U))return ESP_ERR_INVALID_ARG;command->kind=VK_USB_SCREEN_QUERY;return ESP_OK;}
    if(strcmp(event,"vk_screen_commit")!=0)return ESP_ERR_NOT_SUPPORTED;
    const char*const keys[]={"assets","event","expected_revision","revision","screen"};
    command->assets_node=find(d,root,"assets");command->screen_node=find(d,root,"screen");
    const char*const asset_keys[]={"assets"};const char*const screen_keys[]={"configured_mode","image","layout","pet"};
    if(!exact(d,root,keys,5U)||!exact(d,command->assets_node,asset_keys,1U)||!exact(d,command->screen_node,screen_keys,4U)||
       !uint32_value(d,find(d,root,"expected_revision"),UINT32_MAX,false,&command->expected_revision)||
       !uint32_value(d,find(d,root,"revision"),UINT32_MAX,true,&command->revision)||command->expected_revision!=capability->revision)return ESP_ERR_INVALID_ARG;
    uint32_t delta=command->revision-command->expected_revision;if(delta==0U||delta>=0x80000000U)return ESP_ERR_INVALID_ARG;
    if(!mode_from_node(d,find(d,command->screen_node,"configured_mode"),&command->configured_mode))return ESP_ERR_INVALID_ARG;
    uint8_t required=(uint8_t)(1U<<command->configured_mode);if((capability->modes&required)==0U)return ESP_ERR_INVALID_ARG;
    if(!validate_asset_entries(d,find(d,command->assets_node,"assets"),capability->max_assets))return ESP_ERR_INVALID_ARG;
    uint16_t image=find(d,command->screen_node,"image"),layout=find(d,command->screen_node,"layout"),pet=find(d,command->screen_node,"pet");
    bool valid=false;
    if(command->configured_mode==VK_USB_SCREEN_IMAGE)valid=validate_image(d,image)&&vk_usb_json_is_null(d,layout)&&vk_usb_json_is_null(d,pet);
    else if(command->configured_mode==VK_USB_SCREEN_PET)valid=vk_usb_json_is_null(d,image)&&vk_usb_json_is_null(d,layout)&&validate_pet(d,pet,capability->max_pet_states);
    else valid=vk_usb_json_is_null(d,image)&&vk_usb_json_is_null(d,pet)&&validate_layout_envelope(d,layout,capability,command->configured_mode,command->revision);
    size_t start,end;if(!valid||!vk_usb_json_node_range(d,root,&start,&end)||end-start>capability->max_commit_bytes)return ESP_ERR_INVALID_ARG;
    command->kind=VK_USB_SCREEN_COMMIT;return ESP_OK;
}

static bool valid_event(const vk_usb_asset_event_t *e)
{
    if(e==NULL||e->kind>VK_USB_ASSET_EVENT_DELETED)return false;
    if((e->kind==VK_USB_ASSET_EVENT_READY||e->kind==VK_USB_ASSET_EVENT_STORED)&&
       (e->transfer_id==0U||e->total_bytes==0U||!valid_sha(e->sha256)||kind_name(e->asset_kind)==NULL))return false;
    if((e->kind==VK_USB_ASSET_EVENT_PROGRESS||e->kind==VK_USB_ASSET_EVENT_ABORTED)&&e->transfer_id==0U)return false;
    if(e->kind==VK_USB_ASSET_EVENT_PAGE&&(e->snapshot_id==0U||e->entry_count>VK_USB_ASSET_PAGE_MAX_ENTRIES||(e->entry_count&&e->entries==NULL)))return false;
    return true;
}

esp_err_t vk_usb_asset_event_encode(const vk_usb_asset_event_t *e,char*out,size_t cap,size_t*length)
{
    if(out==NULL||length==NULL||cap==0U||!valid_event(e)) return ESP_ERR_INVALID_ARG;
    builder_t b={out,cap,0U,false};
    switch(e->kind){
    case VK_USB_ASSET_EVENT_STORAGE_FORMATTED:append(&b,"{\"event\":\"vk_storage_formatted\",\"revision\":%"PRIu32"}",e->revision);break;
    case VK_USB_ASSET_EVENT_READY:if(e->chunk_bytes==0U||e->chunk_bytes>VK_USB_ASSET_CHUNK_MAX_BYTES)return ESP_ERR_INVALID_ARG;append(&b,"{\"chunk_bytes\":%u,\"event\":\"vk_asset_ready\",\"kind\":\"%s\",\"next_offset\":%"PRIu32",\"sha256\":\"%s\",\"total_bytes\":%"PRIu32",\"transfer_id\":%"PRIu32"}",(unsigned)e->chunk_bytes,kind_name(e->asset_kind),e->next_offset,e->sha256,e->total_bytes,e->transfer_id);break;
    case VK_USB_ASSET_EVENT_PROGRESS:append(&b,"{\"event\":\"vk_asset_progress\",\"next_offset\":%"PRIu32",\"transfer_id\":%"PRIu32"}",e->next_offset,e->transfer_id);break;
    case VK_USB_ASSET_EVENT_STORED:append(&b,"{\"event\":\"vk_asset_stored\",\"kind\":\"%s\",\"sha256\":\"%s\",\"total_bytes\":%"PRIu32",\"transfer_id\":%"PRIu32"}",kind_name(e->asset_kind),e->sha256,e->total_bytes,e->transfer_id);break;
    case VK_USB_ASSET_EVENT_ABORTED:append(&b,"{\"event\":\"vk_asset_aborted\",\"transfer_id\":%"PRIu32"}",e->transfer_id);break;
    case VK_USB_ASSET_EVENT_DELETED:if(!valid_sha(e->sha256))return ESP_ERR_INVALID_ARG;append(&b,"{\"event\":\"vk_asset_deleted\",\"revision\":%"PRIu32",\"sha256\":\"%s\"}",e->revision,e->sha256);break;
    case VK_USB_ASSET_EVENT_PAGE:
        append(&b,"{\"cursor\":%"PRIu32",\"entries\":[",e->cursor);
        for(size_t i=0;i<e->entry_count;++i){const vk_usb_asset_list_entry_t*x=&e->entries[i];if(!valid_sha(x->sha256)||x->total_bytes==0U||kind_name(x->kind)==NULL)return ESP_ERR_INVALID_ARG;if(i&&strcmp(e->entries[i-1].sha256,x->sha256)>=0)return ESP_ERR_INVALID_ARG;append(&b,"%s{\"kind\":\"%s\",\"referenced\":%s,\"sha256\":\"%s\",\"total_bytes\":%"PRIu32"}",i?",":"",kind_name(x->kind),x->referenced?"true":"false",x->sha256,x->total_bytes);}
        append(&b,"],\"event\":\"vk_asset_page\",\"next_cursor\":");if(e->has_next_cursor){if(e->next_cursor==0U)return ESP_ERR_INVALID_ARG;append(&b,"%"PRIu32,e->next_cursor);}else append(&b,"null");append(&b,",\"revision\":%"PRIu32",\"snapshot_id\":%"PRIu32"}",e->revision,e->snapshot_id);break;
    }
    if(b.failed||b.length>VK_USB_MAX_JSON_BYTES) return ESP_ERR_INVALID_SIZE;
    *length=b.length;return ESP_OK;
}

esp_err_t vk_usb_widget_command_decode(const vk_usb_json_document_t *d, uint16_t root,
                                       const vk_usb_screen_capability_t *capability,
                                       uint32_t epoch, uint32_t generation,
                                       vk_usb_widget_command_t *command)
{
    if (d == NULL || capability == NULL || command == NULL || epoch == 0U || generation == 0U ||
        capability->state != VK_USB_CAPABILITY_AVAILABLE || vk_usb_json_kind(d, root) != VK_USB_JSON_OBJECT) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char *const base_keys[] = {"event", "revision", "widget_id", "sequence", "state"};
    static const char *const value_keys[] = {"event", "revision", "widget_id", "sequence", "state", "value"};
    static const char *const message_keys[] = {"event", "revision", "widget_id", "sequence", "state", "message"};
    char event[32], state[8];
    uint16_t value_node = find(d, root, "value"), message_node = find(d, root, "message");
    if (!string(d, find(d, root, "event"), event, sizeof(event)) || strcmp(event, "vk_widget_update") != 0 ||
        !string(d, find(d, root, "widget_id"), command->widget_id, sizeof(command->widget_id)) ||
        !valid_identifier(command->widget_id) ||
        !uint32_value(d, find(d, root, "revision"), UINT32_MAX, true, &command->revision) ||
        !uint32_value(d, find(d, root, "sequence"), UINT32_MAX, true, &command->sequence) ||
        !string(d, find(d, root, "state"), state, sizeof(state))) return ESP_ERR_INVALID_ARG;
    memset(command->text, 0, sizeof(command->text));
    memset(command->message, 0, sizeof(command->message));
    command->expected_epoch = epoch;
    command->snapshot_generation = generation;
    command->value_kind = VK_USB_WIDGET_VALUE_NONE;
    if (strcmp(state, "fresh") == 0) {
        if (!exact(d, root, value_keys, sizeof(value_keys) / sizeof(*value_keys)) || message_node != VK_USB_JSON_NO_NODE) return ESP_ERR_INVALID_ARG;
        command->state = VK_USB_WIDGET_FRESH;
        if (vk_usb_json_kind(d, value_node) == VK_USB_JSON_STRING) {
            if (vk_usb_json_string_copy(d, value_node, command->text, sizeof(command->text)) != VK_USB_JSON_OK ||
                strlen(command->text) > capability->max_widget_value_bytes) return ESP_ERR_INVALID_ARG;
            command->value_kind = VK_USB_WIDGET_VALUE_TEXT;
        } else if (vk_usb_json_kind(d, value_node) == VK_USB_JSON_NUMBER) {
            /* The firmware model owns fixed-point semantics. The wire accepts canonical signed
             * integer milli-units; decimals are formatted only after declaration matching. */
            if (!number_milli(d, value_node, &command->number_milli)) return ESP_ERR_INVALID_ARG;
            command->value_kind = VK_USB_WIDGET_VALUE_NUMBER;
        } else return ESP_ERR_INVALID_ARG;
    } else if (strcmp(state, "stale") == 0) {
        if (!exact(d, root, base_keys, sizeof(base_keys) / sizeof(*base_keys)) || value_node != VK_USB_JSON_NO_NODE || message_node != VK_USB_JSON_NO_NODE) return ESP_ERR_INVALID_ARG;
        command->state = VK_USB_WIDGET_STALE;
    } else if (strcmp(state, "error") == 0) {
        bool has_message = message_node != VK_USB_JSON_NO_NODE;
        if (!(has_message ? exact(d, root, message_keys, sizeof(message_keys) / sizeof(*message_keys)) :
                            exact(d, root, base_keys, sizeof(base_keys) / sizeof(*base_keys))) ||
            value_node != VK_USB_JSON_NO_NODE) return ESP_ERR_INVALID_ARG;
        if (has_message && (vk_usb_json_string_copy(d, message_node, command->message, sizeof(command->message)) != VK_USB_JSON_OK ||
                            strlen(command->message) > VK_USB_PROTOCOL_MESSAGE_MAX_BYTES)) return ESP_ERR_INVALID_ARG;
        command->state = VK_USB_WIDGET_ERROR;
    } else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t vk_usb_widget_event_encode(const vk_usb_widget_event_t *e, char *out,
                                     size_t cap, size_t *length)
{
    static const char *const states[] = {"fresh", "stale", "error"};
    if (e == NULL || out == NULL || length == NULL || cap == 0U || e->revision == 0U ||
        e->sequence == 0U || e->state > VK_USB_WIDGET_ERROR || !valid_identifier(e->widget_id)) return ESP_ERR_INVALID_ARG;
    builder_t b = {out, cap, 0U, false};
    append(&b, "{\"event\":\"vk_widget_applied\",\"revision\":%" PRIu32
               ",\"sequence\":%" PRIu32 ",\"state\":\"%s\",\"widget_id\":\"%s\"}",
           e->revision, e->sequence, states[e->state], e->widget_id);
    if (b.failed || b.length > VK_USB_MAX_JSON_BYTES) return ESP_ERR_INVALID_SIZE;
    *length = b.length;
    return ESP_OK;
}

esp_err_t vk_usb_screen_event_encode(const vk_usb_screen_event_t*e,char*out,size_t cap,size_t*length)
{
    if(e==NULL||out==NULL||length==NULL||cap==0U||e->kind>VK_USB_SCREEN_EVENT_COMMITTED) return ESP_ERR_INVALID_ARG;
    builder_t b={out,cap,0U,false};
    if(e->kind==VK_USB_SCREEN_EVENT_STATE){if(!e->configured){if(e->revision!=0U)return ESP_ERR_INVALID_ARG;append(&b,"{\"assets_manifest_sha256\":null,\"configured\":false,\"configured_mode\":null,\"event\":\"vk_screen_state\",\"revision\":0,\"screen_manifest_sha256\":null}");}else{if(e->revision==0U||mode_name(e->configured_mode)==NULL||!valid_sha(e->assets_manifest_sha256)||!valid_sha(e->screen_manifest_sha256))return ESP_ERR_INVALID_ARG;append(&b,"{\"assets_manifest_sha256\":\"%s\",\"configured\":true,\"configured_mode\":\"%s\",\"event\":\"vk_screen_state\",\"revision\":%"PRIu32",\"screen_manifest_sha256\":\"%s\"}",e->assets_manifest_sha256,mode_name(e->configured_mode),e->revision,e->screen_manifest_sha256);}}
    else{if(e->revision==0U||!valid_sha(e->assets_manifest_sha256)||!valid_sha(e->screen_manifest_sha256))return ESP_ERR_INVALID_ARG;append(&b,"{\"assets_manifest_sha256\":\"%s\",\"event\":\"vk_screen_committed\",\"previous_revision\":%"PRIu32",\"revision\":%"PRIu32",\"screen_manifest_sha256\":\"%s\"}",e->assets_manifest_sha256,e->previous_revision,e->revision,e->screen_manifest_sha256);}
    if(b.failed||b.length>VK_USB_MAX_JSON_BYTES) return ESP_ERR_INVALID_SIZE;
    *length=b.length;return ESP_OK;
}

static bool code_allowed(vk_usb_error_operation_t operation,const char*code)
{
    static const char*const asset[]={"invalid_request","unavailable","wrong_epoch","busy","conflict","not_found","bad_offset","bad_size","bad_hash","kind_mismatch","write_failed","incomplete","invalid_asset","timeout","no_space","referenced","revision_conflict","snapshot_expired","partition_mismatch","not_erased","format_failed","internal"};
    static const char*const screen[]={"invalid_request","unavailable","wrong_epoch","revision_conflict","conflict","invalid_manifest","missing_asset","font_mismatch","limit_exceeded","allocation_failed","render_failed","internal"};
    static const char*const widget[]={"not_configured","wrong_revision","not_found","stale_sequence","type_mismatch","out_of_range","too_large","invalid_state","internal"};
    const char*const*values=operation==VK_USB_ERROR_WIDGET?widget:operation==VK_USB_ERROR_SCREEN?screen:asset;
    size_t count=operation==VK_USB_ERROR_WIDGET?sizeof(widget)/sizeof(*widget):operation==VK_USB_ERROR_SCREEN?sizeof(screen)/sizeof(*screen):sizeof(asset)/sizeof(*asset);for(size_t i=0;i<count;++i)if(strcmp(code,values[i])==0)return true;return false;
}
static bool safe_message(const char*s){if(s==NULL)return true;size_t n=strlen(s);if(n>VK_USB_PROTOCOL_MESSAGE_MAX_BYTES)return false;for(size_t i=0;i<n;++i)if((unsigned char)s[i]<0x20U||s[i]=='"'||s[i]=='\\')return false;return true;}
esp_err_t vk_usb_protocol_error_encode(const vk_usb_protocol_error_t*e,char*out,size_t cap,size_t*length)
{
    if(e==NULL||out==NULL||length==NULL||e->operation>VK_USB_ERROR_WIDGET||e->code==NULL||!code_allowed(e->operation,e->code)||!safe_message(e->message)||(e->sha256&&!valid_sha(e->sha256))||((e->operation==VK_USB_ERROR_SCREEN||e->operation==VK_USB_ERROR_WIDGET)&&(e->has_transfer_id||e->has_next_offset||e->sha256)))return ESP_ERR_INVALID_ARG;
    static const char*const operations[]={"asset","storage","screen","widget"};builder_t b={out,cap,0U,false};append(&b,"{\"code\":\"%s\",\"event\":\"vk_error\"",e->code);if(e->message)append(&b,",\"message\":\"%s\"",e->message);if(e->has_next_offset)append(&b,",\"next_offset\":%"PRIu32,e->next_offset);append(&b,",\"operation\":\"%s\"",operations[e->operation]);if(e->sha256)append(&b,",\"sha256\":\"%s\"",e->sha256);if(e->has_transfer_id){if(e->transfer_id==0U)return ESP_ERR_INVALID_ARG;append(&b,",\"transfer_id\":%"PRIu32,e->transfer_id);}append(&b,"}");if(b.failed||b.length>VK_USB_MAX_JSON_BYTES)return ESP_ERR_INVALID_SIZE;*length=b.length;return ESP_OK;
}
