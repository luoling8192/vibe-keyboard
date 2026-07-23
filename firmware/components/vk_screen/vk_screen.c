#include "vk_screen.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_asset_store.h"

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif
#ifndef ESP_ERR_NOT_ALLOWED
#define ESP_ERR_NOT_ALLOWED 0x10b
#endif

#define NO_PARENT UINT16_MAX

typedef struct {
    vk_screen_t *screen;
    const vk_usb_json_document_t *document;
    vk_screen_model_t *model;
    uint16_t assets_node;
} model_builder_t;

static bool lock_screen(vk_screen_t *screen)
{
    return screen != NULL && !atomic_flag_test_and_set_explicit(&screen->owner, memory_order_acquire);
}

static void unlock_screen(vk_screen_t *screen)
{
    atomic_flag_clear_explicit(&screen->owner, memory_order_release);
}

static uint16_t find(const vk_usb_json_document_t *document, uint16_t object, const char *key)
{
    return vk_usb_json_object_find(document, object, key);
}

static bool exact(const vk_usb_json_document_t *document, uint16_t object,
                  const char *const *keys, size_t count)
{
    return vk_usb_json_kind(document, object) == VK_USB_JSON_OBJECT &&
           vk_usb_json_object_exact_keys(document, object, keys, count);
}

static bool string_value(const vk_usb_json_document_t *document, uint16_t node,
                         char *output, size_t capacity)
{
    return vk_usb_json_string_copy(document, node, output, capacity) == VK_USB_JSON_OK;
}

static bool uint32_value(const vk_usb_json_document_t *document, uint16_t node,
                         uint32_t maximum, bool nonzero, uint32_t *output)
{
    return vk_usb_json_uint32(document, node, maximum, nonzero, output) == VK_USB_JSON_OK;
}

static bool int16_value(const vk_usb_json_document_t *document, uint16_t node, int16_t *output)
{
    int64_t value;
    if (vk_usb_json_int64(document, node, INT16_MIN, INT16_MAX, &value) != VK_USB_JSON_OK) return false;
    *output = (int16_t)value;
    return true;
}

static bool valid_identifier(const char *value)
{
    size_t length = value == NULL ? 0U : strlen(value);
    if (length == 0U || length > VK_SCREEN_MAX_ID_BYTES) return false;
    for (size_t index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') || byte == '_' || byte == '-')) return false;
    }
    return true;
}

static bool valid_sha(const char *value)
{
    if (value == NULL || strlen(value) != 64U) return false;
    for (size_t index = 0U; index < 64U; ++index) {
        char byte = value[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
    }
    return true;
}

static bool checked_add_u32(uint32_t left, uint32_t right, uint32_t *result)
{
    if (left > UINT32_MAX - right) return false;
    *result = left + right;
    return true;
}

bool vk_screen_revision_is_newer(uint32_t candidate, uint32_t current)
{
    uint32_t delta = candidate - current;
    return delta != 0U && delta < 0x80000000U;
}

static bool sequence_is_newer(uint32_t candidate, uint32_t current)
{
    return vk_screen_revision_is_newer(candidate, current);
}

static bool manifest_has_asset(const model_builder_t *builder, const char *sha256,
                               vk_vka1_kind_t required)
{
    uint16_t entries = find(builder->document, builder->assets_node, "assets");
    for (uint16_t entry = vk_usb_json_first_child(builder->document, entries);
         entry != VK_USB_JSON_NO_NODE; entry = vk_usb_json_next_sibling(builder->document, entry)) {
        char hash[65], kind[20];
        if (string_value(builder->document, find(builder->document, entry, "sha256"), hash, sizeof(hash)) &&
            string_value(builder->document, find(builder->document, entry, "kind"), kind, sizeof(kind)) &&
            strcmp(hash, sha256) == 0) {
            return (required == VK_VKA1_IMAGE && strcmp(kind, "image") == 0) ||
                   (required == VK_VKA1_ANIMATION && strcmp(kind, "animation") == 0) ||
                   (required == VK_VKA1_GLYPH_BITMAP && strcmp(kind, "glyph_bitmap") == 0);
        }
    }
    return false;
}

static esp_err_t add_charge(model_builder_t *builder, const char *sha256,
                            vk_vka1_kind_t required, bool pet_transition)
{
    if (!valid_sha(sha256) || builder->screen->config.assets.resolve == NULL ||
        !manifest_has_asset(builder, sha256, required)) return ESP_ERR_NOT_FOUND;
    vk_screen_asset_info_t info = {0};
    esp_err_t result = builder->screen->config.assets.resolve(builder->screen->config.assets.context,
                                                               sha256, &info);
    if (result != ESP_OK || info.kind != required || info.width == 0U || info.height == 0U ||
        info.frame_count == 0U || info.decoded_bytes_per_frame == 0U) {
        if (result == ESP_OK && builder->screen->config.assets.release != NULL) builder->screen->config.assets.release(builder->screen->config.assets.context, &info);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t charge = info.decoded_bytes_per_frame, aggregate = 0U;
    bool admitted = (!pet_transition || checked_add_u32(charge, info.decoded_bytes_per_frame, &charge)) &&
                    checked_add_u32(builder->model->decoded_charge_bytes, charge, &aggregate) &&
                    aggregate <= builder->screen->config.root_budget_bytes;
    if (builder->screen->config.assets.release != NULL) builder->screen->config.assets.release(builder->screen->config.assets.context, &info);
    if (!admitted) return ESP_ERR_NO_MEM;
    builder->model->decoded_charge_bytes = aggregate;
    return ESP_OK;
}

static bool object_id_unique(const vk_screen_model_t *model, const char *id)
{
    for (uint16_t index = 0U; index < model->object_count; ++index) {
        if (strcmp(model->objects[index].id, id) == 0) return false;
    }
    return true;
}

static bool widget_id_unique(const vk_screen_model_t *model, const char *id)
{
    for (uint16_t index = 0U; index < model->widget_count; ++index) {
        if (strcmp(model->widgets[index].id, id) == 0) return false;
    }
    return true;
}

static bool scalar_supported(uint32_t scalar)
{
    return scalar >= 0x20U && scalar <= 0x7eU;
}

static bool text_supported(const char *text)
{
    const uint8_t *bytes = (const uint8_t *)text;
    size_t length = strlen(text);
    for (size_t index = 0U; index < length;) {
        uint32_t scalar;uint8_t first = bytes[index++];
        if (first < 0x80U) scalar = first;
        else if (first >= 0xc2U && first <= 0xdfU && index < length && (bytes[index] & 0xc0U) == 0x80U) {
            scalar = ((uint32_t)(first & 0x1fU) << 6U) | (uint32_t)(bytes[index++] & 0x3fU);
        } else if (first >= 0xe0U && first <= 0xefU && index + 1U < length &&
                   (bytes[index] & 0xc0U) == 0x80U && (bytes[index + 1U] & 0xc0U) == 0x80U) {
            scalar = ((uint32_t)(first & 0x0fU) << 12U) | ((uint32_t)(bytes[index] & 0x3fU) << 6U) |
                     (uint32_t)(bytes[index + 1U] & 0x3fU);index += 2U;
        } else return false;
        if (!scalar_supported(scalar)) return false;
    }
    return true;
}

esp_err_t vk_screen_font_glyph_origin(const char *font_id, uint16_t version,
                                      const char *metrics_sha256, uint32_t scalar,
                                      int16_t pen_x, int16_t *glyph_x, uint16_t *advance)
{
    if (font_id == NULL || metrics_sha256 == NULL || glyph_x == NULL || advance == NULL ||
        strcmp(font_id, "vk-sans") != 0 || version != 1U ||
        strcmp(metrics_sha256, VK_SCREEN_FONT_METRICS_SHA256) != 0) return ESP_ERR_NOT_FOUND;
    static const uint8_t advances[] = {
        4U, 4U, 6U, 10U, 9U, 12U, 10U, 3U, 5U, 5U, 6U, 8U, 3U, 5U, 3U, 5U,
        9U, 5U, 8U, 8U, 9U, 8U, 9U, 8U, 9U, 9U, 3U, 3U, 8U, 8U, 8U, 8U,
        15U, 10U, 11U, 10U, 12U, 9U, 9U, 11U, 11U, 4U, 7U, 10U, 8U, 13U, 11U, 12U,
        10U, 12U, 10U, 9U, 8U, 11U, 10U, 16U, 9U, 9U, 9U, 5U, 5U, 5U, 8U, 7U,
        8U, 8U, 10U, 8U, 10U, 9U, 5U, 10U, 10U, 4U, 4U, 9U, 4U, 15U, 10U, 9U,
        10U, 10U, 6U, 7U, 6U, 10U, 8U, 13U, 8U, 8U, 7U, 5U, 4U, 5U, 8U,
    };
    if (!scalar_supported(scalar)) return ESP_ERR_NOT_SUPPORTED;
    *advance = advances[scalar - 0x20U];
    *glyph_x = pen_x;
    return ESP_OK;
}

static bool font_valid(const vk_screen_t *screen, const vk_usb_json_document_t *document,
                       uint16_t node)
{
    const char *const keys[] = {"id", "version"};
    char id[33];uint32_t version;
    if (!exact(document, node, keys, 2U) ||
        !string_value(document, find(document, node, "id"), id, sizeof(id)) ||
        !uint32_value(document, find(document, node, "version"), UINT16_MAX, true, &version)) return false;
    for (uint16_t index = 0U; index < screen->config.capability.font_count; ++index) {
        const vk_usb_font_capability_t *font = &screen->config.capability.fonts[index];
        if (strcmp(font->id, id) == 0 && font->version == version &&
            strcmp(font->metrics_sha256, VK_SCREEN_FONT_METRICS_SHA256) == 0) return true;
    }
    return false;
}

static bool parse_base(model_builder_t *builder, uint16_t node, bool root,
                       vk_screen_object_t *object, const char *const *extra_keys,
                       size_t extra_count)
{
    static const char *const base_child[] = {"clip", "height", "id", "type", "visible", "width", "z"};
    static const char *const base_root[] = {"clip", "height", "id", "type", "visible", "width", "x", "y", "z"};
    const char *keys[24];size_t count = 0U;
    const char *const *base = root ? base_root : base_child;
    size_t base_count = root ? 9U : 7U;
    for (size_t index = 0U; index < base_count; ++index) keys[count++] = base[index];
    for (size_t index = 0U; index < extra_count; ++index) keys[count++] = extra_keys[index];
    char type[24];uint32_t width, height;bool clip, visible;
    if (count > sizeof(keys) / sizeof(keys[0]) || !exact(builder->document, node, keys, count) ||
        !string_value(builder->document, find(builder->document, node, "id"), object->id, sizeof(object->id)) ||
        !valid_identifier(object->id) || !object_id_unique(builder->model, object->id) ||
        !string_value(builder->document, find(builder->document, node, "type"), type, sizeof(type)) ||
        !uint32_value(builder->document, find(builder->document, node, "width"), UINT16_MAX, true, &width) ||
        !uint32_value(builder->document, find(builder->document, node, "height"), UINT16_MAX, true, &height) ||
        !int16_value(builder->document, find(builder->document, node, "z"), &object->z) ||
        !vk_usb_json_boolean(builder->document, find(builder->document, node, "clip"), &clip) ||
        !vk_usb_json_boolean(builder->document, find(builder->document, node, "visible"), &visible)) return false;
    object->rect.width = (uint16_t)width;object->rect.height = (uint16_t)height;
    object->clip = clip;object->visible = visible;
    if (root && (!int16_value(builder->document, find(builder->document, node, "x"), &object->rect.x) ||
                 !int16_value(builder->document, find(builder->document, node, "y"), &object->rect.y))) return false;
    int32_t right = (int32_t)object->rect.x + object->rect.width;
    int32_t bottom = (int32_t)object->rect.y + object->rect.height;
    if (root && (object->rect.x < 0 || object->rect.y < 0 || right > (int32_t)VK_SCREEN_WIDTH ||
                 bottom > (int32_t)VK_SCREEN_HEIGHT)) return false;
    static const char *const names[] = {"image","pet","static_label","glyph_label","dynamic_label","progress","icon_text","row","column"};
    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcmp(type, names[index]) == 0) {object->type = (vk_screen_object_type_t)index;return true;}
    }
    return false;
}

