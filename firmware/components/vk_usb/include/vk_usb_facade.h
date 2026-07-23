#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*lock)(void *context);
    void (*unlock)(void *context);
    void (*wait_tick)(void *context);
    void *context;
} vk_usb_facade_ops_t;

typedef struct {
    vk_usb_facade_ops_t ops;
    void *service;
    uint32_t leases;
    bool open;
    bool tainted;
} vk_usb_facade_t;

void vk_usb_facade_init(vk_usb_facade_t *facade, const vk_usb_facade_ops_t *ops);
esp_err_t vk_usb_facade_publish(vk_usb_facade_t *facade, void *service);
esp_err_t vk_usb_facade_acquire(vk_usb_facade_t *facade, void **service);
void vk_usb_facade_release(vk_usb_facade_t *facade);
esp_err_t vk_usb_facade_close(vk_usb_facade_t *facade, uint32_t max_wait_ticks);
bool vk_usb_facade_is_tainted(vk_usb_facade_t *facade);

#ifdef __cplusplus
}
#endif
