#include "vk_board_lifecycle.h"

static esp_err_t record_cleanup_error(vk_board_lifecycle_t *lifecycle, esp_err_t error)
{
    if (error != ESP_OK) {
        if (lifecycle->cleanup_error == ESP_OK) {
            lifecycle->cleanup_error = error;
        }
        lifecycle->tainted = true;
    }
    return lifecycle->cleanup_error;
}

esp_err_t vk_board_lifecycle_start(vk_board_lifecycle_t *lifecycle)
{
    if (lifecycle == NULL || lifecycle->stages == NULL || lifecycle->stage_count == 0 ||
            lifecycle->acquired_count != 0 || lifecycle->tainted) {
        return ESP_ERR_INVALID_STATE;
    }

    lifecycle->cleanup_error = ESP_OK;
    for (size_t index = 0; index < lifecycle->stage_count; ++index) {
        const vk_board_lifecycle_stage_t *stage = &lifecycle->stages[index];
        if (stage->acquire == NULL || stage->release == NULL) {
            (void)vk_board_lifecycle_cleanup(lifecycle);
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t error = stage->acquire(lifecycle->context);
        if (error != ESP_OK) {
            (void)vk_board_lifecycle_cleanup(lifecycle);
            return error;
        }
        lifecycle->acquired_count = index + 1;
    }
    return ESP_OK;
}

esp_err_t vk_board_lifecycle_cleanup(vk_board_lifecycle_t *lifecycle)
{
    if (lifecycle == NULL || lifecycle->stages == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (lifecycle->acquired_count > 0) {
        --lifecycle->acquired_count;
        const vk_board_lifecycle_stage_t *stage = &lifecycle->stages[lifecycle->acquired_count];
        if (stage->release != NULL) {
            (void)record_cleanup_error(lifecycle, stage->release(lifecycle->context));
        }
    }
    return lifecycle->cleanup_error;
}

bool vk_board_lifecycle_is_tainted(const vk_board_lifecycle_t *lifecycle)
{
    return lifecycle != NULL && lifecycle->tainted;
}
