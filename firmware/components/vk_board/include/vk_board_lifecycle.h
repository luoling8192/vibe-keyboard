#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*vk_board_lifecycle_acquire_fn)(void *context);
typedef esp_err_t (*vk_board_lifecycle_release_fn)(void *context);

typedef struct {
    vk_board_lifecycle_acquire_fn acquire;
    vk_board_lifecycle_release_fn release;
} vk_board_lifecycle_stage_t;

typedef struct {
    const vk_board_lifecycle_stage_t *stages;
    size_t stage_count;
    size_t acquired_count;
    void *context;
    esp_err_t cleanup_error;
    bool tainted;
} vk_board_lifecycle_t;

esp_err_t vk_board_lifecycle_start(vk_board_lifecycle_t *lifecycle);
esp_err_t vk_board_lifecycle_cleanup(vk_board_lifecycle_t *lifecycle);
bool vk_board_lifecycle_is_tainted(const vk_board_lifecycle_t *lifecycle);

#ifdef __cplusplus
}
#endif
