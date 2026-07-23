#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_usb.h"
#include "vk_vka1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_SCREEN_WIDTH 428U
#define VK_SCREEN_HEIGHT 142U
#define VK_SCREEN_MAX_OBJECTS 64U
#define VK_SCREEN_MAX_WIDGETS 32U
#define VK_SCREEN_MAX_OVERLAYS 4U
#define VK_SCREEN_MAX_PET_STATES 6U
#define VK_SCREEN_MAX_ID_BYTES 32U
#define VK_SCREEN_MAX_TEXT_BYTES 512U
#define VK_SCREEN_FONT_METRICS_SHA256 "b6567a24b312e6e80c2f5ea200e4377d42926e11bd55544752b2533c2235b22b"

typedef enum {
    VK_SCREEN_OBJECT_IMAGE = 0,
    VK_SCREEN_OBJECT_PET,
    VK_SCREEN_OBJECT_STATIC_LABEL,
    VK_SCREEN_OBJECT_GLYPH_LABEL,
    VK_SCREEN_OBJECT_DYNAMIC_LABEL,
    VK_SCREEN_OBJECT_PROGRESS,
    VK_SCREEN_OBJECT_ICON_TEXT,
    VK_SCREEN_OBJECT_ROW,
    VK_SCREEN_OBJECT_COLUMN,
} vk_screen_object_type_t;

typedef enum {
    VK_SCREEN_OVERLAY_RECORDING = 0,
    VK_SCREEN_OVERLAY_UPLOAD,
    VK_SCREEN_OVERLAY_FIRMWARE_UPDATE,
    VK_SCREEN_OVERLAY_ERROR,
} vk_screen_overlay_t;

typedef enum {
    VK_SCREEN_WIDGET_TEXT = 0,
    VK_SCREEN_WIDGET_INTEGER,
    VK_SCREEN_WIDGET_NUMBER,
    VK_SCREEN_WIDGET_PROGRESS,
} vk_screen_widget_type_t;

typedef enum {
    VK_SCREEN_WIDGET_FRESH = 0,
    VK_SCREEN_WIDGET_STALE,
    VK_SCREEN_WIDGET_ERROR,
} vk_screen_widget_state_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
} vk_screen_rect_t;

typedef struct {
    char id[VK_SCREEN_MAX_ID_BYTES + 1U];
    vk_screen_object_type_t type;
    vk_screen_rect_t rect;
    int16_t z;
    uint16_t source_order;
    uint16_t parent;
    bool clip;
    bool visible;
    char widget_id[VK_SCREEN_MAX_ID_BYTES + 1U];
    char sha256[65];
    char text[VK_SCREEN_MAX_TEXT_BYTES + 1U];
    uint32_t background_rgb888;
    uint32_t color_rgb888;
    uint8_t align;
    uint8_t fit;
    bool overflow_clip;
} vk_screen_object_t;

typedef struct {
    char id[VK_SCREEN_MAX_ID_BYTES + 1U];
    char target[VK_SCREEN_MAX_ID_BYTES + 1U];
    vk_screen_widget_type_t type;
    int64_t minimum_milli;
    int64_t maximum_milli;
    int64_t fallback_milli;
    uint8_t decimals;
    uint32_t sequence;
    bool has_sequence;
    char fallback_text[VK_SCREEN_MAX_TEXT_BYTES + 1U];
} vk_screen_widget_t;

typedef struct {
    bool configured;
    vk_usb_screen_configured_mode_t configured_mode;
    uint32_t revision;
    uint32_t previous_revision;
    uint32_t background_rgb888;
    uint16_t object_count;
    uint16_t widget_count;
    vk_screen_object_t objects[VK_SCREEN_MAX_OBJECTS];
    vk_screen_widget_t widgets[VK_SCREEN_MAX_WIDGETS];
    uint32_t decoded_charge_bytes;
    char assets_manifest_sha256[65];
    char screen_manifest_sha256[65];
} vk_screen_model_t;

typedef struct {
    vk_vka1_kind_t kind;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint32_t decoded_bytes_per_frame;
    const uint8_t *container;
    size_t container_bytes;
} vk_screen_asset_info_t;

typedef struct {
    esp_err_t (*resolve)(void *context, const char *sha256, vk_screen_asset_info_t *info);
    void (*release)(void *context, vk_screen_asset_info_t *info);
    void *context;
} vk_screen_asset_resolver_t;

typedef struct {
    esp_err_t (*lock)(void *context);
    void (*unlock)(void *context);
    esp_err_t (*create_candidate)(void *context, const vk_screen_model_t *model, void **root);
    esp_err_t (*apply_widget)(void *context, void *root, const char *object_id,
                              const char *rendered_value, uint32_t sequence,
                              vk_screen_widget_state_t state);
    esp_err_t (*show_overlay)(void *context, vk_screen_overlay_t overlay);
    esp_err_t (*clear_overlay)(void *context, vk_screen_overlay_t overlay);
    esp_err_t (*swap_root)(void *context, void *candidate, void **old_root);
    void (*destroy_root)(void *context, void *root);
    void *context;
} vk_screen_renderer_ops_t;

