#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vk_usb_owner_lock_fn)(void *context);
typedef void (*vk_usb_owner_notify_fn)(void *context, void *handle);

typedef struct {
    vk_usb_owner_lock_fn lock;
    vk_usb_owner_lock_fn unlock;
    vk_usb_owner_notify_fn notify;
    void *context;
} vk_usb_owner_ops_t;

typedef struct {
    vk_usb_owner_ops_t ops;
    void *handle;
    bool alive;
    bool quiescent;
} vk_usb_owner_t;

void vk_usb_owner_init(vk_usb_owner_t *owner, const vk_usb_owner_ops_t *ops);
void vk_usb_owner_attach(vk_usb_owner_t *owner, void *handle);
void vk_usb_owner_quiesce(vk_usb_owner_t *owner, void *handle);
bool vk_usb_owner_request_stop(vk_usb_owner_t *owner);
bool vk_usb_owner_is_quiescent(vk_usb_owner_t *owner);

#ifdef __cplusplus
}
#endif
