#include "vk_usb_owner.h"

#include <stddef.h>
#include <string.h>

void vk_usb_owner_init(vk_usb_owner_t *owner, const vk_usb_owner_ops_t *ops)
{
    if (owner == NULL || ops == NULL) return;
    memset(owner, 0, sizeof(*owner));
    owner->ops = *ops;
    owner->quiescent = true;
}

void vk_usb_owner_attach(vk_usb_owner_t *owner, void *handle)
{
    if (owner == NULL || handle == NULL) return;
    owner->ops.lock(owner->ops.context);
    owner->handle = handle;
    owner->alive = true;
    owner->quiescent = false;
    owner->ops.unlock(owner->ops.context);
}

void vk_usb_owner_quiesce(vk_usb_owner_t *owner, void *handle)
{
    if (owner == NULL) return;
    owner->ops.lock(owner->ops.context);
    if (owner->alive && owner->handle == handle) {
        owner->handle = NULL;
        owner->alive = false;
        owner->quiescent = true;
    }
    owner->ops.unlock(owner->ops.context);
}

bool vk_usb_owner_request_stop(vk_usb_owner_t *owner)
{
    if (owner == NULL) return false;
    owner->ops.lock(owner->ops.context);
    if (!owner->alive || owner->quiescent || owner->handle == NULL) {
        owner->ops.unlock(owner->ops.context);
        return false;
    }
    /* The lock pins the handle until notify returns; quiesce cannot clear/delete it. */
    owner->ops.notify(owner->ops.context, owner->handle);
    owner->ops.unlock(owner->ops.context);
    return true;
}

bool vk_usb_owner_is_quiescent(vk_usb_owner_t *owner)
{
    if (owner == NULL) return true;
    owner->ops.lock(owner->ops.context);
    bool result = owner->quiescent && !owner->alive && owner->handle == NULL;
    owner->ops.unlock(owner->ops.context);
    return result;
}
