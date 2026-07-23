#pragma once

#include <stddef.h>
#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t vk_usb_led_command_decode(const vk_usb_json_document_t *document, uint16_t root,
                                    const vk_usb_led_capability_t *capability,
                                    vk_usb_led_command_t *command);
esp_err_t vk_usb_led_state_encode(const vk_usb_led_state_event_t *event,
                                  char *output, size_t capacity, size_t *output_length);
esp_err_t vk_usb_led_error_encode(const vk_usb_led_error_event_t *event,
                                  char *output, size_t capacity, size_t *output_length);

#ifdef __cplusplus
}
#endif
