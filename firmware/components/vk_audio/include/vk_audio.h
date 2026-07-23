#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes synchronization and session state only. Capture remains stopped. */
esp_err_t vk_audio_init(void);
esp_err_t vk_audio_deinit(void);
esp_err_t vk_audio_prepare(uint32_t *session_id);
esp_err_t vk_audio_release(uint32_t session_id);
esp_err_t vk_audio_cancel_prepared(uint32_t session_id);
esp_err_t vk_audio_abort(void);
esp_err_t vk_audio_start(uint32_t *session_id);
esp_err_t vk_audio_stop(void);
bool vk_audio_is_active(void);
uint32_t vk_audio_session_id(void);
bool vk_audio_is_tainted(void);

typedef struct {
    esp_err_t (*prepare)(uint32_t *session_id);
    esp_err_t (*release)(uint32_t session_id);
    esp_err_t (*cancel_prepared)(uint32_t session_id);
    esp_err_t (*stop)(void);
    esp_err_t (*abort)(void);
    bool (*is_active)(void);
    uint32_t (*session_id)(void);
    /* Returns and clears one proven-quiescent asynchronous runtime failure. */
    bool (*take_runtime_failure)(uint32_t *session_id);
} vk_audio_control_api_t;

/* Typed dependency-injection boundary used by vk_input; no capture is started. */
const vk_audio_control_api_t *vk_audio_control_api(void);

#ifdef __cplusplus
}
#endif