static esp_err_t parse_pet_states(model_builder_t *builder, uint16_t pet, char selected_sha256[65])
{
    const char *const keys[] = {"id", "states", "version"};
    char id[33];uint32_t version;uint16_t states = find(builder->document, pet, "states");
    if (!exact(builder->document, pet, keys, 3U) ||
        !string_value(builder->document, find(builder->document, pet, "id"), id, sizeof(id)) ||
        !valid_identifier(id) ||
        !uint32_value(builder->document, find(builder->document, pet, "version"), 1U, true, &version) ||
        version != 1U || vk_usb_json_kind(builder->document, states) != VK_USB_JSON_OBJECT) return ESP_ERR_INVALID_ARG;
    size_t count = 0U; bool idle = false; uint32_t first_charge = 0U, second_charge = 0U;
    for (uint16_t key = vk_usb_json_first_child(builder->document, states); key != VK_USB_JSON_NO_NODE;) {
        uint16_t value = vk_usb_json_next_sibling(builder->document, key); char state[16];
        if (value == VK_USB_JSON_NO_NODE || !vk_usb_json_node_is_object_key(builder->document, key) ||
            !string_value(builder->document, key, state, sizeof(state))) return ESP_ERR_INVALID_ARG;
        bool allowed = strcmp(state,"idle")==0||strcmp(state,"active")==0||strcmp(state,"recording")==0||
                       strcmp(state,"thinking")==0||strcmp(state,"success")==0||strcmp(state,"error")==0;
        if (!allowed || ++count > builder->screen->config.capability.max_pet_states) return ESP_ERR_INVALID_ARG;
        const char *const sha_keys[] = {"sha256"}; const char *const fallback_keys[] = {"fallback"};
        char sha[65], fallback[8];
        bool has_sha = exact(builder->document,value,sha_keys,1U) && string_value(builder->document,find(builder->document,value,"sha256"),sha,sizeof(sha)) && valid_sha(sha);
        bool has_fallback = exact(builder->document,value,fallback_keys,1U) && string_value(builder->document,find(builder->document,value,"fallback"),fallback,sizeof(fallback)) && strcmp(fallback,"idle")==0;
        if ((!has_sha && !has_fallback) || (strcmp(state,"idle")==0 && !has_sha)) return ESP_ERR_INVALID_ARG;
        if (has_sha) {
            if (!manifest_has_asset(builder,sha,VK_VKA1_IMAGE) && !manifest_has_asset(builder,sha,VK_VKA1_ANIMATION)) return ESP_ERR_NOT_FOUND;
            vk_screen_asset_info_t info={0}; esp_err_t result=builder->screen->config.assets.resolve(builder->screen->config.assets.context,sha,&info);
            if(result!=ESP_OK||(info.kind!=VK_VKA1_IMAGE&&info.kind!=VK_VKA1_ANIMATION))return ESP_ERR_INVALID_RESPONSE;
            uint32_t charge=info.decoded_bytes_per_frame;
            if(info.kind==VK_VKA1_ANIMATION&&info.frame_count>1U&&!checked_add_u32(charge,charge,&charge))return ESP_ERR_NO_MEM;
            if(charge>=first_charge){second_charge=first_charge;first_charge=charge;}else if(charge>second_charge)second_charge=charge;
            if (selected_sha256 != NULL && strcmp(state, "idle") == 0) snprintf(selected_sha256, 65U, "%s", sha);
        }
        idle |= strcmp(state,"idle")==0; key=vk_usb_json_next_sibling(builder->document,value);
    }
    uint32_t pair=0U, aggregate=0U;
    if(!idle||!checked_add_u32(first_charge,second_charge,&pair)||!checked_add_u32(builder->model->decoded_charge_bytes,pair,&aggregate))return idle?ESP_ERR_NO_MEM:ESP_ERR_INVALID_ARG;
    builder->model->decoded_charge_bytes=aggregate;return ESP_OK;
}

