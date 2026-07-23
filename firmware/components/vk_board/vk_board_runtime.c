#include <stddef.h>
#include <string.h>
#ifndef VK_BOARD_RUNTIME_NATIVE
#include "vk_board.h"
#else
#define VK_LCD_BACKLIGHT_INITIAL_DUTY 0
#define VK_LCD_BACKLIGHT_ON_DUTY 255
#define VK_LVGL_DEINIT_TIMEOUT_MS 1000
#endif
#include "vk_board_runtime.h"

#define VK_BOARD_STAGE_COUNT 11

static esp_err_t first_error(esp_err_t first, esp_err_t next)
{
    return first != ESP_OK ? first : next;
}

static void mark_partial_cleanup_error(vk_board_runtime_t *runtime, esp_err_t error)
{
    if (error != ESP_OK) {
        if (runtime->lifecycle.cleanup_error == ESP_OK) {
            runtime->lifecycle.cleanup_error = error;
        }
        runtime->lifecycle.tainted = true;
    }
}

static esp_err_t acquire_keys(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->keys_config(runtime->ops_context);
}

static esp_err_t release_keys(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->keys_reset(runtime->ops_context);
}

static esp_err_t acquire_leds_off(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = runtime->ops->led_create(runtime->ops_context, &runtime->led_strip);
    if (error != ESP_OK) {
        return error;
    }
    error = runtime->ops->led_clear(runtime->ops_context, runtime->led_strip);
    if (error == ESP_OK) {
        error = runtime->ops->led_refresh(runtime->ops_context, runtime->led_strip);
    }
    if (error != ESP_OK) {
        esp_err_t cleanup_error = runtime->ops->led_clear(runtime->ops_context, runtime->led_strip);
        cleanup_error = first_error(cleanup_error, runtime->ops->led_refresh(runtime->ops_context, runtime->led_strip));
        cleanup_error = first_error(cleanup_error, runtime->ops->led_delete(runtime->ops_context, runtime->led_strip));
        runtime->led_strip = NULL;
        mark_partial_cleanup_error(runtime, cleanup_error);
    }
    return error;
}

static esp_err_t release_leds(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = ESP_OK;
    if (runtime->led_strip != NULL) {
        error = runtime->ops->led_clear(runtime->ops_context, runtime->led_strip);
        error = first_error(error, runtime->ops->led_refresh(runtime->ops_context, runtime->led_strip));
        error = first_error(error, runtime->ops->led_delete(runtime->ops_context, runtime->led_strip));
        runtime->led_strip = NULL;
    }
    return first_error(error, runtime->ops->led_gpio_reset(runtime->ops_context));
}

static esp_err_t force_backlight_off(vk_board_runtime_t *runtime)
{
    esp_err_t error = runtime->ops->backlight_set(runtime->ops_context, 0);
    return first_error(error, runtime->ops->backlight_stop(runtime->ops_context));
}

static esp_err_t acquire_backlight(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = runtime->ops->backlight_timer_config(runtime->ops_context);
    if (error != ESP_OK) {
        return error;
    }
    error = runtime->ops->backlight_channel_config(
        runtime->ops_context, VK_LCD_BACKLIGHT_INITIAL_DUTY
    );
    if (error != ESP_OK) {
        esp_err_t cleanup_error = force_backlight_off(runtime);
        cleanup_error = first_error(cleanup_error, runtime->ops->backlight_gpio_reset(runtime->ops_context));
        mark_partial_cleanup_error(runtime, cleanup_error);
    }
    return error;
}

static esp_err_t release_backlight(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = force_backlight_off(runtime);
    return first_error(error, runtime->ops->backlight_gpio_reset(runtime->ops_context));
}

static esp_err_t acquire_spi_bus(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->spi_init(runtime->ops_context);
}

static esp_err_t release_spi_bus(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->spi_free(runtime->ops_context);
}

static esp_err_t acquire_panel_io(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->panel_io_create(runtime->ops_context, &runtime->panel_io);
}

static esp_err_t release_panel_io(void *context)
{
    vk_board_runtime_t *runtime = context;
    if (runtime->panel_io == NULL) {
        return ESP_OK;
    }
    esp_err_t error = runtime->ops->panel_io_delete(runtime->ops_context, runtime->panel_io);
    runtime->panel_io = NULL;
    return error;
}

static esp_err_t acquire_reset_gpio(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->reset_gpio_config(runtime->ops_context);
}

