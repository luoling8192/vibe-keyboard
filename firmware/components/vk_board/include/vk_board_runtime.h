#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "vk_board_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*keys_config)(void *context);
    esp_err_t (*keys_reset)(void *context);
    esp_err_t (*led_create)(void *context, void **strip);
    esp_err_t (*led_clear)(void *context, void *strip);
    esp_err_t (*led_refresh)(void *context, void *strip);
    esp_err_t (*led_delete)(void *context, void *strip);
    esp_err_t (*led_gpio_reset)(void *context);
    esp_err_t (*backlight_timer_config)(void *context);
    esp_err_t (*backlight_channel_config)(void *context, uint32_t duty);
    esp_err_t (*backlight_set)(void *context, uint32_t duty);
    esp_err_t (*backlight_stop)(void *context);
    esp_err_t (*backlight_gpio_reset)(void *context);
    esp_err_t (*spi_init)(void *context);
    esp_err_t (*spi_free)(void *context);
    esp_err_t (*panel_io_create)(void *context, void **panel_io);
    esp_err_t (*panel_io_delete)(void *context, void *panel_io);
    esp_err_t (*reset_gpio_config)(void *context);
    esp_err_t (*reset_set_low)(void *context);
    esp_err_t (*reset_gpio_reset)(void *context);
    esp_err_t (*lvgl_init)(void *context);
    esp_err_t (*lvgl_deinit)(void *context);
    bool (*lvgl_is_initialized)(void *context);
    void (*delay_ms)(void *context, uint32_t delay_ms);
    esp_err_t (*panel_create)(void *context, void *panel_io, void **panel);
    esp_err_t (*panel_delete)(void *context, void *panel);
    esp_err_t (*panel_reset)(void *context, void *panel);
    esp_err_t (*panel_init)(void *context, void *panel);
    esp_err_t (*panel_power)(void *context, void *panel, bool on);
    esp_err_t (*display_add)(void *context, void *panel_io, void *panel, void **display);
    esp_err_t (*display_remove)(void *context, void *display);
} vk_board_runtime_ops_t;

typedef struct {
    const vk_board_runtime_ops_t *ops;
    void *ops_context;
    void *led_strip;
    void *panel_io;
    void *panel;
    void *display;
    vk_board_lifecycle_t lifecycle;
} vk_board_runtime_t;

void vk_board_runtime_prepare(
    vk_board_runtime_t *runtime,
    const vk_board_runtime_ops_t *ops,
    void *ops_context
);
esp_err_t vk_board_runtime_start(vk_board_runtime_t *runtime);
esp_err_t vk_board_runtime_cleanup(vk_board_runtime_t *runtime);
bool vk_board_runtime_is_tainted(const vk_board_runtime_t *runtime);

#ifdef __cplusplus
}
#endif