static esp_err_t parse_object(model_builder_t *builder, uint16_t node, bool root,
                              uint16_t parent, uint8_t depth);

static esp_err_t parse_container(model_builder_t *builder, uint16_t node,
                                 vk_screen_object_t *object, uint16_t object_index,
                                 uint8_t depth)
{
    const vk_usb_json_document_t *document = builder->document;
    uint16_t children_node = find(document, node, "children");
    uint32_t gap;char main_align[8], cross_align[8];
    if (vk_usb_json_kind(document, children_node) != VK_USB_JSON_ARRAY ||
        vk_usb_json_array_count(document, children_node) == 0U ||
        !uint32_value(document, find(document,node,"gap"),UINT16_MAX,false,&gap) ||
        !string_value(document,find(document,node,"main_align"),main_align,sizeof(main_align)) ||
        !string_value(document,find(document,node,"cross_align"),cross_align,sizeof(cross_align))) return ESP_ERR_INVALID_ARG;
    uint8_t main = strcmp(main_align,"start")==0?0U:strcmp(main_align,"center")==0?1U:strcmp(main_align,"end")==0?2U:3U;
    uint8_t cross = strcmp(cross_align,"start")==0?0U:strcmp(cross_align,"center")==0?1U:strcmp(cross_align,"end")==0?2U:3U;
    if (main > 2U || cross > 2U) return ESP_ERR_INVALID_ARG;
    size_t first = builder->model->object_count;
    for (uint16_t child = vk_usb_json_first_child(document, children_node); child != VK_USB_JSON_NO_NODE;
         child = vk_usb_json_next_sibling(document, child)) {
        esp_err_t result = parse_object(builder, child, false, object_index, (uint8_t)(depth + 1U));
        if (result != ESP_OK) return result;
    }
    size_t direct_count = 0U; uint16_t direct_indices[VK_SCREEN_MAX_OBJECTS];
    for (size_t index = first; index < builder->model->object_count; ++index) {
        if (builder->model->objects[index].parent == object_index) direct_indices[direct_count++] = (uint16_t)index;
    }
    vk_screen_rect_t rects[VK_SCREEN_MAX_OBJECTS];
    for (size_t index = 0U; index < direct_count; ++index) rects[index] = builder->model->objects[direct_indices[index]].rect;
    esp_err_t result = vk_screen_layout_place(object->type == VK_SCREEN_OBJECT_ROW,
                                               object->rect.x, object->rect.y,
                                               object->rect.width, object->rect.height,
                                               (uint16_t)gap, main, cross, rects, direct_count);
    if (result != ESP_OK) return result;
    for (size_t index = 0U; index < direct_count; ++index) {
        vk_screen_object_t *child = &builder->model->objects[direct_indices[index]];
        int16_t dx = (int16_t)(rects[index].x - child->rect.x), dy = (int16_t)(rects[index].y - child->rect.y);
        child->rect = rects[index];
        for (size_t descendant = direct_indices[index] + 1U; descendant < builder->model->object_count; ++descendant) {
            uint16_t parent_index = builder->model->objects[descendant].parent; bool owned = false;
            while (parent_index != NO_PARENT && parent_index < builder->model->object_count) {
                if (parent_index == direct_indices[index]) { owned = true; break; }
                parent_index = builder->model->objects[parent_index].parent;
            }
            if (owned) { builder->model->objects[descendant].rect.x += dx; builder->model->objects[descendant].rect.y += dy; }
        }
    }
    return ESP_OK;
}

