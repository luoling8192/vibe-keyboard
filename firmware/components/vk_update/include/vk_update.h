#ifndef VK_UPDATE_H
#define VK_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VK_UPDATE_SLOT_SIZE 0x500000U
#define VK_UPDATE_OTA0_OFFSET 0x020000U
#define VK_UPDATE_OTA1_OFFSET 0x520000U
#define VK_UPDATE_CHUNK_MAX 512U
#define VK_UPDATE_SHA256_HEX_BYTES 65U
#define VK_UPDATE_IDENTITY_BYTES 33U
#define VK_UPDATE_IDLE_TIMEOUT_MS 30000ULL

typedef enum {
    VK_UPDATE_OK = 0,
    VK_UPDATE_INVALID_ARGUMENT,
    VK_UPDATE_UNAVAILABLE,
    VK_UPDATE_WRONG_EPOCH,
    VK_UPDATE_WRONG_RUNNING_SLOT,
    VK_UPDATE_WRONG_TARGET,
    VK_UPDATE_BUSY,
    VK_UPDATE_CONFLICT,
    VK_UPDATE_NOT_FOUND,
    VK_UPDATE_BAD_SIZE,
    VK_UPDATE_BAD_HASH,
    VK_UPDATE_BAD_OFFSET,
    VK_UPDATE_WRITE_FAILED,
    VK_UPDATE_INCOMPLETE,
    VK_UPDATE_IMAGE_INVALID,
    VK_UPDATE_READBACK_MISMATCH,
    VK_UPDATE_NOT_SEALED,
    VK_UPDATE_SELECTION_FAILED,
    VK_UPDATE_ALREADY_ACTIVATING,
    VK_UPDATE_TIMEOUT,
} vk_update_result_t;

typedef enum {
    VK_UPDATE_SLOT_OTA0 = 0,
    VK_UPDATE_SLOT_OTA1 = 1,
} vk_update_slot_t;

typedef struct {
    vk_update_slot_t running_slot;
    uint32_t running_offset;
    vk_update_slot_t target_slot;
    uint32_t target_offset;
    uint32_t slot_size;
    bool bootloader_migration_verified;
    bool rollback_pending_verify_verified;
} vk_update_partition_tuple_t;

typedef struct {
    char project[VK_UPDATE_IDENTITY_BYTES];
    char version[VK_UPDATE_IDENTITY_BYTES];
    uint16_t chip_id;
    uint16_t min_revision;
    uint16_t max_revision;
    uint32_t image_size;
} vk_update_image_identity_t;

typedef struct {
    void *context;
    vk_update_result_t (*read_tuple)(void *context, vk_update_partition_tuple_t *tuple);
    vk_update_result_t (*begin)(void *context, const vk_update_partition_tuple_t *tuple, uint32_t size);
    vk_update_result_t (*write)(void *context, uint32_t target_offset, const uint8_t *data, size_t length);
    vk_update_result_t (*seal)(void *context, uint32_t size, char digest[VK_UPDATE_SHA256_HEX_BYTES], vk_update_image_identity_t *identity);
    vk_update_result_t (*readback)(void *context, uint32_t target_offset, uint32_t size, char digest[VK_UPDATE_SHA256_HEX_BYTES], vk_update_image_identity_t *identity);
    vk_update_result_t (*select)(void *context, vk_update_slot_t target_slot);
    void (*cancel)(void *context);
    uint64_t (*monotonic_ms)(void *context);
} vk_update_backend_t;

typedef struct {
    bool active;
    bool sealed;
    bool activating;
    uint32_t epoch;
    uint32_t transfer_id;
    uint32_t size;
    uint32_t next_offset;
    uint64_t last_activity_ms;
    char sha256[VK_UPDATE_SHA256_HEX_BYTES];
    vk_update_partition_tuple_t tuple;
    vk_update_image_identity_t identity;
    vk_update_backend_t backend;
} vk_update_t;

typedef struct {
    bool available;
    const char *unavailable_reason;
    vk_update_slot_t target;
    uint16_t chunk_bytes;
    uint32_t max_image_bytes;
} vk_update_capability_t;

void vk_update_init(vk_update_t *update, const vk_update_backend_t *backend);
void vk_update_invalidate(vk_update_t *update);
vk_update_result_t vk_update_capability(vk_update_t *update, vk_update_capability_t *capability);
vk_update_result_t vk_update_begin(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t size, const char *sha256);
vk_update_result_t vk_update_write(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t offset, const uint8_t *data, size_t length);
vk_update_result_t vk_update_seal(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t size, const char *sha256);
vk_update_result_t vk_update_query(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, uint32_t *next_offset, bool *sealed);
vk_update_result_t vk_update_cancel(vk_update_t *update, uint32_t epoch, uint32_t transfer_id);
vk_update_result_t vk_update_activate(vk_update_t *update, uint32_t epoch, uint32_t transfer_id, const char *sha256);

#endif
