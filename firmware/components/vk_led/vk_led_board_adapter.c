#include "vk_led_board_adapter.h"

#include <string.h>

static esp_err_t force_off(vk_led_board_adapter_t *adapter)
{
    esp_err_t result = adapter->board.clear(adapter->board.context);
    esp_err_t refresh = adapter->board.refresh(adapter->board.context);
    if (result != ESP_OK || refresh != ESP_OK) {
        adapter->tainted = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t apply_frame(void *context,
                             const uint8_t logical_rgb[VK_LED_PIXEL_COUNT][3])
{
    vk_led_board_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->admitted || adapter->tainted) return ESP_ERR_INVALID_STATE;
    for (uint8_t logical = 0U; logical < VK_LED_PIXEL_COUNT; ++logical) {
        uint8_t physical = logical;
        if (logical < 4U) physical = adapter->profile.key_pixels[logical];
        esp_err_t result = adapter->board.set_pixel(adapter->board.context, physical,
                                                    logical_rgb[logical][0],
                                                    logical_rgb[logical][1],
                                                    logical_rgb[logical][2]);
        if (result != ESP_OK) {
            adapter->tainted = true;
            (void)force_off(adapter);
            return result;
        }
    }
    esp_err_t result = adapter->board.refresh(adapter->board.context);
    if (result != ESP_OK) {
        adapter->tainted = true;
        (void)force_off(adapter);
    }
    return result;
}

static esp_err_t apply_off(void *context)
{
    vk_led_board_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->admitted) return ESP_ERR_INVALID_STATE;
    return force_off(adapter);
}

static esp_err_t release_adapter(void *context)
{
    vk_led_board_adapter_t *adapter = context;
    if (adapter == NULL || !adapter->admitted) return ESP_ERR_INVALID_STATE;
    if (force_off(adapter) != ESP_OK) return ESP_FAIL;
    esp_err_t result = adapter->board.release(adapter->board.context);
    if (result == ESP_OK) adapter->admitted = false;
    else adapter->tainted = true;
    return result;
}

esp_err_t vk_led_board_adapter_init(vk_led_board_adapter_t *adapter,
                                    const vk_led_board_ops_t *board,
                                    const vk_led_profile_t *profile)
{
    if (adapter == NULL || board == NULL || board->set_pixel == NULL ||
        board->clear == NULL || board->refresh == NULL || board->release == NULL ||
        !vk_led_profile_validate(profile)) return ESP_ERR_INVALID_ARG;
    memset(adapter, 0, sizeof(*adapter));
    adapter->board = *board;
    adapter->profile = *profile;
    adapter->admitted = true;
    return ESP_OK;
}

void vk_led_board_adapter_transport(vk_led_board_adapter_t *adapter,
                                    vk_led_transport_ops_t *transport)
{
    if (transport == NULL) return;
    memset(transport, 0, sizeof(*transport));
    if (adapter == NULL || !adapter->admitted) return;
    transport->context = adapter;
    transport->apply_complete_frame = apply_frame;
    transport->apply_all_off = apply_off;
    transport->release = release_adapter;
}
