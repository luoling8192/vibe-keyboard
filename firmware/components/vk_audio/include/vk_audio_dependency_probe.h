#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compile/link probe for the pinned ESP-SR and micro-opus ABI.
 * This is not the production audio runtime.
 */
esp_err_t vk_audio_dependency_probe(void);

#ifdef __cplusplus
}
#endif
