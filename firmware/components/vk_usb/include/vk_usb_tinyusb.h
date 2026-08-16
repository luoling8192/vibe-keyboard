#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t vk_usb_tinyusb_connect(
    const vk_usb_uac_source_registration_t *source);
esp_err_t vk_usb_tinyusb_disconnect(void);
int vk_usb_tinyusb_read(uint8_t *bytes, size_t capacity, uint32_t timeout_ms);
int vk_usb_tinyusb_write(const uint8_t *bytes, size_t length,
                         uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
