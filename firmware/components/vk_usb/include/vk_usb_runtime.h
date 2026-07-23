#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*prepare)(void *context);
    esp_err_t (*create_task)(void *context);
    bool (*wait_started)(void *context, uint32_t timeout_ms);
    void (*request_stop)(void *context);
    bool (*wait_stopped)(void *context, uint32_t timeout_ms);
    esp_err_t (*startup_error)(void *context);
    esp_err_t (*runtime_error)(void *context);
    esp_err_t (*cleanup_error)(void *context);
    void (*release)(void *context);
    void *context;
} vk_usb_runtime_ops_t;

typedef enum {
    VK_USB_RUNTIME_IDLE = 0,
    VK_USB_RUNTIME_RUNNING,
    VK_USB_RUNTIME_TAINTED,
} vk_usb_runtime_state_t;

typedef struct {
    vk_usb_runtime_ops_t ops;
    vk_usb_runtime_state_t state;
} vk_usb_runtime_t;

void vk_usb_runtime_init(vk_usb_runtime_t *runtime, const vk_usb_runtime_ops_t *ops);
esp_err_t vk_usb_runtime_start(vk_usb_runtime_t *runtime);
esp_err_t vk_usb_runtime_stop(vk_usb_runtime_t *runtime);
bool vk_usb_runtime_is_tainted(const vk_usb_runtime_t *runtime);

#ifdef __cplusplus
}
#endif
