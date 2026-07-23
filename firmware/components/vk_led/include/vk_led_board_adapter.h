#pragma once

#include "vk_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *context;
    esp_err_t (*set_pixel)(void *context, uint8_t physical_index,
                           uint8_t red, uint8_t green, uint8_t blue);
    esp_err_t (*clear)(void *context);
    esp_err_t (*refresh)(void *context);
    esp_err_t (*release)(void *context);
} vk_led_board_ops_t;

typedef struct {
    vk_led_board_ops_t board;
    vk_led_profile_t profile;
    bool admitted;
    bool tainted;
} vk_led_board_adapter_t;

/* The adapter is admitted only with a reviewed profile. It never exposes the strip handle. */
esp_err_t vk_led_board_adapter_init(vk_led_board_adapter_t *adapter,
                                    const vk_led_board_ops_t *board,
                                    const vk_led_profile_t *profile);
void vk_led_board_adapter_transport(vk_led_board_adapter_t *adapter,
                                    vk_led_transport_ops_t *transport);

#ifdef __cplusplus
}
#endif
