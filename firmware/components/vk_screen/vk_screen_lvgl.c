#include "vk_screen.h"

#ifdef ESP_PLATFORM

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

typedef struct {
    uint8_t *container;
    size_t container_bytes;
    uint16_t *pixels;
    lv_image_dsc_t descriptor;
    uint16_t frame_count;
    uint16_t frame_index;
    uint16_t durations_ms[64];
    uint64_t deadline_ms;
    lv_obj_t *object;
} vk_lvgl_image_owner_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *objects[VK_SCREEN_MAX_OBJECTS];
    char object_ids[VK_SCREEN_MAX_OBJECTS][VK_SCREEN_MAX_ID_BYTES + 1U];
    vk_lvgl_image_owner_t images[VK_SCREEN_MAX_OBJECTS];
    lv_obj_t *overlay;
    uint16_t count;
} vk_lvgl_root_t;
typedef struct { vk_lvgl_root_t *active; vk_screen_asset_resolver_t assets; lv_font_t *font; } vk_lvgl_context_t;
static vk_lvgl_context_t s_context;
static void adapter_destroy(void *context, void *root);

static const char *initial_label_text(
    const vk_screen_model_t *model,
    const vk_screen_object_t *object,
    char formatted[VK_SCREEN_MAX_TEXT_BYTES + 1U]
)
{
    if (object->type == VK_SCREEN_OBJECT_STATIC_LABEL) return object->text;
    for (uint16_t index = 0U; index < model->widget_count; ++index) {
        const vk_screen_widget_t *widget = &model->widgets[index];
        if (strcmp(widget->target, object->id) != 0) continue;
        if (widget->type == VK_SCREEN_WIDGET_TEXT) return widget->fallback_text;
        uint8_t decimals = widget->type == VK_SCREEN_WIDGET_INTEGER ? 0U : widget->decimals;
        if (vk_screen_format_milli(
                widget->fallback_milli, decimals, formatted,
                VK_SCREEN_MAX_TEXT_BYTES + 1U) == ESP_OK) {
            return formatted;
        }
        break;
    }
    return "";
}

