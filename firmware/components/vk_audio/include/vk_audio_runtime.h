#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_AUDIO_JOIN_TIMEOUT_MS 1500U

typedef struct {
    esp_err_t (*prepare)(void *context);
    esp_err_t (*create_worker)(void *context);
    bool (*wait_started)(void *context, uint32_t timeout_ms);
    esp_err_t (*startup_error)(void *context);
    void (*request_stop)(void *context);
    bool (*wait_stopped)(void *context, uint32_t timeout_ms);
    esp_err_t (*runtime_error)(void *context);
    esp_err_t (*cleanup_error)(void *context);
    esp_err_t (*retry_cleanup)(void *context);
    void (*release)(void *context);
    void *context;
} vk_audio_runtime_ops_t;

typedef enum {
    VK_AUDIO_RUNTIME_IDLE = 0,
    VK_AUDIO_RUNTIME_STARTING,
    VK_AUDIO_RUNTIME_RUNNING,
    VK_AUDIO_RUNTIME_STOPPING,
    VK_AUDIO_RUNTIME_TAINTED,
} vk_audio_runtime_state_t;

typedef struct {
    vk_audio_runtime_ops_t ops;
    vk_audio_runtime_state_t state;
    bool retained;
    bool stopped_observed;
} vk_audio_runtime_t;

void vk_audio_runtime_init(vk_audio_runtime_t *runtime,
                           const vk_audio_runtime_ops_t *ops);
esp_err_t vk_audio_runtime_start(vk_audio_runtime_t *runtime);
esp_err_t vk_audio_runtime_stop(vk_audio_runtime_t *runtime);
esp_err_t vk_audio_runtime_collect(vk_audio_runtime_t *runtime, uint32_t timeout_ms);
bool vk_audio_runtime_is_active(const vk_audio_runtime_t *runtime);
bool vk_audio_runtime_is_tainted(const vk_audio_runtime_t *runtime);

#ifdef __cplusplus
}
#endif