static esp_err_t parse_object(model_builder_t *builder, uint16_t node, bool root,
                              uint16_t parent, uint8_t depth)
{
    if (depth == 0U || depth > builder->screen->config.capability.max_depth ||
        builder->model->object_count >= builder->screen->config.capability.max_objects ||
        builder->model->object_count >= VK_SCREEN_MAX_OBJECTS) return ESP_ERR_INVALID_SIZE;
    char type[24];
    if (!string_value(builder->document, find(builder->document,node,"type"), type, sizeof(type))) return ESP_ERR_INVALID_ARG;
    static const char *const image[] = {"background_rgb888","fit","sha256"};
    static const char *const pet[] = {"background_rgb888","fit","pet"};
    static const char *const label[] = {"align","color_rgb888","font","overflow","text"};
    static const char *const glyph[] = {"align","color_rgb888","font","glyph","overflow"};
    static const char *const dynamic[] = {"align","color_rgb888","font","overflow","widget_id"};
    static const char *const progress[] = {"background_rgb888","fill_rgb888","widget_id"};
    static const char *const icon[] = {"color_rgb888","font","gap","sha256","widget_id"};
    static const char *const container[] = {"children","cross_align","gap","main_align"};
    const char *const *extras = NULL;size_t extra_count = 0U;
    if(strcmp(type,"image")==0){extras=image;extra_count=3U;}
    else if(strcmp(type,"pet")==0){extras=pet;extra_count=3U;}
    else if(strcmp(type,"static_label")==0){extras=label;extra_count=5U;}
    else if(strcmp(type,"glyph_label")==0){extras=glyph;extra_count=5U;}
    else if(strcmp(type,"dynamic_label")==0){extras=dynamic;extra_count=5U;}
    else if(strcmp(type,"progress")==0){extras=progress;extra_count=3U;}
    else if(strcmp(type,"icon_text")==0){extras=icon;extra_count=5U;}
    else if(strcmp(type,"row")==0||strcmp(type,"column")==0){extras=container;extra_count=4U;}
    else return ESP_ERR_NOT_SUPPORTED;
    uint16_t index = builder->model->object_count;
    vk_screen_object_t *object = &builder->model->objects[index];memset(object,0,sizeof(*object));
    if (!parse_base(builder,node,root,object,extras,extra_count)) return ESP_ERR_INVALID_ARG;
    object->parent=parent;object->source_order=index;builder->model->object_count++;
    char sha[65], text[VK_SCREEN_MAX_TEXT_BYTES+1U], widget[33], fit[12], align[8], overflow[8];uint32_t rgb;
    if (object->type == VK_SCREEN_OBJECT_IMAGE) {
        if (!string_value(builder->document,find(builder->document,node,"sha256"),sha,sizeof(sha)) ||
            !string_value(builder->document,find(builder->document,node,"fit"),fit,sizeof(fit)) ||
            !(strcmp(fit,"contain")==0||strcmp(fit,"cover")==0||strcmp(fit,"stretch")==0||strcmp(fit,"center")==0) ||
            !uint32_value(builder->document,find(builder->document,node,"background_rgb888"),0xffffffU,false,&rgb)) return ESP_ERR_INVALID_ARG;
        snprintf(object->sha256,sizeof(object->sha256),"%s",sha);object->background_rgb888=rgb;
        return add_charge(builder,sha,VK_VKA1_IMAGE,false);
    }
    if (object->type == VK_SCREEN_OBJECT_PET) {
        if (!string_value(builder->document,find(builder->document,node,"fit"),fit,sizeof(fit)) ||
            !uint32_value(builder->document,find(builder->document,node,"background_rgb888"),0xffffffU,false,&rgb)) return ESP_ERR_INVALID_ARG;
        object->background_rgb888=rgb;return parse_pet_states(builder,find(builder->document,node,"pet"),object->sha256);
    }
    if (object->type == VK_SCREEN_OBJECT_STATIC_LABEL || object->type == VK_SCREEN_OBJECT_DYNAMIC_LABEL) {
        if (!font_valid(builder->screen,builder->document,find(builder->document,node,"font")) ||
            !string_value(builder->document,find(builder->document,node,"align"),align,sizeof(align)) ||
            !string_value(builder->document,find(builder->document,node,"overflow"),overflow,sizeof(overflow)) || strcmp(overflow,"clip")!=0 ||
            !uint32_value(builder->document,find(builder->document,node,"color_rgb888"),0xffffffU,false,&rgb)) return ESP_ERR_INVALID_ARG;
        object->color_rgb888=rgb;
        object->align = strcmp(align, "left") == 0 ? 0U : strcmp(align, "center") == 0 ? 1U : strcmp(align, "right") == 0 ? 2U : UINT8_MAX;
        object->overflow_clip = strcmp(overflow, "clip") == 0;
        if (object->align == UINT8_MAX) return ESP_ERR_INVALID_ARG;
        if (object->type == VK_SCREEN_OBJECT_STATIC_LABEL) {
            if (!string_value(builder->document,find(builder->document,node,"text"),text,sizeof(text)) || !text_supported(text)) return ESP_ERR_INVALID_ARG;
            snprintf(object->text, sizeof(object->text), "%s", text);
        } else {
            if (!string_value(builder->document,find(builder->document,node,"widget_id"),widget,sizeof(widget)) || !valid_identifier(widget)) return ESP_ERR_INVALID_ARG;
            snprintf(object->widget_id,sizeof(object->widget_id),"%s",widget);
        }
        return ESP_OK;
    }
    if (object->type == VK_SCREEN_OBJECT_GLYPH_LABEL) {
        const char *const glyph_keys[]={"advance","baseline","bearing_x","bearing_y","sha256"};uint32_t advance;int16_t metric;
        uint16_t glyph_node=find(builder->document,node,"glyph");
        if(!vk_usb_json_is_null(builder->document,find(builder->document,node,"font"))||!exact(builder->document,glyph_node,glyph_keys,5U)||
           !uint32_value(builder->document,find(builder->document,glyph_node,"advance"),UINT16_MAX,true,&advance)||
           !int16_value(builder->document,find(builder->document,glyph_node,"baseline"),&metric)||
           !int16_value(builder->document,find(builder->document,glyph_node,"bearing_x"),&metric)||
           !int16_value(builder->document,find(builder->document,glyph_node,"bearing_y"),&metric)||
           !string_value(builder->document,find(builder->document,glyph_node,"sha256"),sha,sizeof(sha)))return ESP_ERR_INVALID_ARG;
        snprintf(object->sha256,sizeof(object->sha256),"%s",sha);return add_charge(builder,sha,VK_VKA1_GLYPH_BITMAP,false);
    }
    if (object->type == VK_SCREEN_OBJECT_PROGRESS || object->type == VK_SCREEN_OBJECT_ICON_TEXT) {
        if (!string_value(builder->document,find(builder->document,node,"widget_id"),widget,sizeof(widget)) || !valid_identifier(widget)) return ESP_ERR_INVALID_ARG;
        snprintf(object->widget_id,sizeof(object->widget_id),"%s",widget);
        if (object->type == VK_SCREEN_OBJECT_ICON_TEXT) {
            uint32_t gap;
            if (!font_valid(builder->screen,builder->document,find(builder->document,node,"font")) ||
                !string_value(builder->document,find(builder->document,node,"sha256"),sha,sizeof(sha)) ||
                !uint32_value(builder->document,find(builder->document,node,"gap"),UINT16_MAX,false,&gap)) return ESP_ERR_INVALID_ARG;
            snprintf(object->sha256,sizeof(object->sha256),"%s",sha);return add_charge(builder,sha,VK_VKA1_IMAGE,false);
        }
        return ESP_OK;
    }
    return parse_container(builder,node,object,index,depth);
}

static esp_err_t parse_widgets(model_builder_t *builder, uint16_t widgets_node)
{
    if (vk_usb_json_kind(builder->document,widgets_node)!=VK_USB_JSON_ARRAY ||
        vk_usb_json_array_count(builder->document,widgets_node)>builder->screen->config.capability.max_widgets ||
        vk_usb_json_array_count(builder->document,widgets_node)>VK_SCREEN_MAX_WIDGETS) return ESP_ERR_INVALID_SIZE;
    for(uint16_t node=vk_usb_json_first_child(builder->document,widgets_node);node!=VK_USB_JSON_NO_NODE;node=vk_usb_json_next_sibling(builder->document,node)){
        char type[16];if(!string_value(builder->document,find(builder->document,node,"type"),type,sizeof(type)))return ESP_ERR_INVALID_ARG;
        vk_screen_widget_t *widget=&builder->model->widgets[builder->model->widget_count];memset(widget,0,sizeof(*widget));
        if(strcmp(type,"text")==0){const char*const keys[]={"fallback","id","target","type"};if(!exact(builder->document,node,keys,4U)||!string_value(builder->document,find(builder->document,node,"fallback"),widget->fallback_text,sizeof(widget->fallback_text))||strlen(widget->fallback_text)>builder->screen->config.capability.max_widget_value_bytes||!text_supported(widget->fallback_text))return ESP_ERR_INVALID_ARG;widget->type=VK_SCREEN_WIDGET_TEXT;}
        else if(strcmp(type,"integer")==0){const char*const keys[]={"fallback","id","target","type"};int64_t value;if(!exact(builder->document,node,keys,4U)||vk_usb_json_int64(builder->document,find(builder->document,node,"fallback"),INT64_MIN,INT64_MAX,&value)!=VK_USB_JSON_OK)return ESP_ERR_INVALID_ARG;widget->type=VK_SCREEN_WIDGET_INTEGER;widget->fallback_milli=value;}
        else if(strcmp(type,"number")==0||strcmp(type,"progress")==0){const char*const keys[]={"fallback","format","id","max","min","target","type"};const char*const format_keys[]={"decimals"};int64_t min,max,fallback;uint32_t decimals;uint16_t format=find(builder->document,node,"format");if(!exact(builder->document,node,keys,7U)||!exact(builder->document,format,format_keys,1U)||vk_usb_json_int64(builder->document,find(builder->document,node,"min"),INT64_MIN,INT64_MAX,&min)!=VK_USB_JSON_OK||vk_usb_json_int64(builder->document,find(builder->document,node,"max"),INT64_MIN,INT64_MAX,&max)!=VK_USB_JSON_OK||vk_usb_json_int64(builder->document,find(builder->document,node,"fallback"),INT64_MIN,INT64_MAX,&fallback)!=VK_USB_JSON_OK||!uint32_value(builder->document,find(builder->document,format,"decimals"),3U,false,&decimals)||min>=max||fallback<min||fallback>max)return ESP_ERR_INVALID_ARG;widget->type=strcmp(type,"number")==0?VK_SCREEN_WIDGET_NUMBER:VK_SCREEN_WIDGET_PROGRESS;widget->minimum_milli=min;widget->maximum_milli=max;widget->fallback_milli=fallback;widget->decimals=(uint8_t)decimals;}
        else return ESP_ERR_NOT_SUPPORTED;
        if(!string_value(builder->document,find(builder->document,node,"id"),widget->id,sizeof(widget->id))||!string_value(builder->document,find(builder->document,node,"target"),widget->target,sizeof(widget->target))||!valid_identifier(widget->id)||!valid_identifier(widget->target)||!widget_id_unique(builder->model,widget->id))return ESP_ERR_INVALID_ARG;
        uint16_t matches=0U;vk_screen_object_t *target=NULL;for(uint16_t i=0;i<builder->model->object_count;i++){vk_screen_object_t*object=&builder->model->objects[i];if(strcmp(object->widget_id,widget->id)==0&&strcmp(object->id,widget->target)==0){matches++;target=object;}}
        bool compatible=target!=NULL&&((widget->type==VK_SCREEN_WIDGET_PROGRESS&&target->type==VK_SCREEN_OBJECT_PROGRESS)||(widget->type!=VK_SCREEN_WIDGET_PROGRESS&&(target->type==VK_SCREEN_OBJECT_DYNAMIC_LABEL||target->type==VK_SCREEN_OBJECT_ICON_TEXT)));
        if(matches!=1U||!compatible)return ESP_ERR_INVALID_ARG;
        builder->model->widget_count++;
    }
    for(uint16_t i=0;i<builder->model->object_count;i++){vk_screen_object_t*object=&builder->model->objects[i];if(object->widget_id[0]){uint16_t count=0;for(uint16_t w=0;w<builder->model->widget_count;w++)if(strcmp(builder->model->widgets[w].id,object->widget_id)==0&&strcmp(builder->model->widgets[w].target,object->id)==0)count++;if(count!=1U)return ESP_ERR_INVALID_ARG;}}
    return ESP_OK;
}