static esp_err_t adapter_lock(void *context)
{
    (void)context;
    return lvgl_port_lock(0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void adapter_unlock(void *context)
{
    (void)context;
    lvgl_port_unlock();
}

static lv_obj_t *create_object(lv_obj_t *parent, const vk_screen_object_t *object)
{
    if (object->type == VK_SCREEN_OBJECT_STATIC_LABEL || object->type == VK_SCREEN_OBJECT_GLYPH_LABEL ||
        object->type == VK_SCREEN_OBJECT_DYNAMIC_LABEL || object->type == VK_SCREEN_OBJECT_ICON_TEXT) return lv_label_create(parent);
    if (object->type == VK_SCREEN_OBJECT_PROGRESS) return lv_bar_create(parent);
    if (object->type == VK_SCREEN_OBJECT_IMAGE || object->type == VK_SCREEN_OBJECT_PET) return lv_image_create(parent);
    return lv_obj_create(parent);
}

static esp_err_t adapter_create(void *context, const vk_screen_model_t *model, void **root)
{
    (void)context;
    if (model == NULL || root == NULL) return ESP_ERR_INVALID_ARG;
    vk_lvgl_root_t *candidate = heap_caps_calloc(
        1U, sizeof(*candidate), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (candidate == NULL) return ESP_ERR_NO_MEM;
    candidate->screen = lv_obj_create(NULL);
    if (candidate->screen == NULL) { free(candidate); return ESP_ERR_NO_MEM; }
    lv_obj_remove_style_all(candidate->screen);
    lv_obj_set_size(candidate->screen, VK_SCREEN_WIDTH, VK_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(candidate->screen, lv_color_hex(model->background_rgb888), 0);
    lv_obj_set_style_bg_opa(candidate->screen, LV_OPA_COVER, 0);

    /* Sort objects by z then source_order for stable LVGL stacking.
     * A trivial insertion sort suffices for VK_SCREEN_MAX_OBJECTS = 64. */
    uint16_t order[VK_SCREEN_MAX_OBJECTS];
    for (uint16_t i = 0U; i < model->object_count; ++i) order[i] = i;
    for (uint16_t i = 1U; i < model->object_count; ++i) {
        uint16_t key = order[i]; int32_t j = (int32_t)i - 1;
        const vk_screen_object_t *key_obj = &model->objects[key];
        while (j >= 0) {
            const vk_screen_object_t *cmp = &model->objects[order[j]];
            if (cmp->z < key_obj->z || (cmp->z == key_obj->z && cmp->source_order <= key_obj->source_order)) break;
            order[j + 1] = (uint16_t)order[j]; --j;
        }
        order[j + 1] = key;
    }

    bool has_font = s_context.font != NULL;
    for (uint16_t slot = 0U; slot < model->object_count; ++slot) {
        uint16_t index = order[slot];
        const vk_screen_object_t *object = &model->objects[index];
        lv_obj_t *parent = candidate->screen;
        if (object->parent != UINT16_MAX && object->parent < model->object_count && candidate->objects[object->parent] != NULL)
            parent = candidate->objects[object->parent];
        lv_obj_t *item = create_object(parent, object);
        if (item == NULL) { adapter_destroy(context, candidate); return ESP_ERR_NO_MEM; }
        candidate->objects[index] = item;
        snprintf(candidate->object_ids[index], sizeof(candidate->object_ids[index]), "%s", object->id);
        candidate->count = model->object_count;
        int32_t x = object->rect.x, y = object->rect.y;
        if (object->parent != UINT16_MAX && object->parent < model->object_count) {
            x -= model->objects[object->parent].rect.x;
            y -= model->objects[object->parent].rect.y;
        }
        lv_obj_set_pos(item, x, y);
        lv_obj_set_size(item, object->rect.width, object->rect.height);
        if (!object->visible) lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
        /* LV_OBJ_FLAG_CLIP_CHILDREN not available in current LVGL port.
         * Nested overflow is clamped by lv_obj_set_size already set above. */
        lv_obj_set_style_bg_color(item, lv_color_hex(object->background_rgb888), 0);
        if (object->type == VK_SCREEN_OBJECT_STATIC_LABEL || object->type == VK_SCREEN_OBJECT_DYNAMIC_LABEL || object->type == VK_SCREEN_OBJECT_ICON_TEXT) {
            char formatted[VK_SCREEN_MAX_TEXT_BYTES + 1U];
            lv_label_set_text(item, initial_label_text(model, object, formatted));
            lv_obj_set_style_text_color(item, lv_color_hex(object->color_rgb888), 0);
            lv_obj_set_style_text_align(item, object->align == 1U ? LV_TEXT_ALIGN_CENTER : object->align == 2U ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_long_mode(item, LV_LABEL_LONG_CLIP);
            if (has_font) lv_obj_set_style_text_font(item, s_context.font, 0);
        } else if (object->type == VK_SCREEN_OBJECT_PROGRESS) {
            lv_bar_set_range(item, 0, 1000);
            lv_bar_set_value(item, 0, LV_ANIM_OFF);
        }
        if (object->type == VK_SCREEN_OBJECT_IMAGE || object->type == VK_SCREEN_OBJECT_PET) {
            vk_lvgl_context_t *adapter = context; vk_screen_asset_info_t info = {0};
            vk_lvgl_image_owner_t *owner = &candidate->images[index];
            if (object->sha256[0] == 0 || adapter->assets.resolve == NULL ||
                adapter->assets.resolve(adapter->assets.context, object->sha256, &info) != ESP_OK ||
                info.container == NULL || info.frame_count > 64U) {
                if (info.container != NULL && adapter->assets.release != NULL) adapter->assets.release(adapter->assets.context, &info);
                adapter_destroy(context, candidate); return ESP_ERR_INVALID_RESPONSE;
            }
            owner->container = (uint8_t *)info.container; owner->container_bytes = info.container_bytes;
            owner->frame_count = info.frame_count; owner->object = item;
            owner->pixels = malloc(info.decoded_bytes_per_frame);
            if (owner->pixels == NULL || vk_vka1_decode_frame(owner->container, owner->container_bytes, 0U,
                owner->pixels, (size_t)info.width * info.height) != VK_VKA1_OK) {
                free(owner->pixels); owner->pixels = NULL; if (adapter->assets.release != NULL) adapter->assets.release(adapter->assets.context, &info);
                owner->container = NULL; adapter_destroy(context, candidate); return ESP_ERR_NO_MEM;
            }
            for (uint16_t frame = 0U; frame < info.frame_count; ++frame) {
                vk_vka1_frame_info_t frame_info;
                if (vk_vka1_frame_info(owner->container, owner->container_bytes, frame, &frame_info) != VK_VKA1_OK) {
                    free(owner->pixels); owner->pixels = NULL; if (adapter->assets.release != NULL) adapter->assets.release(adapter->assets.context, &info);
                    owner->container = NULL; adapter_destroy(context, candidate); return ESP_ERR_INVALID_RESPONSE;
                }
                owner->durations_ms[frame] = frame_info.duration_ms;
            }
            owner->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
            owner->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
            owner->descriptor.header.w = info.width; owner->descriptor.header.h = info.height;
            owner->descriptor.header.stride = info.width * sizeof(uint16_t);
            owner->descriptor.data_size = info.decoded_bytes_per_frame;
            owner->descriptor.data = (const uint8_t *)owner->pixels;
            lv_image_set_src(item, &owner->descriptor);
            /* Apply image fit: 0=contain 1=cover 2=stretch 3=center.
             * Zoom uses fixed-point 256 == 100 %.  Scale factors are clipped
             * to the object rect to keep the image within bounds on overflow. */
            if (object->fit == 3U) {
                lv_obj_set_size(item, info.width, info.height);
                lv_obj_align_to(item, parent, LV_ALIGN_CENTER,
                    x + (int32_t)object->rect.width / 2 - (int32_t)info.width / 2,
                    y + (int32_t)object->rect.height / 2 - (int32_t)info.height / 2);
            } else if (object->fit == 0U || object->fit == 1U) {
                if (info.width > 0U && info.height > 0U) {
                    uint16_t sx = (uint16_t)((uint32_t)object->rect.width * 256U / info.width);
                    uint16_t sy = (uint16_t)((uint32_t)object->rect.height * 256U / info.height);
                    uint16_t zoom = (object->fit == 0U) ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
                    lv_image_set_scale_x(item, zoom);
                    lv_image_set_scale_y(item, zoom);
                }
                lv_obj_center(item);
            }
            /* stretch (fit==2) is the default — bounds already applied above. */
        }
    }
    *root = candidate; return ESP_OK;
}

static esp_err_t adapter_widget(void *context, void *root, const char *object_id,
                                const char *value, uint32_t sequence,
                                vk_screen_widget_state_t state)
{
    (void)context;(void)sequence;(void)state;
    vk_lvgl_root_t *candidate = root; if (candidate == NULL || object_id == NULL || value == NULL) return ESP_ERR_INVALID_ARG;
    for (uint16_t index = 0U; index < candidate->count; ++index) {
        lv_obj_t *item = candidate->objects[index]; if (item == NULL) continue;
        if (strcmp(candidate->object_ids[index], object_id) == 0) {
            if (lv_obj_check_type(item, &lv_bar_class)) { char *end = NULL; long number = strtol(value, &end, 10); if (end == value || *end != 0) return ESP_ERR_INVALID_ARG; lv_bar_set_value(item, (int32_t)number, LV_ANIM_OFF); }
            else lv_label_set_text(item, value);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t adapter_overlay(void *context, vk_screen_overlay_t overlay)
{
    static const char *const labels[] = {"Recording", "Uploading", "Updating", "Error"};
    vk_lvgl_context_t *adapter = context; vk_lvgl_root_t *active = adapter == NULL ? NULL : adapter->active;
    if (active == NULL || active->screen == NULL || overlay > VK_SCREEN_OVERLAY_ERROR) return ESP_ERR_INVALID_STATE;
    if (active->overlay == NULL) {
        active->overlay = lv_label_create(active->screen); if (active->overlay == NULL) return ESP_ERR_NO_MEM;
        lv_obj_set_size(active->overlay, VK_SCREEN_WIDTH, VK_SCREEN_HEIGHT);
        lv_obj_set_style_bg_opa(active->overlay, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(active->overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(active->overlay, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_align(active->overlay, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(active->overlay, labels[overlay]);
    lv_obj_move_foreground(active->overlay);
    return ESP_OK;
}

static esp_err_t adapter_clear_overlay(void *context, vk_screen_overlay_t overlay)
{
    vk_lvgl_context_t *adapter = context; (void)overlay;
    if (adapter == NULL || adapter->active == NULL || adapter->active->overlay == NULL) return ESP_ERR_INVALID_STATE;
    lv_obj_delete(adapter->active->overlay); adapter->active->overlay = NULL; return ESP_OK;
}

static esp_err_t adapter_swap(void *context, void *candidate, void **old_root)
{
    vk_lvgl_context_t *adapter = context;
    if (adapter == NULL || candidate == NULL || old_root == NULL) return ESP_ERR_INVALID_ARG;
    vk_lvgl_root_t *next = candidate; *old_root = adapter->active; lv_screen_load(next->screen); adapter->active = next; return ESP_OK;
}

static void adapter_destroy(void *context, void *root)
{
    (void)context;
    if (root != NULL) {
        vk_lvgl_root_t *candidate = root;
        if (candidate->screen != NULL) lv_obj_delete(candidate->screen);
        for (uint16_t index = 0U; index < VK_SCREEN_MAX_OBJECTS; ++index) {
            free(candidate->images[index].pixels);
            free(candidate->images[index].container);
        }
        free(candidate);
    }
}

esp_err_t vk_screen_lvgl_tick(uint64_t now_ms)
{
    vk_lvgl_root_t *root = s_context.active;
    if (root == NULL) return ESP_ERR_INVALID_STATE;
    for (uint16_t index = 0U; index < VK_SCREEN_MAX_OBJECTS; ++index) {
        vk_lvgl_image_owner_t *owner = &root->images[index];
        if (owner->frame_count <= 1U || owner->object == NULL) continue;
        if (owner->deadline_ms == 0U) owner->deadline_ms = now_ms + owner->durations_ms[0];
        while (now_ms >= owner->deadline_ms) {
            owner->frame_index = (uint16_t)((owner->frame_index + 1U) % owner->frame_count);
            owner->deadline_ms += owner->durations_ms[owner->frame_index];
        }
        if (vk_vka1_decode_frame(owner->container, owner->container_bytes, owner->frame_index,
                                 owner->pixels, owner->descriptor.header.w * owner->descriptor.header.h) != VK_VKA1_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        lv_image_set_src(owner->object, &owner->descriptor);
        lv_obj_invalidate(owner->object);
    }
    return ESP_OK;
}

esp_err_t vk_screen_lvgl_make_renderer(vk_screen_renderer_ops_t *ops,
                                       const vk_screen_asset_resolver_t *assets,
                                       void *font)
{
    if (ops == NULL || assets == NULL || assets->resolve == NULL || assets->release == NULL) return ESP_ERR_INVALID_ARG;
    s_context.assets = *assets;
    s_context.font = (lv_font_t *)font;
    *ops = (vk_screen_renderer_ops_t){
        .lock = adapter_lock,
        .unlock = adapter_unlock,
        .create_candidate = adapter_create,
        .apply_widget = adapter_widget,
        .show_overlay = adapter_overlay,
        .clear_overlay = adapter_clear_overlay,
        .swap_root = adapter_swap,
        .destroy_root = adapter_destroy,
        .context = &s_context,
    };
    return ESP_OK;
}

#endif
