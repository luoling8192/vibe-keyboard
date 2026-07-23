#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_ASSET_SHA256_BYTES 32U
#define VK_ASSET_SHA256_HEX_BYTES 64U
#define VK_ASSET_PATH_BYTES 96U
#define VK_ASSET_COMMIT_BYTES 112U
#define VK_ASSET_MAX_COMMITS 64U
#define VK_ASSET_SPIFFS_NAME_MIN_BYTES 78U
#define VK_ASSET_FORMAT_CONFIRMATION "verified_erased_spiffs"

typedef enum {
    VK_ASSET_STORE_UNMOUNTED = 0,
    VK_ASSET_STORE_UNFORMATTED,
    VK_ASSET_STORE_READY,
    VK_ASSET_STORE_CORRUPT,
    VK_ASSET_STORE_MOUNT_FAILED,
} vk_asset_store_state_t;

typedef enum {
    VK_ASSET_KIND_IMAGE = 0,
    VK_ASSET_KIND_ANIMATION,
    VK_ASSET_KIND_GLYPH_BITMAP,
} vk_asset_kind_t;

typedef struct {
    uint32_t epoch;
    uint64_t nonce;
    uint32_t partition_offset;
    uint32_t partition_size;
    uint8_t erased_sha256[VK_ASSET_SHA256_BYTES];
} vk_asset_format_token_t;

typedef struct {
    uint32_t transfer_id;
    uint32_t total_bytes;
    uint32_t next_offset;
    vk_asset_kind_t kind;
    uint8_t sha256[VK_ASSET_SHA256_BYTES];
} vk_asset_transfer_t;

typedef struct {
    uint32_t revision;
    uint32_t previous_revision;
    const uint8_t *assets_manifest;
    size_t assets_manifest_bytes;
    const uint8_t *screen_manifest;
    size_t screen_manifest_bytes;
} vk_asset_revision_t;

typedef struct {
    uint32_t current_revision;
    uint32_t previous_revision;
    bool has_current;
    bool has_previous;
} vk_asset_recovery_t;

typedef struct {
    esp_err_t (*mount)(void *context, bool format_if_mount_failed);
    esp_err_t (*unmount)(void *context);
    esp_err_t (*format)(void *context);
    esp_err_t (*partition_is_all_ff)(void *context, uint32_t offset, uint32_t size,
                                     uint8_t sha256[VK_ASSET_SHA256_BYTES], bool *all_ff);
    esp_err_t (*read_file)(void *context, const char *name, size_t offset,
                           uint8_t *bytes, size_t capacity, size_t *read_bytes);
    esp_err_t (*file_size)(void *context, const char *name, size_t *size);
    esp_err_t (*write_new_file)(void *context, const char *name, const uint8_t *bytes,
                                size_t count, bool sync);
    esp_err_t (*rewrite_file)(void *context, const char *name, const uint8_t *bytes,
                              size_t count, bool sync);
    esp_err_t (*append_file)(void *context, const char *name, size_t exact_offset,
                             const uint8_t *bytes, size_t count, bool sync);
    /* Restores a part file to the last durable sidecar offset. Implementations must
     * persist the shortened length before returning success. */
    esp_err_t (*truncate_file)(void *context, const char *name, size_t exact_size,
                               bool sync);
    esp_err_t (*remove_file)(void *context, const char *name);
    esp_err_t (*list_files)(void *context, const char *prefix,
                            char names[][VK_ASSET_PATH_BYTES], size_t capacity, size_t *count);
    esp_err_t (*free_bytes)(void *context, uint32_t *free_bytes);
} vk_asset_fs_ops_t;

typedef esp_err_t (*vk_asset_vka1_validator_t)(void *context, const char *name,
                                                const uint8_t expected_hash[VK_ASSET_SHA256_BYTES],
                                                uint32_t exact_bytes, vk_asset_kind_t kind);
typedef esp_err_t (*vk_asset_revision_validator_t)(void *context, uint32_t revision,
                                                    uint32_t previous_revision,
                                                    const uint8_t *assets_manifest,
                                                    size_t assets_bytes,
                                                    const uint8_t *screen_manifest,
                                                    size_t screen_bytes);

typedef struct {
    const vk_asset_fs_ops_t *fs;
    void *fs_context;
    vk_asset_vka1_validator_t validate_vka1;
    void *vka1_context;
    vk_asset_revision_validator_t validate_revision;
    void *revision_context;
    uint32_t partition_offset;
    uint32_t partition_size;
    uint32_t reserve_bytes;
    uint32_t max_asset_bytes;
    uint16_t max_assets;
} vk_asset_store_config_t;