static esp_err_t build_model(vk_screen_t *screen, const vk_usb_screen_command_t *command,
                             vk_screen_model_t *model)
{
    memset(model,0,sizeof(*model));model->configured=true;model->configured_mode=command->configured_mode;
    model->revision=command->revision;model->previous_revision=command->expected_revision;
    model_builder_t builder={.screen=screen,.document=command->document,.model=model,.assets_node=command->assets_node};
    const vk_usb_json_document_t *document=command->document;uint16_t screen_node=command->screen_node;
    esp_err_t result=ESP_OK;
    if(command->configured_mode==VK_USB_SCREEN_IMAGE){
        uint16_t image=find(document,screen_node,"image");char sha[65], fit[12];uint32_t background=0U;
        if(!string_value(document,find(document,image,"sha256"),sha,sizeof(sha)) ||
           !string_value(document,find(document,image,"fit"),fit,sizeof(fit)) ||
           !uint32_value(document,find(document,image,"background_rgb888"),0xffffffU,false,&background))return ESP_ERR_INVALID_ARG;
        vk_screen_object_t *object=&model->objects[model->object_count++];
        *object=(vk_screen_object_t){.type=VK_SCREEN_OBJECT_IMAGE,.rect={0,0,VK_SCREEN_WIDTH,VK_SCREEN_HEIGHT},.parent=NO_PARENT,.visible=true,.clip=true,.background_rgb888=background};
        snprintf(object->id,sizeof(object->id),"screen-image");snprintf(object->sha256,sizeof(object->sha256),"%s",sha);
        object->fit = strcmp(fit,"contain") == 0 ? 0U : strcmp(fit,"cover") == 0 ? 1U : strcmp(fit,"stretch") == 0 ? 2U : strcmp(fit,"center") == 0 ? 3U : UINT8_MAX;
        if (object->fit == UINT8_MAX) return ESP_ERR_INVALID_ARG;
        model->background_rgb888 = background;
        result = add_charge(&builder, sha, VK_VKA1_IMAGE, false);
    }
    else if(command->configured_mode==VK_USB_SCREEN_PET){
        vk_screen_object_t *object=&model->objects[model->object_count++];
        *object=(vk_screen_object_t){.type=VK_SCREEN_OBJECT_PET,.rect={0,0,VK_SCREEN_WIDTH,VK_SCREEN_HEIGHT},.parent=NO_PARENT,.visible=true,.clip=true};
        snprintf(object->id,sizeof(object->id),"screen-pet");result=parse_pet_states(&builder,find(document,screen_node,"pet"),object->sha256);
    }
    else {uint16_t layout=find(document,screen_node,"layout"),objects=find(document,layout,"objects"),widgets=find(document,layout,"widgets");uint32_t background;if(!uint32_value(document,find(document,layout,"background_rgb888"),0xffffffU,false,&background))return ESP_ERR_INVALID_ARG;model->background_rgb888=background;for(uint16_t object=vk_usb_json_first_child(document,objects);result==ESP_OK&&object!=VK_USB_JSON_NO_NODE;object=vk_usb_json_next_sibling(document,object))result=parse_object(&builder,object,true,NO_PARENT,1U);if(result==ESP_OK)result=parse_widgets(&builder,widgets);}
    uint32_t candidate_peak=model->decoded_charge_bytes,current=screen->current.decoded_charge_bytes,total;
    if(result==ESP_OK&&(!checked_add_u32(current,candidate_peak,&total)||!checked_add_u32(total,screen->config.decoder_scratch_bytes,&total)||total>screen->config.root_budget_bytes))result=ESP_ERR_NO_MEM;
    if(result!=ESP_OK)return result;
    size_t as,ae,ss,se;if(!vk_usb_json_node_range(document,command->assets_node,&as,&ae)||!vk_usb_json_node_range(document,command->screen_node,&ss,&se))return ESP_ERR_INVALID_ARG;
    uint8_t digest[32];vk_asset_sha256(document->bytes+as,ae-as,digest);static const char hex[]="0123456789abcdef";for(size_t i=0;i<32;i++){model->assets_manifest_sha256[i*2]=hex[digest[i]>>4];model->assets_manifest_sha256[i*2+1]=hex[digest[i]&15];}model->assets_manifest_sha256[64]=0;vk_asset_sha256(document->bytes+ss,se-ss,digest);for(size_t i=0;i<32;i++){model->screen_manifest_sha256[i*2]=hex[digest[i]>>4];model->screen_manifest_sha256[i*2+1]=hex[digest[i]&15];}model->screen_manifest_sha256[64]=0;
    return ESP_OK;
}

esp_err_t vk_screen_init(vk_screen_t *screen, const vk_screen_config_t *config)
{
    if(screen==NULL||config==NULL||config->capability.state!=VK_USB_CAPABILITY_AVAILABLE||
       config->capability.max_objects==0U||config->capability.max_objects>VK_SCREEN_MAX_OBJECTS||
       config->capability.max_widgets>VK_SCREEN_MAX_WIDGETS||config->capability.max_depth==0U||
       config->capability.max_depth>VK_USB_JSON_MAX_DEPTH||config->assets.resolve==NULL||
       config->renderer.lock==NULL||config->renderer.unlock==NULL||config->renderer.create_candidate==NULL||
       config->renderer.swap_root==NULL||config->renderer.destroy_root==NULL||config->root_budget_bytes==0U||
       config->decoder_scratch_bytes==0U)return ESP_ERR_INVALID_ARG;
    memset(screen,0,sizeof(*screen));screen->config=*config;atomic_flag_clear(&screen->owner);atomic_init(&screen->stopping,false);return ESP_OK;
}

esp_err_t vk_screen_stop(vk_screen_t *screen)
{
    if (screen == NULL) return ESP_ERR_INVALID_ARG;
    atomic_store_explicit(&screen->stopping, true, memory_order_release);
    if (!lock_screen(screen)) return ESP_ERR_TIMEOUT;
    if (screen->config.renderer.lock(screen->config.renderer.context) != ESP_OK) {
        unlock_screen(screen);
        return ESP_ERR_TIMEOUT;
    }
    void *root = screen->current_root;
    screen->current_root = NULL;
    screen->current = (vk_screen_model_t){0};
    screen->overlay_count = 0U;
    screen->bound_epoch = 0U;
    screen->bound_snapshot_generation = 0U;
    screen->config.renderer.unlock(screen->config.renderer.context);
    if (root != NULL) screen->config.renderer.destroy_root(screen->config.renderer.context, root);
    unlock_screen(screen);
    return ESP_OK;
}

static void set_error_stage(vk_screen_t *screen, const char *stage)
{
    snprintf(screen->last_error_stage, sizeof(screen->last_error_stage), "%s", stage);
}

