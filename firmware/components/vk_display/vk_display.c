#include "vk_display.h"

#include <string.h>

static const vk_display_profile_t s_product_profile = {
    .width = VK_DISPLAY_WIDTH,
    .height = VK_DISPLAY_HEIGHT,
    .bytes_per_pixel = VK_DISPLAY_BYTES_PER_PIXEL,
    .buffer_lines = VK_DISPLAY_BUFFER_LINES,
    .buffer_count = VK_DISPLAY_BUFFER_COUNT,
    .queue_depth = VK_DISPLAY_QUEUE_DEPTH,
    .pixel_clock_hz = VK_DISPLAY_PIXEL_CLOCK_HZ,
    .x_gap = VK_DISPLAY_X_GAP,
    .y_gap = VK_DISPLAY_Y_GAP,
    .rgb565 = true,
    .swap_bytes = true,
    .dma_capable = true,
    .psram_enabled = true,
    .software_rotate = false,
    .full_refresh = false,
    .direct_mode = false,
};

static void lock_display(vk_display_t *display)
{
    while (atomic_flag_test_and_set_explicit(&display->lock, memory_order_acquire)) {
    }
}

static void unlock_display(vk_display_t *display)
{
    atomic_flag_clear_explicit(&display->lock, memory_order_release);
}

static bool dependencies_ready(const vk_display_dependencies_t *dependencies)
{
    return dependencies->store_ready && dependencies->screen_owner_ready &&
           dependencies->font_profile_ready && dependencies->physical_acceptance_admitted;
}

const vk_display_profile_t *vk_display_product_profile(void)
{
    return &s_product_profile;
}

