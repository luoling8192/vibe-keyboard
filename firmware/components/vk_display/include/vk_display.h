#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_DISPLAY_WIDTH 428U
#define VK_DISPLAY_HEIGHT 142U
#define VK_DISPLAY_BYTES_PER_PIXEL 2U
#define VK_DISPLAY_BUFFER_LINES 10U
#define VK_DISPLAY_BUFFER_PIXELS (VK_DISPLAY_WIDTH * VK_DISPLAY_BUFFER_LINES)
#define VK_DISPLAY_BUFFER_BYTES (VK_DISPLAY_BUFFER_PIXELS * VK_DISPLAY_BYTES_PER_PIXEL)
#define VK_DISPLAY_BUFFER_COUNT 2U
#define VK_DISPLAY_QUEUE_DEPTH 10U
#define VK_DISPLAY_MAX_TRANSFER_BYTES \
    (VK_DISPLAY_WIDTH * VK_DISPLAY_HEIGHT * VK_DISPLAY_BYTES_PER_PIXEL)
#define VK_DISPLAY_PIXEL_CLOCK_HZ 40000000U
#define VK_DISPLAY_X_GAP 14
#define VK_DISPLAY_Y_GAP 13

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t bytes_per_pixel;
    uint16_t buffer_lines;
    uint8_t buffer_count;
    uint8_t queue_depth;
    uint32_t pixel_clock_hz;
    int16_t x_gap;
    int16_t y_gap;
    bool rgb565;
    bool swap_bytes;
    bool dma_capable;
    bool psram_enabled;
    bool software_rotate;
    bool full_refresh;
    bool direct_mode;
} vk_display_profile_t;

typedef struct {
    bool store_ready;
    bool screen_owner_ready;
    bool font_profile_ready;
    bool physical_acceptance_admitted;
} vk_display_dependencies_t;

typedef struct {
    atomic_flag lock;
    bool started;
    bool stopping;
    bool tainted;
    bool transport_ready;
    bool screen_available;
    uint8_t in_flight_flushes;
    uint32_t next_flush_token;
    uint32_t active_flush_tokens[VK_DISPLAY_QUEUE_DEPTH];
    vk_display_profile_t profile;
    vk_display_dependencies_t dependencies;
} vk_display_t;

const vk_display_profile_t *vk_display_product_profile(void);
esp_err_t vk_display_validate_profile(const vk_display_profile_t *profile);
esp_err_t vk_display_start(vk_display_t *display, const vk_display_profile_t *profile,
                           bool panel_ready, bool lvgl_display_ready);
esp_err_t vk_display_set_dependencies(vk_display_t *display,
                                      const vk_display_dependencies_t *dependencies);
esp_err_t vk_display_begin_flush(vk_display_t *display, int32_t x1, int32_t y1,
                                 int32_t x2, int32_t y2, size_t byte_count);
esp_err_t vk_display_begin_flush_token(vk_display_t *display, int32_t x1, int32_t y1,
                                       int32_t x2, int32_t y2, size_t byte_count,
                                       uint32_t *token);
esp_err_t vk_display_complete_flush(vk_display_t *display, esp_err_t transport_result);
esp_err_t vk_display_complete_flush_token(vk_display_t *display, uint32_t token,
                                          esp_err_t transport_result);
esp_err_t vk_display_stop(vk_display_t *display);
bool vk_display_transport_ready(vk_display_t *display);
bool vk_display_screen_available(vk_display_t *display);
bool vk_display_is_tainted(vk_display_t *display);

#ifdef ESP_PLATFORM
/* Production composition remains unavailable until the physical acceptance gate is admitted. */
esp_err_t vk_display_product_init(void);
esp_err_t vk_display_product_deinit(void);
bool vk_display_product_screen_available(void);
vk_display_t *vk_display_product_instance(void);
esp_err_t vk_display_product_set_dependencies(const vk_display_dependencies_t *dependencies);
/* Hooks the display owner's in-flight/taint accounting to LVGL's actual flush events. */
esp_err_t vk_display_product_bind_lvgl_flush(void);
/* Panel/board transport owners call this before LV_EVENT_FLUSH_FINISH when the
 * asynchronous transfer reports a concrete result. It consumes exactly the
 * oldest correlated token; the subsequent LVGL finish event is a no-op. */
esp_err_t vk_display_product_complete_flush(esp_err_t transport_result);
#endif

#ifdef __cplusplus
}
#endif
