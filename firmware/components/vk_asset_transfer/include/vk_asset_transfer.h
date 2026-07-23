#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_asset_store.h"
#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_ASSET_TRANSFER_CATALOG_DEADLINE_MS 30000U
#define VK_ASSET_TRANSFER_IDLE_DEADLINE_MS 30000U
#define VK_ASSET_TRANSFER_MAX_CATALOG_ENTRIES VK_USB_ASSET_PAGE_MAX_ENTRIES

typedef uint64_t (*vk_asset_transfer_now_ms_t)(void *context);

typedef struct {
    vk_asset_store_t *store;
    vk_asset_transfer_now_ms_t now_ms;
    void *clock_context;
    uint16_t chunk_bytes;
    uint16_t max_frames;
    uint16_t min_frame_ms;
    uint16_t max_frame_ms;
    uint32_t max_active_decoded_bytes;
    uint32_t decoder_scratch_bytes;
    bool production_available;
} vk_asset_transfer_config_t;

typedef struct {
    vk_asset_transfer_config_t config;
    uint32_t bound_epoch;
    uint32_t bound_snapshot_generation;
    uint64_t transfer_deadline_ms;
    uint32_t catalog_epoch;
    uint32_t catalog_snapshot_id;
    uint32_t catalog_revision;
    uint32_t catalog_next_cursor;
    uint64_t catalog_deadline_ms;
    size_t catalog_count;
    char last_error_detail[80];
    vk_usb_asset_list_entry_t catalog[VK_ASSET_TRANSFER_MAX_CATALOG_ENTRIES];
} vk_asset_transfer_service_t;

esp_err_t vk_asset_transfer_init(vk_asset_transfer_service_t *service,
                                 const vk_asset_transfer_config_t *config);
esp_err_t vk_asset_transfer_get_capabilities(void *context, uint32_t expected_epoch,
                                             vk_usb_capability_snapshot_t *snapshot);
esp_err_t vk_asset_transfer_handle_command(void *context,
                                           const vk_usb_asset_command_t *command);
esp_err_t vk_asset_transfer_handle_chunk(void *context,
                                         const vk_usb_asset_chunk_t *chunk);
esp_err_t vk_asset_transfer_get_state(void *context, uint32_t transfer_id,
                                      uint32_t expected_epoch,
                                      uint32_t snapshot_generation,
                                      vk_usb_asset_command_t *tuple,
                                      uint32_t *next_offset);
void vk_asset_transfer_registration(vk_asset_transfer_service_t *service,
                                    vk_usb_capability_provider_registration_t *capabilities,
                                    vk_usb_asset_handler_registration_t *assets);
esp_err_t vk_asset_transfer_catalog_page(vk_asset_transfer_service_t *service,
                                         uint32_t expected_epoch, uint32_t snapshot_id,
                                         uint32_t cursor, uint8_t limit,
                                         vk_usb_asset_event_t *event);
void vk_asset_transfer_close_epoch(vk_asset_transfer_service_t *service, uint32_t epoch);

#ifdef __cplusplus
}
#endif