typedef esp_err_t (*vk_screen_durable_publish_t)(void *context,
                                                 const vk_usb_screen_command_t *command,
                                                 vk_screen_model_t *candidate);

typedef struct {
    vk_usb_screen_capability_t capability;
    vk_screen_asset_resolver_t assets;
    vk_screen_renderer_ops_t renderer;
    uint32_t root_budget_bytes;
    uint32_t decoder_scratch_bytes;
    vk_screen_durable_publish_t durable_publish;
    void *durable_context;
    bool production_profile_admitted;
} vk_screen_config_t;

typedef struct {
    uint32_t revision;
    char widget_id[VK_SCREEN_MAX_ID_BYTES + 1U];
    uint32_t sequence;
    vk_screen_widget_state_t state;
    bool has_text;
    char text[VK_SCREEN_MAX_TEXT_BYTES + 1U];
    bool has_number;
    int64_t number_milli;
} vk_screen_widget_update_t;

typedef struct {
    vk_screen_config_t config;
    atomic_flag owner;
    atomic_bool stopping;
    vk_screen_model_t current;
    void *current_root;
    vk_screen_overlay_t overlays[VK_SCREEN_MAX_OVERLAYS];
    uint8_t overlay_count;
    uint32_t bound_epoch;
    uint32_t bound_snapshot_generation;
    uint64_t pet_deadline_ms;
    uint16_t pet_frame;
    uint8_t selected_assets_manifest[VK_USB_MAX_JSON_BYTES];
    size_t selected_assets_manifest_bytes;
    uint8_t selected_screen_manifest[VK_USB_MAX_JSON_BYTES];
    size_t selected_screen_manifest_bytes;
    char last_error_stage[24];
} vk_screen_t;

esp_err_t vk_screen_init(vk_screen_t *screen, const vk_screen_config_t *config);
esp_err_t vk_screen_stop(vk_screen_t *screen);
esp_err_t vk_screen_commit(vk_screen_t *screen, const vk_usb_screen_command_t *command,
                           vk_usb_screen_event_t *event);
const char *vk_screen_last_error_stage(const vk_screen_t *screen);
/* Rebuilds a render root from an already validated immutable store revision.
 * The caller owns the bounded JSON workspace and envelope buffer. No durable write occurs. */
esp_err_t vk_screen_restore(vk_screen_t *screen, uint32_t revision, uint32_t previous_revision,
                            const uint8_t *assets_manifest, size_t assets_bytes,
                            const uint8_t *screen_manifest, size_t screen_bytes,
                            vk_usb_json_document_t *document,
                            uint8_t *envelope, size_t envelope_capacity);
esp_err_t vk_screen_query(vk_screen_t *screen, uint32_t expected_epoch,
                          uint32_t snapshot_generation, vk_usb_screen_event_t *event);
esp_err_t vk_screen_widget_update(vk_screen_t *screen,
                                  const vk_screen_widget_update_t *update,
                                  char rendered_value[VK_SCREEN_MAX_TEXT_BYTES + 1U]);
esp_err_t vk_screen_push_overlay(vk_screen_t *screen, vk_screen_overlay_t overlay);
esp_err_t vk_screen_pop_overlay(vk_screen_t *screen, vk_screen_overlay_t overlay);
esp_err_t vk_screen_pet_tick(vk_screen_t *screen, uint64_t now_ms, uint16_t frame_count,
                             const uint16_t *durations_ms, uint16_t *frame_index);
void vk_screen_close_epoch(vk_screen_t *screen, uint32_t epoch);
bool vk_screen_revision_is_newer(uint32_t candidate, uint32_t current);
esp_err_t vk_screen_layout_place(bool row, int16_t origin_x, int16_t origin_y,
                                 uint16_t width, uint16_t height, uint16_t gap,
                                 uint8_t main_align, uint8_t cross_align,
                                 vk_screen_rect_t *children, size_t count);
esp_err_t vk_screen_format_milli(int64_t value_milli, uint8_t decimals,
                                 char *output, size_t capacity);
esp_err_t vk_screen_font_glyph_origin(const char *font_id, uint16_t version,
                                      const char *metrics_sha256, uint32_t scalar,
                                      int16_t pen_x, int16_t *glyph_x, uint16_t *advance);

#ifdef ESP_PLATFORM
/* LVGL adapter creation is side-effect free. Objects are created only by vk_screen_commit().
 * Pass NULL for |font| when the compiled-font gate is unadmitted; the default LVGL
 * font will be used in that case. |font| is typed as void* to avoid pulling lvgl.h
 * into every translation unit that includes this header. */
esp_err_t vk_screen_lvgl_make_renderer(vk_screen_renderer_ops_t *ops,
                                       const vk_screen_asset_resolver_t *assets,
                                       void *font);
/* Advances every active VKA1 animation/pet descriptor using one monotonic clock.
 * The caller must hold the LVGL port lock. */
esp_err_t vk_screen_lvgl_tick(uint64_t now_ms);
#endif

#ifdef __cplusplus
}
#endif
