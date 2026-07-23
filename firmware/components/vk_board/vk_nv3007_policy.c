#include "vk_nv3007_policy.h"

esp_err_t vk_nv3007_validate_swap_xy(bool swap_axes)
{
    return swap_axes ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

esp_err_t vk_nv3007_validate_mirror(bool mirror_x, bool mirror_y)
{
    return (mirror_x || mirror_y) ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}