typedef struct {
    vk_asset_store_config_t config;
    atomic_flag admission;
    vk_asset_store_state_t state;
    vk_asset_transfer_t transfer;
    bool transfer_active;
    uint32_t selected_revision;
    uint32_t previous_revision;
    bool mounted;
    bool format_token_valid;
    vk_asset_format_token_t format_token;
} vk_asset_store_t;

esp_err_t vk_asset_store_init(vk_asset_store_t *store, const vk_asset_store_config_t *config);
esp_err_t vk_asset_store_mount(vk_asset_store_t *store);
esp_err_t vk_asset_store_unmount(vk_asset_store_t *store);
vk_asset_store_state_t vk_asset_store_state(const vk_asset_store_t *store);

esp_err_t vk_asset_store_authorize_format(vk_asset_store_t *store, uint32_t epoch, uint64_t nonce,
                                           vk_asset_format_token_t *token);
esp_err_t vk_asset_store_format(vk_asset_store_t *store, uint32_t epoch,
                                 const vk_asset_format_token_t *token, const char *confirmation);

esp_err_t vk_asset_store_begin(vk_asset_store_t *store, const vk_asset_transfer_t *transfer);
esp_err_t vk_asset_store_resume(vk_asset_store_t *store, uint32_t transfer_id,
                                 vk_asset_transfer_t *transfer);
esp_err_t vk_asset_store_append(vk_asset_store_t *store, uint32_t transfer_id,
                                 uint32_t exact_offset, const uint8_t *bytes, size_t count,
                                 uint32_t *next_offset);
esp_err_t vk_asset_store_seal(vk_asset_store_t *store, uint32_t transfer_id);
esp_err_t vk_asset_store_abort(vk_asset_store_t *store, uint32_t transfer_id);

esp_err_t vk_asset_store_publish_revision(vk_asset_store_t *store,
                                           const vk_asset_revision_t *revision,
                                           uint8_t screen_manifest_sha256[VK_ASSET_SHA256_BYTES],
                                           uint8_t assets_manifest_sha256[VK_ASSET_SHA256_BYTES]);
esp_err_t vk_asset_store_recover(vk_asset_store_t *store, vk_asset_recovery_t *recovery);
/* Loads one already validated immutable revision. Buffers are caller-owned and bounded. */
esp_err_t vk_asset_store_load_revision(vk_asset_store_t *store, uint32_t revision,
                                        uint32_t *previous_revision,
                                        uint8_t *assets_manifest, size_t assets_capacity,
                                        size_t *assets_bytes,
                                        uint8_t *screen_manifest, size_t screen_capacity,
                                        size_t *screen_bytes);
esp_err_t vk_asset_store_collect(vk_asset_store_t *store);

typedef struct {
    uint8_t sha256[VK_ASSET_SHA256_BYTES];
    uint32_t total_bytes;
    vk_asset_kind_t kind;
    bool referenced;
} vk_asset_catalog_entry_t;

esp_err_t vk_asset_store_status(vk_asset_store_t *store, uint32_t *free_bytes,
                                uint32_t *revision, bool *transfer_active);
esp_err_t vk_asset_store_transfer(vk_asset_store_t *store, uint32_t transfer_id,
                                  vk_asset_transfer_t *transfer);
esp_err_t vk_asset_store_catalog(vk_asset_store_t *store, vk_asset_catalog_entry_t *entries,
                                 size_t capacity, size_t *count, uint32_t *revision);
esp_err_t vk_asset_store_delete(vk_asset_store_t *store,
                                const uint8_t sha256[VK_ASSET_SHA256_BYTES],
                                uint32_t expected_revision);

bool vk_asset_revision_is_newer(uint32_t candidate, uint32_t current);
/* VKA1 files use their canonical identifier (bytes 24...55 zeroed); other files use raw SHA-256. */
esp_err_t vk_asset_sha256_file(const vk_asset_store_t *store, const char *name,
                               uint8_t digest[VK_ASSET_SHA256_BYTES]);
void vk_asset_sha256(const uint8_t *bytes, size_t count, uint8_t digest[VK_ASSET_SHA256_BYTES]);

#ifdef ESP_PLATFORM
esp_err_t vk_asset_spiffs_make_ops(vk_asset_fs_ops_t *ops);
#endif

#ifdef __cplusplus
}
#endif
