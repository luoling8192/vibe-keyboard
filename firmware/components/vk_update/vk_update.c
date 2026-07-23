#include "vk_update.h"

#include <string.h>

static bool valid_sha256(const char *value)
{
    if (value == NULL || strlen(value) != 64U) return false;
    for (size_t i = 0; i < 64U; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static bool valid_identity(const vk_update_image_identity_t *identity, uint32_t size)
{
    return identity != NULL && identity->project[0] != '\0' && identity->version[0] != '\0' &&
           identity->chip_id == 9U && identity->min_revision <= identity->max_revision &&
           identity->image_size == size;
}

static vk_update_result_t validate_tuple(const vk_update_partition_tuple_t *tuple)
{
    if (tuple == NULL || !tuple->bootloader_migration_verified ||
        !tuple->rollback_pending_verify_verified || tuple->slot_size != VK_UPDATE_SLOT_SIZE) {
        return VK_UPDATE_UNAVAILABLE;
    }
    if (tuple->running_slot == VK_UPDATE_SLOT_OTA0 &&
        tuple->running_offset == VK_UPDATE_OTA0_OFFSET &&
        tuple->target_slot == VK_UPDATE_SLOT_OTA1 && tuple->target_offset == VK_UPDATE_OTA1_OFFSET) {
        return VK_UPDATE_OK;
    }
    if (tuple->running_slot == VK_UPDATE_SLOT_OTA1 &&
        tuple->running_offset == VK_UPDATE_OTA1_OFFSET &&
        tuple->target_slot == VK_UPDATE_SLOT_OTA0 && tuple->target_offset == VK_UPDATE_OTA0_OFFSET) {
        return VK_UPDATE_OK;
    }
    if ((tuple->running_slot == VK_UPDATE_SLOT_OTA0 && tuple->running_offset != VK_UPDATE_OTA0_OFFSET) ||
        (tuple->running_slot == VK_UPDATE_SLOT_OTA1 && tuple->running_offset != VK_UPDATE_OTA1_OFFSET)) {
        return VK_UPDATE_WRONG_RUNNING_SLOT;
    }
    return VK_UPDATE_WRONG_TARGET;
}

static bool same_tuple(const vk_update_partition_tuple_t *a, const vk_update_partition_tuple_t *b)
{
    return a->running_slot == b->running_slot && a->running_offset == b->running_offset &&
           a->target_slot == b->target_slot && a->target_offset == b->target_offset &&
           a->slot_size == b->slot_size &&
           a->bootloader_migration_verified == b->bootloader_migration_verified &&
           a->rollback_pending_verify_verified == b->rollback_pending_verify_verified;
}

static vk_update_result_t current_tuple(vk_update_t *update, vk_update_partition_tuple_t *tuple)
{
    if (update == NULL || tuple == NULL || update->backend.read_tuple == NULL) return VK_UPDATE_UNAVAILABLE;
    vk_update_result_t result = update->backend.read_tuple(update->backend.context, tuple);
    return result == VK_UPDATE_OK ? validate_tuple(tuple) : result;
}

static void invalidate_stage(vk_update_t *update)
{
    if (update->active && update->backend.cancel != NULL) update->backend.cancel(update->backend.context);
    memset(update, 0, offsetof(vk_update_t, backend));
}

static vk_update_result_t current_time(vk_update_t *update, uint64_t *now)
{
    if (update == NULL || now == NULL || update->backend.monotonic_ms == NULL) return VK_UPDATE_UNAVAILABLE;
    *now = update->backend.monotonic_ms(update->backend.context);
    return VK_UPDATE_OK;
}

static vk_update_result_t require_active(vk_update_t *update, uint32_t epoch, uint32_t transfer_id)
{
    if (update == NULL || epoch == 0U) return VK_UPDATE_INVALID_ARGUMENT;
    if (!update->active || update->epoch != epoch) return VK_UPDATE_WRONG_EPOCH;
    if (update->transfer_id != transfer_id) return VK_UPDATE_NOT_FOUND;
    uint64_t now = 0U;
    vk_update_result_t result = current_time(update, &now);
    if (result != VK_UPDATE_OK || now < update->last_activity_ms ||
        (!update->sealed && now - update->last_activity_ms >= VK_UPDATE_IDLE_TIMEOUT_MS)) {
        invalidate_stage(update);
        return result == VK_UPDATE_OK ? VK_UPDATE_TIMEOUT : result;
    }
    vk_update_partition_tuple_t current;
    result = current_tuple(update, &current);
    if (result != VK_UPDATE_OK || !same_tuple(&current, &update->tuple)) {
        invalidate_stage(update);
        return result == VK_UPDATE_OK ? VK_UPDATE_WRONG_TARGET : result;
    }
    return VK_UPDATE_OK;
}

static vk_update_result_t mark_successful_activity(vk_update_t *update)
{
    uint64_t now = 0U;
    vk_update_result_t result = current_time(update, &now);
    if (result != VK_UPDATE_OK || now < update->last_activity_ms) {
        invalidate_stage(update);
        return result == VK_UPDATE_OK ? VK_UPDATE_TIMEOUT : result;
    }
    update->last_activity_ms = now;
    return VK_UPDATE_OK;
}

void vk_update_init(vk_update_t *update, const vk_update_backend_t *backend)
{
    if (update == NULL) return;
    memset(update, 0, sizeof(*update));
    if (backend != NULL) update->backend = *backend;
}

void vk_update_invalidate(vk_update_t *update)
{
    if (update == NULL) return;
    invalidate_stage(update);
}

vk_update_result_t vk_update_capability(vk_update_t *update, vk_update_capability_t *capability)
{
    if (capability == NULL) return VK_UPDATE_INVALID_ARGUMENT;
    memset(capability, 0, sizeof(*capability));
    capability->unavailable_reason = "bootloader_migration_required";
    vk_update_partition_tuple_t tuple;
    vk_update_result_t result = current_tuple(update, &tuple);
    if (result != VK_UPDATE_OK) return result;
    capability->available = true;
    capability->unavailable_reason = NULL;
    capability->target = tuple.target_slot;
    capability->chunk_bytes = VK_UPDATE_CHUNK_MAX;
    capability->max_image_bytes = tuple.slot_size;
    return VK_UPDATE_OK;
}

vk_update_result_t vk_update_begin(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t size, const char *sha256)
{
    if (update == NULL || epoch == 0U || transfer_id == 0U || !valid_sha256(sha256)) return VK_UPDATE_INVALID_ARGUMENT;
    if (size == 0U || size > VK_UPDATE_SLOT_SIZE) return VK_UPDATE_BAD_SIZE;
    if (update->active) {
        if (update->epoch != epoch || update->transfer_id != transfer_id || update->size != size ||
            strcmp(update->sha256, sha256) != 0) return VK_UPDATE_CONFLICT;
        vk_update_result_t replay = require_active(update, epoch, transfer_id);
        if (replay != VK_UPDATE_OK) return replay;
        return mark_successful_activity(update);
    }
    vk_update_partition_tuple_t tuple;
    vk_update_result_t result = current_tuple(update, &tuple);
    if (result != VK_UPDATE_OK) return result;
    if (update->backend.begin == NULL) return VK_UPDATE_UNAVAILABLE;
    result = update->backend.begin(update->backend.context, &tuple, size);
    if (result != VK_UPDATE_OK) return result;
    update->active = true;
    update->epoch = epoch;
    update->transfer_id = transfer_id;
    update->size = size;
    memcpy(update->sha256, sha256, 65U);
    update->tuple = tuple;
    result = current_time(update, &update->last_activity_ms);
    if (result != VK_UPDATE_OK) {
        invalidate_stage(update);
        return result;
    }
    return VK_UPDATE_OK;
}

vk_update_result_t vk_update_write(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t offset, const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0U || length > VK_UPDATE_CHUNK_MAX) return VK_UPDATE_BAD_SIZE;
    vk_update_result_t result = require_active(update, epoch, transfer_id);
    if (result != VK_UPDATE_OK) return result;
    if (update->sealed || update->activating) return VK_UPDATE_CONFLICT;
    if (offset != update->next_offset) return VK_UPDATE_BAD_OFFSET;
    if (length > (size_t)(update->size - update->next_offset)) return VK_UPDATE_BAD_SIZE;
    if (update->backend.write == NULL) {
        invalidate_stage(update);
        return VK_UPDATE_UNAVAILABLE;
    }
    result = update->backend.write(update->backend.context, update->tuple.target_offset + offset, data, length);
    if (result != VK_UPDATE_OK) {
        invalidate_stage(update);
        return result;
    }
    update->next_offset += (uint32_t)length;
    result = mark_successful_activity(update);
    return result;
}

vk_update_result_t vk_update_seal(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t size, const char *sha256)
{
    vk_update_result_t result = require_active(update, epoch, transfer_id);
    if (result != VK_UPDATE_OK) return result;
    if (size != update->size) return VK_UPDATE_BAD_SIZE;
    if (!valid_sha256(sha256) || strcmp(update->sha256, sha256) != 0) return VK_UPDATE_BAD_HASH;
    if (update->sealed) return VK_UPDATE_OK;
    if (update->next_offset != update->size) return VK_UPDATE_INCOMPLETE;
    if (update->backend.seal == NULL || update->backend.readback == NULL) return VK_UPDATE_UNAVAILABLE;
    char sealed_digest[VK_UPDATE_SHA256_HEX_BYTES] = {0};
    vk_update_image_identity_t sealed_identity = {0};
    result = update->backend.seal(update->backend.context, size, sealed_digest, &sealed_identity);
    if (result != VK_UPDATE_OK) { invalidate_stage(update); return result; }
    if (strcmp(sealed_digest, update->sha256) != 0 || !valid_identity(&sealed_identity, size)) {
        invalidate_stage(update);
        return VK_UPDATE_IMAGE_INVALID;
    }
    char readback_digest[VK_UPDATE_SHA256_HEX_BYTES] = {0};
    vk_update_image_identity_t readback_identity = {0};
    result = update->backend.readback(update->backend.context, update->tuple.target_offset, size, readback_digest, &readback_identity);
    if (result != VK_UPDATE_OK || strcmp(readback_digest, sealed_digest) != 0 ||
        memcmp(&readback_identity, &sealed_identity, sizeof(sealed_identity)) != 0) {
        invalidate_stage(update);
        return VK_UPDATE_READBACK_MISMATCH;
    }
    update->identity = sealed_identity;
    update->sealed = true;
    return mark_successful_activity(update);
}

vk_update_result_t vk_update_query(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t *next_offset, bool *sealed)
{
    if (update == NULL || next_offset == NULL || sealed == NULL || !update->active) return VK_UPDATE_NOT_FOUND;
    vk_update_result_t result = require_active(update, epoch, transfer_id);
    if (result != VK_UPDATE_OK) return result;
    *next_offset = update->next_offset;
    *sealed = update->sealed;
    return mark_successful_activity(update);
}

vk_update_result_t vk_update_cancel(vk_update_t *update, uint32_t epoch, uint32_t transfer_id)
{
    vk_update_result_t result = require_active(update, epoch, transfer_id);
    if (result != VK_UPDATE_OK) return result;
    vk_update_invalidate(update);
    return VK_UPDATE_OK;
}

vk_update_result_t vk_update_activate(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, const char *sha256)
{
    vk_update_result_t result = require_active(update, epoch, transfer_id);
    if (result != VK_UPDATE_OK) return result;
    if (!update->sealed) return VK_UPDATE_NOT_SEALED;
    if (!valid_sha256(sha256) || strcmp(update->sha256, sha256) != 0) return VK_UPDATE_BAD_HASH;
    if (update->activating) return VK_UPDATE_OK;
    char digest[VK_UPDATE_SHA256_HEX_BYTES] = {0};
    vk_update_image_identity_t identity = {0};
    result = update->backend.readback(update->backend.context, update->tuple.target_offset, update->size, digest, &identity);
    if (result != VK_UPDATE_OK || strcmp(digest, update->sha256) != 0 ||
        memcmp(&identity, &update->identity, sizeof(identity)) != 0) {
        invalidate_stage(update);
        return VK_UPDATE_READBACK_MISMATCH;
    }
    if (update->backend.select == NULL) return VK_UPDATE_UNAVAILABLE;
    result = update->backend.select(update->backend.context, update->tuple.target_slot);
    if (result != VK_UPDATE_OK) {
        invalidate_stage(update);
        return VK_UPDATE_SELECTION_FAILED;
    }
    update->activating = true;
    return mark_successful_activity(update);
}
