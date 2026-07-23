#include "vk_usb_runtime.h"

#include <string.h>

#define VK_USB_RUNTIME_START_TIMEOUT_MS 2000U
#define VK_USB_RUNTIME_STOP_TIMEOUT_MS 3000U

void vk_usb_runtime_init(vk_usb_runtime_t *runtime, const vk_usb_runtime_ops_t *ops)
{
    if (runtime == NULL) return;
    memset(runtime, 0, sizeof(*runtime));
    if (ops != NULL) runtime->ops = *ops;
}

static bool valid(const vk_usb_runtime_t *runtime)
{
    return runtime != NULL && runtime->ops.prepare != NULL && runtime->ops.create_task != NULL && runtime->ops.wait_started != NULL && runtime->ops.request_stop != NULL && runtime->ops.wait_stopped != NULL && runtime->ops.startup_error != NULL && runtime->ops.runtime_error != NULL && runtime->ops.cleanup_error != NULL && runtime->ops.release != NULL;
}

esp_err_t vk_usb_runtime_start(vk_usb_runtime_t *runtime)
{
    if (!valid(runtime) || runtime->state != VK_USB_RUNTIME_IDLE) return ESP_ERR_INVALID_STATE;
    esp_err_t error = runtime->ops.prepare(runtime->ops.context);
    if (error != ESP_OK) return error;
    error = runtime->ops.create_task(runtime->ops.context);
    if (error != ESP_OK) { runtime->ops.release(runtime->ops.context); return error; }
    if (!runtime->ops.wait_started(runtime->ops.context, VK_USB_RUNTIME_START_TIMEOUT_MS)) {
        runtime->ops.request_stop(runtime->ops.context);
        if (!runtime->ops.wait_stopped(runtime->ops.context, VK_USB_RUNTIME_START_TIMEOUT_MS)) {
            runtime->state = VK_USB_RUNTIME_TAINTED;
            return ESP_ERR_TIMEOUT; /* Task retains its context until externally proven stopped. */
        }
        runtime->ops.release(runtime->ops.context);
        return ESP_ERR_TIMEOUT;
    }
    error = runtime->ops.startup_error(runtime->ops.context);
    if (error != ESP_OK) {
        if (!runtime->ops.wait_stopped(runtime->ops.context, VK_USB_RUNTIME_STOP_TIMEOUT_MS)) {
            runtime->state = VK_USB_RUNTIME_TAINTED;
            return ESP_ERR_TIMEOUT;
        }
        runtime->ops.release(runtime->ops.context);
        return error;
    }
    runtime->state = VK_USB_RUNTIME_RUNNING;
    return ESP_OK;
}

esp_err_t vk_usb_runtime_stop(vk_usb_runtime_t *runtime)
{
    if (!valid(runtime)) return ESP_ERR_INVALID_STATE;
    if (runtime->state == VK_USB_RUNTIME_IDLE) return ESP_OK;
    if (runtime->state == VK_USB_RUNTIME_TAINTED) return ESP_ERR_INVALID_STATE;
    runtime->ops.request_stop(runtime->ops.context);
    if (!runtime->ops.wait_stopped(runtime->ops.context, VK_USB_RUNTIME_STOP_TIMEOUT_MS)) {
        runtime->state = VK_USB_RUNTIME_TAINTED;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t runtime_error = runtime->ops.runtime_error(runtime->ops.context);
    esp_err_t cleanup_error = runtime->ops.cleanup_error(runtime->ops.context);
    if (cleanup_error != ESP_OK) {
        runtime->state = VK_USB_RUNTIME_TAINTED;
        return cleanup_error; /* Driver ownership is retained until cleanup is proven. */
    }
    runtime->ops.release(runtime->ops.context);
    runtime->state = VK_USB_RUNTIME_IDLE;
    return runtime_error;
}

bool vk_usb_runtime_is_tainted(const vk_usb_runtime_t *runtime)
{
    return runtime != NULL && runtime->state == VK_USB_RUNTIME_TAINTED;
}