esp_err_t vk_display_validate_profile(const vk_display_profile_t *profile)
{
    if (profile == NULL) return ESP_ERR_INVALID_ARG;
    if (profile->width != VK_DISPLAY_WIDTH || profile->height != VK_DISPLAY_HEIGHT ||
        profile->bytes_per_pixel != VK_DISPLAY_BYTES_PER_PIXEL ||
        profile->buffer_lines != VK_DISPLAY_BUFFER_LINES ||
        profile->buffer_count != VK_DISPLAY_BUFFER_COUNT ||
        profile->queue_depth != VK_DISPLAY_QUEUE_DEPTH ||
        profile->pixel_clock_hz != VK_DISPLAY_PIXEL_CLOCK_HZ ||
        profile->x_gap != VK_DISPLAY_X_GAP || profile->y_gap != VK_DISPLAY_Y_GAP ||
        !profile->rgb565 || !profile->swap_bytes || !profile->dma_capable ||
        !profile->psram_enabled || profile->software_rotate || profile->full_refresh ||
        profile->direct_mode) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t vk_display_start(vk_display_t *display, const vk_display_profile_t *profile,
                           bool panel_ready, bool lvgl_display_ready)
{
    if (display == NULL || vk_display_validate_profile(profile) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(display, 0, sizeof(*display));
    atomic_flag_clear(&display->lock);
    display->profile = *profile;
    if (!panel_ready || !lvgl_display_ready) return ESP_ERR_INVALID_STATE;
    display->transport_ready = true;
    display->started = true;
    return ESP_OK;
}

esp_err_t vk_display_set_dependencies(vk_display_t *display,
                                      const vk_display_dependencies_t *dependencies)
{
    if (display == NULL || dependencies == NULL) return ESP_ERR_INVALID_ARG;
    lock_display(display);
    if (!display->started || display->stopping || display->tainted) {
        unlock_display(display);
        return ESP_ERR_INVALID_STATE;
    }
    display->dependencies = *dependencies;
    display->screen_available = display->transport_ready && dependencies_ready(dependencies);
    unlock_display(display);
    return ESP_OK;
}

esp_err_t vk_display_begin_flush_token(vk_display_t *display, int32_t x1, int32_t y1,
                                       int32_t x2, int32_t y2, size_t byte_count,
                                       uint32_t *token)
{
    if (display == NULL || x1 < 0 || y1 < 0 || x2 <= x1 || y2 <= y1 ||
        x2 > (int32_t)VK_DISPLAY_WIDTH || y2 > (int32_t)VK_DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t width = (size_t)(x2 - x1);
    size_t height = (size_t)(y2 - y1);
    if (height > VK_DISPLAY_BUFFER_LINES || width > SIZE_MAX / height ||
        width * height > SIZE_MAX / VK_DISPLAY_BYTES_PER_PIXEL ||
        byte_count != width * height * VK_DISPLAY_BYTES_PER_PIXEL ||
        byte_count > VK_DISPLAY_BUFFER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    lock_display(display);
    if (!display->started || display->stopping || display->tainted ||
        !display->transport_ready || display->in_flight_flushes >= VK_DISPLAY_QUEUE_DEPTH) {
        unlock_display(display);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t assigned = ++display->next_flush_token;
    if (assigned == 0U) assigned = ++display->next_flush_token;
    display->active_flush_tokens[display->in_flight_flushes++] = assigned;
    if (token != NULL) *token = assigned;
    unlock_display(display);
    return ESP_OK;
}

esp_err_t vk_display_begin_flush(vk_display_t *display, int32_t x1, int32_t y1,
                                 int32_t x2, int32_t y2, size_t byte_count)
{
    return vk_display_begin_flush_token(display, x1, y1, x2, y2, byte_count, NULL);
}

esp_err_t vk_display_complete_flush_token(vk_display_t *display, uint32_t token,
                                          esp_err_t transport_result)
{
    if (display == NULL) return ESP_ERR_INVALID_ARG;
    lock_display(display);
    size_t index = 0U;
    if (!display->started || display->in_flight_flushes == 0U) { unlock_display(display); return ESP_ERR_INVALID_STATE; }
    if (token != 0U) {
        while (index < display->in_flight_flushes && display->active_flush_tokens[index] != token) ++index;
        if (index == display->in_flight_flushes) { unlock_display(display); return ESP_ERR_INVALID_STATE; }
    }
    for (size_t cursor = index + 1U; cursor < display->in_flight_flushes; ++cursor)
        display->active_flush_tokens[cursor - 1U] = display->active_flush_tokens[cursor];
    display->active_flush_tokens[--display->in_flight_flushes] = 0U;
    if (transport_result != ESP_OK) {
        display->tainted = true;
        display->transport_ready = false;
        display->screen_available = false;
    }
    unlock_display(display);
    return transport_result;
}

esp_err_t vk_display_complete_flush(vk_display_t *display, esp_err_t transport_result)
{
    return vk_display_complete_flush_token(display, 0U, transport_result);
}

esp_err_t vk_display_stop(vk_display_t *display)
{
    if (display == NULL) return ESP_ERR_INVALID_ARG;
    lock_display(display);
    if (!display->started) {
        unlock_display(display);
        return ESP_OK;
    }
    display->stopping = true;
    display->screen_available = false;
    if (display->in_flight_flushes != 0U) {
        display->tainted = true;
        unlock_display(display);
        return ESP_ERR_TIMEOUT;
    }
    display->transport_ready = false;
    display->started = false;
    unlock_display(display);
    return ESP_OK;
}

bool vk_display_transport_ready(vk_display_t *display)
{
    if (display == NULL) return false;
    lock_display(display);
    bool ready = display->started && display->transport_ready && !display->tainted;
    unlock_display(display);
    return ready;
}

bool vk_display_screen_available(vk_display_t *display)
{
    if (display == NULL) return false;
    lock_display(display);
    bool available = display->started && display->screen_available && !display->tainted;
    unlock_display(display);
    return available;
}

bool vk_display_is_tainted(vk_display_t *display)
{
    if (display == NULL) return true;
    lock_display(display);
    bool tainted = display->tainted;
    unlock_display(display);
    return tainted;
}
