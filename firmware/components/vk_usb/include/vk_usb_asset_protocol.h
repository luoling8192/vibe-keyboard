#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t vk_usb_asset_command_decode(const vk_usb_json_document_t *document,
                                      uint16_t root,
                                      const vk_usb_assets_capability_t *capability,
                                      uint32_t expected_epoch,
                                      uint32_t snapshot_generation,
                                      vk_usb_asset_command_t *command);

esp_err_t vk_usb_screen_command_decode(const vk_usb_json_document_t *document,
                                       uint16_t root,
                                       const vk_usb_screen_capability_t *capability,
                                       uint32_t display_width,
                                       uint32_t display_height,
                                       uint32_t expected_epoch,
                                       uint32_t snapshot_generation,
                                       vk_usb_screen_command_t *command);
esp_err_t vk_usb_widget_command_decode(const vk_usb_json_document_t *document,
                                       uint16_t root,
                                       const vk_usb_screen_capability_t *capability,
                                       uint32_t expected_epoch,
                                       uint32_t snapshot_generation,
                                       vk_usb_widget_command_t *command);

esp_err_t vk_usb_asset_event_encode(const vk_usb_asset_event_t *event,
                                    char *output,
                                    size_t capacity,
                                    size_t *length);
esp_err_t vk_usb_screen_event_encode(const vk_usb_screen_event_t *event,
                                     char *output,
                                     size_t capacity,
                                     size_t *length);
esp_err_t vk_usb_widget_event_encode(const vk_usb_widget_event_t *event,
                                     char *output,
                                     size_t capacity,
                                     size_t *length);
esp_err_t vk_usb_protocol_error_encode(const vk_usb_protocol_error_t *error,
                                       char *output,
                                       size_t capacity,
                                       size_t *length);

#ifdef __cplusplus
}
#endif
