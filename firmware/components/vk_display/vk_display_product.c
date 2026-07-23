#include "vk_display.h"

#include "vk_board.h"
#include "lvgl.h"
#include <string.h>

/* The connected product profile is the exact 428x142 RGB565 Vibe Board panel. */
#define VK_DISPLAY_PHYSICAL_ACCEPTANCE_ADMITTED 1

static vk_display_t s_display;
static uint32_t s_flush_tokens[VK_DISPLAY_QUEUE_DEPTH];
static uint8_t s_flush_head;
static uint8_t s_flush_count;
static uint8_t s_transport_completions_pending_finish;

static bool push_flush_token(uint32_t token)
{
    if (s_flush_count >= VK_DISPLAY_QUEUE_DEPTH) return false;
    s_flush_tokens[(uint8_t)((s_flush_head + s_flush_count) % VK_DISPLAY_QUEUE_DEPTH)] = token;
    ++s_flush_count;
    return true;
}

static bool pop_flush_token(uint32_t *token)
{
    if (token == NULL || s_flush_count == 0U) return false;
    *token = s_flush_tokens[s_flush_head];
    s_flush_tokens[s_flush_head] = 0U;
    s_flush_head = (uint8_t)((s_flush_head + 1U) % VK_DISPLAY_QUEUE_DEPTH);
    --s_flush_count;
    return true;
}

esp_err_t vk_display_product_complete_flush(esp_err_t transport_result)
{
    uint32_t token = 0U;
    if (!pop_flush_token(&token)) return ESP_ERR_NOT_FOUND;
    esp_err_t result = vk_display_complete_flush_token(&s_display, token, transport_result);
    if (s_transport_completions_pending_finish < UINT8_MAX) ++s_transport_completions_pending_finish;
    return result;
}

static void display_flush_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FLUSH_START) {
        lv_area_t *area = lv_event_get_invalidated_area(event);
        if (area == NULL) return;
        size_t width = (size_t)(area->x2 - area->x1 + 1), height = (size_t)(area->y2 - area->y1 + 1);
        size_t bytes = width <= SIZE_MAX / height && width * height <= SIZE_MAX / VK_DISPLAY_BYTES_PER_PIXEL ?
                       width * height * VK_DISPLAY_BYTES_PER_PIXEL : SIZE_MAX;
        uint32_t token = 0U;
        if (vk_display_begin_flush_token(&s_display, area->x1, area->y1, area->x2 + 1, area->y2 + 1, bytes, &token) == ESP_OK &&
            !push_flush_token(token)) (void)vk_display_complete_flush_token(&s_display, token, ESP_ERR_NO_MEM);
    } else if (code == LV_EVENT_FLUSH_FINISH) {
        if (s_transport_completions_pending_finish != 0U) --s_transport_completions_pending_finish;
        else (void)vk_display_product_complete_flush(ESP_OK);
    }
}

esp_err_t vk_display_product_bind_lvgl_flush(void)
{
    lv_display_t *display = vk_board_display();
    if (display == NULL || !vk_display_transport_ready(&s_display)) return ESP_ERR_INVALID_STATE;
    lv_display_add_event_cb(display, display_flush_event, LV_EVENT_FLUSH_START, &s_display);
    lv_display_add_event_cb(display, display_flush_event, LV_EVENT_FLUSH_FINISH, &s_display);
    return ESP_OK;
}

esp_err_t vk_display_product_init(void)
{
    memset(s_flush_tokens, 0, sizeof(s_flush_tokens));
    s_flush_head = 0U;
    s_flush_count = 0U;
    s_transport_completions_pending_finish = 0U;
    esp_lcd_panel_handle_t panel = vk_board_panel();
    lv_display_t *lvgl_display = vk_board_display();
    esp_err_t error = vk_display_start(&s_display, vk_display_product_profile(),
                                       panel != NULL, lvgl_display != NULL);
    if (error != ESP_OK) return error;
    error = vk_display_product_bind_lvgl_flush();
    if (error != ESP_OK) { (void)vk_display_stop(&s_display); return error; }

    /* Register a transport completion hook with the board so that asynchronous
     * panel transfer results can propagate into the display owner's
     * in-flight/taint accounting independently of LV_EVENT_FLUSH_FINISH.
     * The happy-path LVGL finish event is a no-op once the transport has
     * already completed the token. */
    vk_board_set_display_flush_callback(vk_display_product_complete_flush);

    const vk_display_dependencies_t dependencies = {
        .store_ready = false,
        .screen_owner_ready = false,
        .font_profile_ready = false,
        .physical_acceptance_admitted = VK_DISPLAY_PHYSICAL_ACCEPTANCE_ADMITTED != 0,
    };
    error = vk_display_set_dependencies(&s_display, &dependencies);
    if (error != ESP_OK) (void)vk_display_stop(&s_display);
    return error;
}

esp_err_t vk_display_product_deinit(void)
{
    return vk_display_stop(&s_display);
}

bool vk_display_product_screen_available(void)
{
    return vk_display_screen_available(&s_display);
}

vk_display_t *vk_display_product_instance(void)
{
    return &s_display;
}

esp_err_t vk_display_product_set_dependencies(const vk_display_dependencies_t *dependencies)
{
    if (dependencies == NULL || dependencies->physical_acceptance_admitted !=
        (VK_DISPLAY_PHYSICAL_ACCEPTANCE_ADMITTED != 0)) return ESP_ERR_INVALID_ARG;
    return vk_display_set_dependencies(&s_display, dependencies);
}
