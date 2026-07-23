#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

bool vk_usb_capability_snapshot_validate(const vk_usb_capability_snapshot_t *snapshot,
                                         bool update_boot_policy_enabled);
esp_err_t vk_usb_capability_snapshot_encode(const vk_usb_capability_snapshot_t *snapshot,
                                            char *output, size_t capacity,
                                            size_t *output_length);

#ifdef __cplusplus
}
#endif
