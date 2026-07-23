#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t vk_nv3007_validate_swap_xy(bool swap_axes);
esp_err_t vk_nv3007_validate_mirror(bool mirror_x, bool mirror_y);

#ifdef __cplusplus
}
#endif