static esp_err_t release_reset_gpio(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = runtime->ops->reset_set_low(runtime->ops_context);
    return first_error(error, runtime->ops->reset_gpio_reset(runtime->ops_context));
}

static esp_err_t acquire_lvgl(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->lvgl_init(runtime->ops_context);
}

static esp_err_t release_lvgl(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = runtime->ops->lvgl_deinit(runtime->ops_context);
    if (error != ESP_OK) {
        return error;
    }
    for (uint32_t elapsed = 0; elapsed < VK_LVGL_DEINIT_TIMEOUT_MS; ++elapsed) {
        if (!runtime->ops->lvgl_is_initialized(runtime->ops_context)) {
            return ESP_OK;
        }
        runtime->ops->delay_ms(runtime->ops_context, 1);
    }
    return runtime->ops->lvgl_is_initialized(runtime->ops_context) ? ESP_ERR_TIMEOUT : ESP_OK;
}

static esp_err_t acquire_panel(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->panel_create(
        runtime->ops_context, runtime->panel_io, &runtime->panel
    );
}

static esp_err_t release_panel(void *context)
{
    vk_board_runtime_t *runtime = context;
    if (runtime->panel == NULL) {
        return ESP_OK;
    }
    esp_err_t error = runtime->ops->panel_delete(runtime->ops_context, runtime->panel);
    runtime->panel = NULL;
    return error;
}

static esp_err_t initialize_panel(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = runtime->ops->panel_reset(runtime->ops_context, runtime->panel);
    if (error == ESP_OK) {
        error = runtime->ops->panel_init(runtime->ops_context, runtime->panel);
    }
    return error == ESP_OK
        ? runtime->ops->panel_power(runtime->ops_context, runtime->panel, true)
        : error;
}

static esp_err_t release_initialized_panel(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->panel == NULL
        ? ESP_OK
        : runtime->ops->panel_power(runtime->ops_context, runtime->panel, false);
}

static esp_err_t acquire_display(void *context)
{
    vk_board_runtime_t *runtime = context;
    return runtime->ops->display_add(
        runtime->ops_context, runtime->panel_io, runtime->panel, &runtime->display
    );
}

static esp_err_t release_display(void *context)
{
    vk_board_runtime_t *runtime = context;
    if (runtime->display == NULL) {
        return ESP_OK;
    }
    esp_err_t error = runtime->ops->display_remove(runtime->ops_context, runtime->display);
    runtime->display = NULL;
    return error;
}

static esp_err_t acquire_backlight_on(void *context)
{
    vk_board_runtime_t *runtime = context;
    esp_err_t error = runtime->ops->backlight_set(
        runtime->ops_context, VK_LCD_BACKLIGHT_ON_DUTY
    );
    if (error != ESP_OK) {
        mark_partial_cleanup_error(runtime, force_backlight_off(runtime));
    }
    return error;
}

static esp_err_t release_backlight_on(void *context)
{
    return force_backlight_off(context);
}

static const vk_board_lifecycle_stage_t s_stages[VK_BOARD_STAGE_COUNT] = {
    {acquire_keys, release_keys},
    {acquire_leds_off, release_leds},
    {acquire_backlight, release_backlight},
    {acquire_spi_bus, release_spi_bus},
    {acquire_panel_io, release_panel_io},
    {acquire_reset_gpio, release_reset_gpio},
    {acquire_lvgl, release_lvgl},
    {acquire_panel, release_panel},
    {initialize_panel, release_initialized_panel},
    {acquire_display, release_display},
    {acquire_backlight_on, release_backlight_on},
};

void vk_board_runtime_prepare(
    vk_board_runtime_t *runtime,
    const vk_board_runtime_ops_t *ops,
    void *ops_context
) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->ops = ops;
    runtime->ops_context = ops_context;
    runtime->lifecycle.stages = s_stages;
    runtime->lifecycle.stage_count = VK_BOARD_STAGE_COUNT;
    runtime->lifecycle.context = runtime;
}

esp_err_t vk_board_runtime_start(vk_board_runtime_t *runtime)
{
    if (runtime == NULL || runtime->ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return vk_board_lifecycle_start(&runtime->lifecycle);
}

esp_err_t vk_board_runtime_cleanup(vk_board_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return vk_board_lifecycle_cleanup(&runtime->lifecycle);
}

bool vk_board_runtime_is_tainted(const vk_board_runtime_t *runtime)
{
    return runtime != NULL && vk_board_lifecycle_is_tainted(&runtime->lifecycle);
}
