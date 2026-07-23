#include "vk_audio_runtime.h"

#include <string.h>

static bool valid(const vk_audio_runtime_t *runtime)
{
    return runtime != NULL && runtime->ops.prepare != NULL &&
           runtime->ops.create_worker != NULL && runtime->ops.wait_started != NULL &&
           runtime->ops.startup_error != NULL && runtime->ops.request_stop != NULL &&
           runtime->ops.wait_stopped != NULL && runtime->ops.runtime_error != NULL &&
           runtime->ops.cleanup_error != NULL && runtime->ops.retry_cleanup != NULL &&
           runtime->ops.release != NULL;
}

void vk_audio_runtime_init(vk_audio_runtime_t *runtime,
                           const vk_audio_runtime_ops_t *ops)
{
    if (runtime == NULL) return;
    memset(runtime, 0, sizeof(*runtime));
    if (ops != NULL) runtime->ops = *ops;
}

esp_err_t vk_audio_runtime_start(vk_audio_runtime_t *runtime)
{
    if (!valid(runtime) || runtime->state != VK_AUDIO_RUNTIME_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime->state = VK_AUDIO_RUNTIME_STARTING;
    esp_err_t error = runtime->ops.prepare(runtime->ops.context);
    if (error != ESP_OK) {
        runtime->state = VK_AUDIO_RUNTIME_IDLE;
        return error;
    }
    error = runtime->ops.create_worker(runtime->ops.context);
    if (error != ESP_OK) {
        runtime->stopped_observed = true;
        runtime->ops.release(runtime->ops.context);
        runtime->state = VK_AUDIO_RUNTIME_IDLE;
        return error;
    }
    runtime->stopped_observed = false;
    if (!runtime->ops.wait_started(runtime->ops.context, VK_AUDIO_JOIN_TIMEOUT_MS)) {
        runtime->ops.request_stop(runtime->ops.context);
        if (!runtime->ops.wait_stopped(runtime->ops.context, VK_AUDIO_JOIN_TIMEOUT_MS)) {
            runtime->retained = true;
            runtime->state = VK_AUDIO_RUNTIME_TAINTED;
            return ESP_ERR_TIMEOUT;
        }
        runtime->stopped_observed = true;
        runtime->ops.release(runtime->ops.context);
        runtime->state = VK_AUDIO_RUNTIME_IDLE;
        return ESP_ERR_TIMEOUT;
    }
    error = runtime->ops.startup_error(runtime->ops.context);
    if (error != ESP_OK) {
        runtime->ops.request_stop(runtime->ops.context);
        if (!runtime->ops.wait_stopped(runtime->ops.context, VK_AUDIO_JOIN_TIMEOUT_MS)) {
            runtime->retained = true;
            runtime->state = VK_AUDIO_RUNTIME_TAINTED;
            return ESP_ERR_TIMEOUT;
        }
        runtime->stopped_observed = true;
        runtime->ops.release(runtime->ops.context);
        runtime->state = VK_AUDIO_RUNTIME_IDLE;
        return error;
    }
    runtime->state = VK_AUDIO_RUNTIME_RUNNING;
    return ESP_OK;
}

esp_err_t vk_audio_runtime_stop(vk_audio_runtime_t *runtime)
{
    if (!valid(runtime)) return ESP_ERR_INVALID_STATE;
    if (runtime->state == VK_AUDIO_RUNTIME_IDLE) return ESP_OK;
    if (runtime->state == VK_AUDIO_RUNTIME_TAINTED ||
        runtime->state == VK_AUDIO_RUNTIME_STARTING ||
        runtime->state == VK_AUDIO_RUNTIME_STOPPING) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime->state = VK_AUDIO_RUNTIME_STOPPING;
    runtime->ops.request_stop(runtime->ops.context);
    if (!runtime->ops.wait_stopped(runtime->ops.context, VK_AUDIO_JOIN_TIMEOUT_MS)) {
        runtime->retained = true;
        runtime->state = VK_AUDIO_RUNTIME_TAINTED;
        return ESP_ERR_TIMEOUT;
    }
    runtime->stopped_observed = true;
    esp_err_t runtime_error = runtime->ops.runtime_error(runtime->ops.context);
    esp_err_t cleanup_error = runtime->ops.cleanup_error(runtime->ops.context);
    if (cleanup_error != ESP_OK) {
        runtime->retained = true;
        runtime->state = VK_AUDIO_RUNTIME_TAINTED;
        return cleanup_error;
    }
    runtime->ops.release(runtime->ops.context);
    runtime->retained = false;
    runtime->state = VK_AUDIO_RUNTIME_IDLE;
    return runtime_error;
}

esp_err_t vk_audio_runtime_collect(vk_audio_runtime_t *runtime, uint32_t timeout_ms)
{
    if (!valid(runtime) || runtime->state != VK_AUDIO_RUNTIME_TAINTED ||
        !runtime->retained) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!runtime->stopped_observed &&
        !runtime->ops.wait_stopped(runtime->ops.context, timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    runtime->stopped_observed = true;
    esp_err_t cleanup_error = runtime->ops.cleanup_error(runtime->ops.context);
    if (cleanup_error != ESP_OK) {
        cleanup_error = runtime->ops.retry_cleanup(runtime->ops.context);
        if (cleanup_error != ESP_OK) return cleanup_error;
    }
    runtime->ops.release(runtime->ops.context);
    runtime->retained = false;
    /* Collection proves destruction only. Taint is intentionally sticky, so a
     * failed composition cannot restart without a full process reboot. */
    return ESP_OK;
}

bool vk_audio_runtime_is_active(const vk_audio_runtime_t *runtime)
{
    return runtime != NULL && runtime->state == VK_AUDIO_RUNTIME_RUNNING;
}

bool vk_audio_runtime_is_tainted(const vk_audio_runtime_t *runtime)
{
    return runtime != NULL && runtime->state == VK_AUDIO_RUNTIME_TAINTED;
}