const char *vk_screen_last_error_stage(const vk_screen_t *screen)
{
    return screen == NULL || screen->last_error_stage[0] == '\0'
        ? "unknown"
        : screen->last_error_stage;
}

esp_err_t vk_screen_commit(vk_screen_t *screen, const vk_usb_screen_command_t *command,
                           vk_usb_screen_event_t *event)
{
    if (screen == NULL || command == NULL || event == NULL ||
        command->kind != VK_USB_SCREEN_COMMIT || command->expected_epoch == 0U ||
        command->snapshot_generation == 0U ||
        atomic_load_explicit(&screen->stopping, memory_order_acquire)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_screen(screen)) return ESP_ERR_INVALID_STATE;
    screen->last_error_stage[0] = '\0';

    if (screen->current.configured && command->revision == screen->current.revision) {
        size_t assets_start, assets_end, screen_start, screen_end;
        bool ranges =
            vk_usb_json_node_range(command->document, command->assets_node,
                                   &assets_start, &assets_end) &&
            vk_usb_json_node_range(command->document, command->screen_node,
                                   &screen_start, &screen_end);
        bool same =
            command->expected_revision == screen->current.previous_revision &&
            ranges &&
            assets_end - assets_start == screen->selected_assets_manifest_bytes &&
            screen_end - screen_start == screen->selected_screen_manifest_bytes &&
            memcmp(command->document->bytes + assets_start,
                   screen->selected_assets_manifest,
                   assets_end - assets_start) == 0 &&
            memcmp(command->document->bytes + screen_start,
                   screen->selected_screen_manifest,
                   screen_end - screen_start) == 0;
        if (!same) {
            set_error_stage(screen, "replay");
            unlock_screen(screen);
            return ESP_ERR_INVALID_STATE;
        }
        *event = (vk_usb_screen_event_t){
            .kind = VK_USB_SCREEN_EVENT_COMMITTED,
            .previous_revision = screen->current.previous_revision,
            .revision = screen->current.revision,
        };
        snprintf(event->assets_manifest_sha256,
                 sizeof(event->assets_manifest_sha256), "%s",
                 screen->current.assets_manifest_sha256);
        snprintf(event->screen_manifest_sha256,
                 sizeof(event->screen_manifest_sha256), "%s",
                 screen->current.screen_manifest_sha256);
        unlock_screen(screen);
        return ESP_OK;
    }
    if (command->expected_revision != screen->current.revision ||
        !vk_screen_revision_is_newer(command->revision, screen->current.revision)) {
        set_error_stage(screen, "revision");
        unlock_screen(screen);
        return ESP_ERR_INVALID_STATE;
    }

    vk_screen_model_t *candidate = calloc(1U, sizeof(*candidate));
    if (candidate == NULL) {
        set_error_stage(screen, "model_alloc");
        unlock_screen(screen);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = build_model(screen, command, candidate);
    if (result != ESP_OK) set_error_stage(screen, "model_build");

    size_t assets_start = 0U, assets_end = 0U;
    size_t screen_start = 0U, screen_end = 0U;
    if (result == ESP_OK &&
        (!vk_usb_json_node_range(command->document, command->assets_node,
                                 &assets_start, &assets_end) ||
         !vk_usb_json_node_range(command->document, command->screen_node,
                                 &screen_start, &screen_end) ||
         assets_end - assets_start > sizeof(screen->selected_assets_manifest) ||
         screen_end - screen_start > sizeof(screen->selected_screen_manifest))) {
        set_error_stage(screen, "manifest_range");
        result = ESP_ERR_INVALID_SIZE;
    }

    void *new_root = NULL;
    bool renderer_locked = false;
    if (result == ESP_OK) {
        result = screen->config.renderer.lock(screen->config.renderer.context);
        renderer_locked = result == ESP_OK;
        if (result != ESP_OK) set_error_stage(screen, "renderer_lock");
    }
    if (result == ESP_OK) {
        result = screen->config.renderer.create_candidate(
            screen->config.renderer.context, candidate, &new_root);
        if (result != ESP_OK) set_error_stage(screen, "renderer_create");
    }
    if (result == ESP_OK && screen->config.durable_publish != NULL) {
        result = screen->config.durable_publish(
            screen->config.durable_context, command, candidate);
        if (result != ESP_OK) set_error_stage(screen, "durable_publish");
    }
    if (result == ESP_OK) {
        void *old_root = NULL;
        result = screen->config.renderer.swap_root(
            screen->config.renderer.context, new_root, &old_root);
        if (result != ESP_OK) {
            set_error_stage(screen, "renderer_swap");
        } else {
            memcpy(screen->selected_assets_manifest,
                   command->document->bytes + assets_start,
                   assets_end - assets_start);
            screen->selected_assets_manifest_bytes = assets_end - assets_start;
            memcpy(screen->selected_screen_manifest,
                   command->document->bytes + screen_start,
                   screen_end - screen_start);
            screen->selected_screen_manifest_bytes = screen_end - screen_start;
            screen->current = *candidate;
            screen->current_root = new_root;
            screen->bound_epoch = command->expected_epoch;
            screen->bound_snapshot_generation = command->snapshot_generation;
            new_root = old_root;
        }
    }
    if (renderer_locked) {
        screen->config.renderer.unlock(screen->config.renderer.context);
    }
    if (new_root != NULL) {
        screen->config.renderer.destroy_root(
            screen->config.renderer.context, new_root);
    }
    if (result == ESP_OK) {
        *event = (vk_usb_screen_event_t){
            .kind = VK_USB_SCREEN_EVENT_COMMITTED,
            .previous_revision = candidate->previous_revision,
            .revision = candidate->revision,
        };
        snprintf(event->assets_manifest_sha256,
                 sizeof(event->assets_manifest_sha256), "%s",
                 candidate->assets_manifest_sha256);
        snprintf(event->screen_manifest_sha256,
                 sizeof(event->screen_manifest_sha256), "%s",
                 candidate->screen_manifest_sha256);
    }
    free(candidate);
    unlock_screen(screen);
    return result;
}

esp_err_t vk_screen_restore(vk_screen_t *screen, uint32_t revision, uint32_t previous_revision,
                            const uint8_t *assets_manifest, size_t assets_bytes,
                            const uint8_t *screen_manifest, size_t screen_bytes,
                            vk_usb_json_document_t *document,
                            uint8_t *envelope, size_t envelope_capacity)
{
    if (!screen || !revision || !assets_manifest || !assets_bytes || !screen_manifest ||
        !screen_bytes || !document || !envelope || atomic_load_explicit(&screen->stopping, memory_order_acquire)) return ESP_ERR_INVALID_ARG;
    if (screen->current.configured || screen->current_root != NULL) return ESP_ERR_INVALID_STATE;
    if (vk_usb_json_parse(document, assets_manifest, assets_bytes) != VK_USB_JSON_OK) return ESP_ERR_INVALID_RESPONSE;
    uint16_t persisted_assets_root = vk_usb_json_root(document);
    static const char *const assets_keys[] = {"assets", "previous_revision", "revision", "schema"};
    uint32_t manifest_revision = 0U, manifest_previous = 0U, schema = 0U;
    uint16_t assets_array = find(document, persisted_assets_root, "assets"); size_t assets_start = 0U, assets_end = 0U;
    if (!exact(document, persisted_assets_root, assets_keys, 4U) ||
        !uint32_value(document, find(document, persisted_assets_root, "revision"), UINT32_MAX, true, &manifest_revision) || manifest_revision != revision ||
        !uint32_value(document, find(document, persisted_assets_root, "previous_revision"), UINT32_MAX, false, &manifest_previous) || manifest_previous != previous_revision ||
        !uint32_value(document, find(document, persisted_assets_root, "schema"), 1U, true, &schema) || schema != 1U ||
        !vk_usb_json_node_range(document, assets_array, &assets_start, &assets_end)) return ESP_ERR_INVALID_RESPONSE;
    int prefix = snprintf((char *)envelope, envelope_capacity, "{\"assets\":{\"assets\":");
    if (prefix <= 0 || (size_t)prefix >= envelope_capacity || assets_end - assets_start >= envelope_capacity - (size_t)prefix) return ESP_ERR_INVALID_SIZE;
    size_t used = (size_t)prefix; memcpy(envelope + used, assets_manifest + assets_start, assets_end - assets_start); used += assets_end - assets_start;
    int middle = snprintf((char *)envelope + used, envelope_capacity - used,
                          "},\"event\":\"vk_screen_commit\",\"expected_revision\":%u,\"revision\":%u,\"screen\":",
                          (unsigned)previous_revision, (unsigned)revision);
    if (middle <= 0 || (size_t)middle >= envelope_capacity - used) return ESP_ERR_INVALID_SIZE;
    used += (size_t)middle;
    if (vk_usb_json_parse(document, screen_manifest, screen_bytes) != VK_USB_JSON_OK) return ESP_ERR_INVALID_RESPONSE;
    uint16_t persisted_screen_root = vk_usb_json_root(document);
    static const char *const screen_keys[] = {"configured_mode", "image", "layout", "pet", "previous_revision", "revision", "schema"};
    if (!exact(document, persisted_screen_root, screen_keys, 7U) ||
        !uint32_value(document, find(document, persisted_screen_root, "revision"), UINT32_MAX, true, &manifest_revision) || manifest_revision != revision ||
        !uint32_value(document, find(document, persisted_screen_root, "previous_revision"), UINT32_MAX, false, &manifest_previous) || manifest_previous != previous_revision ||
        !uint32_value(document, find(document, persisted_screen_root, "schema"), 1U, true, &schema) || schema != 1U) return ESP_ERR_INVALID_RESPONSE;
    static const char *const field_names[] = {"configured_mode", "image", "layout", "pet"};
    if (used == envelope_capacity) return ESP_ERR_INVALID_SIZE;
    envelope[used++] = '{';
    for (size_t field = 0U; field < 4U; ++field) {
        uint16_t value = find(document, persisted_screen_root, field_names[field]); size_t start = 0U, end = 0U;
        int key_count = snprintf((char *)envelope + used, envelope_capacity - used, "%s\"%s\":", field ? "," : "", field_names[field]);
        if (key_count <= 0 || (size_t)key_count >= envelope_capacity - used || !vk_usb_json_node_range(document, value, &start, &end) || end - start >= envelope_capacity - used - (size_t)key_count) return ESP_ERR_INVALID_SIZE;
        used += (size_t)key_count; memcpy(envelope + used, screen_manifest + start, end - start); used += end - start;
    }
    if (envelope_capacity - used < 3U) return ESP_ERR_INVALID_SIZE;
    envelope[used++] = '}'; envelope[used++] = '}';
    if (used > VK_USB_JSON_MAX_BYTES || vk_usb_json_parse(document, envelope, used) != VK_USB_JSON_OK) return ESP_ERR_INVALID_RESPONSE;
    uint16_t root = vk_usb_json_root(document);
    uint16_t assets_node = find(document, root, "assets"), screen_node = find(document, root, "screen");
    char mode[16];
    if (!string_value(document, find(document, screen_node, "configured_mode"), mode, sizeof(mode))) return ESP_ERR_INVALID_RESPONSE;
    vk_usb_screen_configured_mode_t configured_mode;
    if (strcmp(mode, "image") == 0) configured_mode = VK_USB_SCREEN_IMAGE;
    else if (strcmp(mode, "pet") == 0) configured_mode = VK_USB_SCREEN_PET;
    else if (strcmp(mode, "dashboard") == 0) configured_mode = VK_USB_SCREEN_DASHBOARD;
    else if (strcmp(mode, "custom") == 0) configured_mode = VK_USB_SCREEN_CUSTOM;
    else return ESP_ERR_INVALID_RESPONSE;
    vk_usb_screen_command_t command = {
        .kind = VK_USB_SCREEN_COMMIT,
        .expected_epoch = 1U,
        .snapshot_generation = 1U,
        .expected_revision = previous_revision,
        .revision = revision,
        .configured_mode = configured_mode,
        .document = document,
        .assets_node = assets_node,
        .screen_node = screen_node,
    };
    vk_usb_screen_event_t event;
    /* The previous durable revision is the restore base even though no root has been built yet. */
    screen->current.revision = previous_revision;
    vk_screen_durable_publish_t publisher = screen->config.durable_publish;
    void *publisher_context = screen->config.durable_context;
    screen->config.durable_publish = NULL;
    screen->config.durable_context = NULL;
    esp_err_t result = vk_screen_commit(screen, &command, &event);
    screen->config.durable_publish = publisher;
    screen->config.durable_context = publisher_context;
    if (result == ESP_OK) {
        uint8_t digest[32]; static const char hex[] = "0123456789abcdef";
        vk_asset_sha256(assets_manifest, assets_bytes, digest);
        for (size_t index = 0U; index < 32U; ++index) { screen->current.assets_manifest_sha256[index * 2U] = hex[digest[index] >> 4U]; screen->current.assets_manifest_sha256[index * 2U + 1U] = hex[digest[index] & 15U]; }
        screen->current.assets_manifest_sha256[64] = 0;
        vk_asset_sha256(screen_manifest, screen_bytes, digest);
        for (size_t index = 0U; index < 32U; ++index) { screen->current.screen_manifest_sha256[index * 2U] = hex[digest[index] >> 4U]; screen->current.screen_manifest_sha256[index * 2U + 1U] = hex[digest[index] & 15U]; }
        screen->current.screen_manifest_sha256[64] = 0;
        screen->bound_epoch = 0U;
        screen->bound_snapshot_generation = 0U;
    } else {
        screen->current = (vk_screen_model_t){0};
    }
    return result;
}

esp_err_t vk_screen_query(vk_screen_t*screen,uint32_t epoch,uint32_t generation,vk_usb_screen_event_t*event){if(!screen||!event||!epoch||!generation)return ESP_ERR_INVALID_ARG;if(!lock_screen(screen))return ESP_ERR_INVALID_STATE;memset(event,0,sizeof(*event));event->kind=VK_USB_SCREEN_EVENT_STATE;event->configured=screen->current.configured;event->configured_mode=screen->current.configured_mode;event->revision=screen->current.revision;if(screen->current.configured){snprintf(event->assets_manifest_sha256,sizeof(event->assets_manifest_sha256),"%s",screen->current.assets_manifest_sha256);snprintf(event->screen_manifest_sha256,sizeof(event->screen_manifest_sha256),"%s",screen->current.screen_manifest_sha256);}screen->bound_epoch=epoch;screen->bound_snapshot_generation=generation;unlock_screen(screen);return ESP_OK;}

esp_err_t vk_screen_format_milli(int64_t value,uint8_t decimals,char*out,size_t capacity){if(!out||capacity==0||decimals>3)return ESP_ERR_INVALID_ARG;int64_t unit=1;for(uint8_t i=decimals;i<3;i++)unit*=10;bool negative=value<0;uint64_t magnitude=negative?(uint64_t)(-(value+1))+1U:(uint64_t)value;uint64_t rounded=(magnitude+((uint64_t)unit/2U))/(uint64_t)unit;uint64_t scale=1;for(uint8_t i=0;i<decimals;i++)scale*=10U;uint64_t whole=rounded/scale,fraction=rounded%scale;if(whole==0&&fraction==0)negative=false;int n=decimals?snprintf(out,capacity,"%s%"PRIu64".%0*"PRIu64,negative?"-":"",whole,(int)decimals,fraction):snprintf(out,capacity,"%s%"PRIu64,negative?"-":"",whole);return n>=0&&(size_t)n<capacity?ESP_OK:ESP_ERR_INVALID_SIZE;}

esp_err_t vk_screen_widget_update(vk_screen_t*screen,const vk_screen_widget_update_t*update,char rendered[VK_SCREEN_MAX_TEXT_BYTES+1U]){if(!screen||!update||!rendered||!update->revision||!update->sequence||!valid_identifier(update->widget_id))return ESP_ERR_INVALID_ARG;if(!lock_screen(screen))return ESP_ERR_INVALID_STATE;if(!screen->current.configured||screen->current.revision!=update->revision){unlock_screen(screen);return ESP_ERR_INVALID_STATE;}vk_screen_widget_t*widget=NULL;for(uint16_t i=0;i<screen->current.widget_count;i++)if(strcmp(screen->current.widgets[i].id,update->widget_id)==0){widget=&screen->current.widgets[i];break;}if(!widget|| (widget->has_sequence&&!sequence_is_newer(update->sequence,widget->sequence))){unlock_screen(screen);return ESP_ERR_NOT_FOUND;}if(update->state==VK_SCREEN_WIDGET_FRESH){if(widget->type==VK_SCREEN_WIDGET_TEXT){if(!update->has_text||update->has_number||strlen(update->text)>screen->config.capability.max_widget_value_bytes||!text_supported(update->text)){unlock_screen(screen);return ESP_ERR_INVALID_ARG;}snprintf(rendered,VK_SCREEN_MAX_TEXT_BYTES+1U,"%s",update->text);}else{if(!update->has_number||update->has_text||(widget->type!=VK_SCREEN_WIDGET_INTEGER&&(update->number_milli<widget->minimum_milli||update->number_milli>widget->maximum_milli))){unlock_screen(screen);return ESP_ERR_INVALID_ARG;}uint8_t decimals=widget->type==VK_SCREEN_WIDGET_INTEGER?0U:widget->decimals;if(vk_screen_format_milli(update->number_milli,decimals,rendered,VK_SCREEN_MAX_TEXT_BYTES+1U)!=ESP_OK){unlock_screen(screen);return ESP_ERR_INVALID_SIZE;}}}else{if(widget->type==VK_SCREEN_WIDGET_TEXT)snprintf(rendered,VK_SCREEN_MAX_TEXT_BYTES+1U,"%s",widget->fallback_text);else{uint8_t decimals=widget->type==VK_SCREEN_WIDGET_INTEGER?0U:widget->decimals;if(vk_screen_format_milli(widget->fallback_milli,decimals,rendered,VK_SCREEN_MAX_TEXT_BYTES+1U)!=ESP_OK){unlock_screen(screen);return ESP_ERR_INVALID_SIZE;}}}
    esp_err_t result=screen->config.renderer.lock(screen->config.renderer.context);if(result==ESP_OK){result=screen->config.renderer.apply_widget?screen->config.renderer.apply_widget(screen->config.renderer.context,screen->current_root,widget->target,rendered,update->sequence,update->state):ESP_ERR_NOT_SUPPORTED;screen->config.renderer.unlock(screen->config.renderer.context);}if(result==ESP_OK){widget->sequence=update->sequence;widget->has_sequence=true;}unlock_screen(screen);return result;}

esp_err_t vk_screen_push_overlay(vk_screen_t*screen,vk_screen_overlay_t overlay){if(!screen||overlay>VK_SCREEN_OVERLAY_ERROR)return ESP_ERR_INVALID_ARG;if(!lock_screen(screen))return ESP_ERR_INVALID_STATE;if(screen->overlay_count>=VK_SCREEN_MAX_OVERLAYS){unlock_screen(screen);return ESP_ERR_NO_MEM;}esp_err_t result=screen->config.renderer.lock(screen->config.renderer.context);if(result==ESP_OK){result=screen->config.renderer.show_overlay?screen->config.renderer.show_overlay(screen->config.renderer.context,overlay):ESP_ERR_NOT_SUPPORTED;screen->config.renderer.unlock(screen->config.renderer.context);}if(result==ESP_OK)screen->overlays[screen->overlay_count++]=overlay;unlock_screen(screen);return result;}
esp_err_t vk_screen_pop_overlay(vk_screen_t*screen,vk_screen_overlay_t overlay){if(!screen||overlay>VK_SCREEN_OVERLAY_ERROR)return ESP_ERR_INVALID_ARG;if(!lock_screen(screen))return ESP_ERR_INVALID_STATE;if(screen->overlay_count==0U||screen->overlays[screen->overlay_count-1U]!=overlay){unlock_screen(screen);return ESP_ERR_INVALID_STATE;}esp_err_t result=screen->config.renderer.lock(screen->config.renderer.context);if(result==ESP_OK){result=screen->config.renderer.clear_overlay?screen->config.renderer.clear_overlay(screen->config.renderer.context,overlay):ESP_ERR_NOT_SUPPORTED;screen->config.renderer.unlock(screen->config.renderer.context);}if(result==ESP_OK)screen->overlay_count--;unlock_screen(screen);return result;}

esp_err_t vk_screen_pet_tick(vk_screen_t*screen,uint64_t now,uint16_t count,const uint16_t*durations,uint16_t*out){if(!screen||!durations||!out||count==0U)return ESP_ERR_INVALID_ARG;if(!lock_screen(screen))return ESP_ERR_INVALID_STATE;if(screen->pet_frame>=count)screen->pet_frame=0U;if(screen->pet_deadline_ms==0U)screen->pet_deadline_ms=now+durations[screen->pet_frame];while(now>=screen->pet_deadline_ms){screen->pet_frame=(uint16_t)((screen->pet_frame+1U)%count);uint16_t duration=durations[screen->pet_frame];if(duration==0U){unlock_screen(screen);return ESP_ERR_INVALID_ARG;}if(screen->pet_deadline_ms>UINT64_MAX-duration){screen->pet_deadline_ms=UINT64_MAX;break;}screen->pet_deadline_ms+=duration;}*out=screen->pet_frame;unlock_screen(screen);return ESP_OK;}

void vk_screen_close_epoch(vk_screen_t*screen,uint32_t epoch){if(!screen||!epoch||!lock_screen(screen))return;if(screen->bound_epoch==epoch){screen->bound_epoch=0U;screen->bound_snapshot_generation=0U;for(uint16_t i=0;i<screen->current.widget_count;i++)screen->current.widgets[i].has_sequence=false;}unlock_screen(screen);}

esp_err_t vk_screen_layout_place(bool row,int16_t ox,int16_t oy,uint16_t width,uint16_t height,uint16_t gap,uint8_t main,uint8_t cross,vk_screen_rect_t*children,size_t count){if(!children||count==0U||main>2U||cross>2U)return ESP_ERR_INVALID_ARG;uint32_t used=(uint32_t)gap*(uint32_t)(count-1U);for(size_t i=0;i<count;i++){uint32_t size=row?children[i].width:children[i].height;if(!checked_add_u32(used,size,&used))return ESP_ERR_INVALID_SIZE;}uint32_t available=row?width:height;if(used>available)return ESP_ERR_INVALID_SIZE;uint32_t remain=available-used,offset=main==0U?0U:main==2U?remain:remain/2U;for(size_t i=0;i<count;i++){uint32_t cross_size=row?children[i].height:children[i].width,cross_available=row?height:width;if(cross_size>cross_available)return ESP_ERR_INVALID_SIZE;uint32_t cross_remain=cross_available-cross_size,cross_offset=cross==0U?0U:cross==2U?cross_remain:cross_remain/2U;int32_t x=row?(int32_t)ox+offset:(int32_t)ox+cross_offset,y=row?(int32_t)oy+cross_offset:(int32_t)oy+offset;if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX)return ESP_ERR_INVALID_SIZE;children[i].x=(int16_t)x;children[i].y=(int16_t)y;offset+=(row?children[i].width:children[i].height)+gap;}return ESP_OK;}
