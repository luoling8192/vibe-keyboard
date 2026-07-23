#include "vk_usb_facade.h"

#include <string.h>

static bool valid(const vk_usb_facade_t *facade)
{
    return facade != NULL && facade->ops.lock != NULL &&
           facade->ops.unlock != NULL && facade->ops.wait_tick != NULL;
}

void vk_usb_facade_init(vk_usb_facade_t *facade, const vk_usb_facade_ops_t *ops)
{
    if (facade == NULL) return;
    memset(facade, 0, sizeof(*facade));
    if (ops != NULL) facade->ops = *ops;
}

esp_err_t vk_usb_facade_publish(vk_usb_facade_t *facade, void *service)
{
    if (!valid(facade) || service == NULL) return ESP_ERR_INVALID_ARG;
    facade->ops.lock(facade->ops.context);
    if (facade->open || facade->service != NULL || facade->leases != 0U || facade->tainted) {
        facade->ops.unlock(facade->ops.context);
        return ESP_ERR_INVALID_STATE;
    }
    facade->service = service;
    facade->open = true;
    facade->ops.unlock(facade->ops.context);
    return ESP_OK;
}

esp_err_t vk_usb_facade_acquire(vk_usb_facade_t *facade, void **service)
{
    if (!valid(facade) || service == NULL) return ESP_ERR_INVALID_ARG;
    facade->ops.lock(facade->ops.context);
    if (!facade->open || facade->service == NULL || facade->tainted || facade->leases == UINT32_MAX) {
        facade->ops.unlock(facade->ops.context);
        return ESP_ERR_INVALID_STATE;
    }
    ++facade->leases;
    *service = facade->service;
    facade->ops.unlock(facade->ops.context);
    return ESP_OK;
}

void vk_usb_facade_release(vk_usb_facade_t *facade)
{
    if (!valid(facade)) return;
    facade->ops.lock(facade->ops.context);
    if (facade->leases == 0U) {
        facade->tainted = true;
    } else {
        --facade->leases;
    }
    facade->ops.unlock(facade->ops.context);
}

esp_err_t vk_usb_facade_close(vk_usb_facade_t *facade, uint32_t max_wait_ticks)
{
    if (!valid(facade)) return ESP_ERR_INVALID_ARG;
    facade->ops.lock(facade->ops.context);
    facade->open = false;
    facade->ops.unlock(facade->ops.context);

    for (uint32_t tick = 0U;; ++tick) {
        facade->ops.lock(facade->ops.context);
        if (facade->leases == 0U) {
            facade->service = NULL;
            bool tainted = facade->tainted;
            facade->ops.unlock(facade->ops.context);
            return tainted ? ESP_ERR_INVALID_STATE : ESP_OK;
        }
        if (tick >= max_wait_ticks) {
            facade->tainted = true;
            facade->ops.unlock(facade->ops.context);
            return ESP_ERR_TIMEOUT;
        }
        facade->ops.unlock(facade->ops.context);
        facade->ops.wait_tick(facade->ops.context);
    }
}

bool vk_usb_facade_is_tainted(vk_usb_facade_t *facade)
{
    if (!valid(facade)) return true;
    facade->ops.lock(facade->ops.context);
    bool result = facade->tainted;
    facade->ops.unlock(facade->ops.context);
    return result;
}
