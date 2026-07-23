#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_asset_store.h"
#include "vk_asset_transfer.h"
#include "vk_display.h"
#include "vk_screen.h"
#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_SCREEN_SERVICE_STORAGE_OFFSET 0x00a20000U
#define VK_SCREEN_SERVICE_STORAGE_SIZE 0x005e0000U
#define VK_SCREEN_SERVICE_RESERVE_BYTES 1U
#define VK_SCREEN_SERVICE_MAX_ASSET_BYTES 1048576U
#define VK_SCREEN_SERVICE_MAX_ASSETS 64U
#define VK_SCREEN_SERVICE_CHUNK_BYTES 4084U
#define VK_SCREEN_SERVICE_MAX_FRAMES 64U
#define VK_SCREEN_SERVICE_MIN_FRAME_MS 10U
#define VK_SCREEN_SERVICE_MAX_FRAME_MS 60000U
#define VK_SCREEN_SERVICE_DECODER_SCRATCH_BYTES 4096U
/* Two full RGB565 roots coexist during replacement, plus decoder scratch. */
#define VK_SCREEN_SERVICE_DECODE_BUDGET_BYTES 247200U

/* The product build admits the connected 428x142 Vibe Board display profile. */
#define VK_SCREEN_SERVICE_PHYSICAL_ACCEPTANCE_ADMITTED 1

typedef struct {
    vk_asset_store_t *store;
    vk_asset_transfer_service_t *transfer;
    vk_screen_t *screen;
    vk_display_t *display;
    bool store_recovered;
    bool font_profile_admitted;
    bool physical_acceptance_admitted;
    /* Fixed recovery workspace lives with the service, never on a small task stack. */
    uint8_t restore_assets_manifest[VK_USB_MAX_JSON_BYTES];
    uint8_t restore_screen_manifest[VK_USB_MAX_JSON_BYTES];
    /* Canonical persisted records are firmware-owned and never alias host JSON. */
    uint8_t publish_assets_manifest[VK_USB_MAX_JSON_BYTES];
    uint8_t publish_screen_manifest[VK_USB_MAX_JSON_BYTES];
} vk_screen_service_t;

typedef struct {
    bool mounted;
    bool recovered;
    bool transfer_ready;
    bool screen_ready;
    bool display_ready;
    bool font_ready;
    bool physical_acceptance_admitted;
} vk_screen_service_readiness_t;

esp_err_t vk_screen_service_init(vk_screen_service_t *service,
                                 vk_asset_store_t *store,
                                 vk_asset_transfer_service_t *transfer,
                                 vk_screen_t *screen,
                                 vk_display_t *display,
                                 const vk_screen_service_readiness_t *readiness);
esp_err_t vk_screen_service_get_capabilities(void *context, uint32_t expected_epoch,
                                             vk_usb_capability_snapshot_t *snapshot);
esp_err_t vk_screen_service_handle_screen(void *context,
                                          const vk_usb_screen_command_t *command,
                                          vk_usb_screen_event_t *event);
esp_err_t vk_screen_service_handle_widget(void *context,
                                          const vk_usb_widget_command_t *command,
                                          vk_usb_widget_event_t *event);
/* Restores current, then previous, from immutable durable manifests. The service
 * remains unavailable if neither revision can build a complete render root. */
esp_err_t vk_screen_service_restore(vk_screen_service_t *service,
                                    const vk_asset_recovery_t *recovery,
                                    vk_usb_json_document_t *document,
                                    uint8_t *envelope, size_t envelope_capacity);
void vk_screen_service_registrations(vk_screen_service_t *service,
                                     vk_usb_capability_provider_registration_t *capabilities,
                                     vk_usb_asset_handler_registration_t *assets,
                                     vk_usb_screen_handler_registration_t *screen,
                                     vk_usb_widget_handler_registration_t *widget);
void vk_screen_service_close_epoch(vk_screen_service_t *service, uint32_t epoch);

#ifdef ESP_PLATFORM
/* An erased storage partition is formatted after two exact all-FF checks.
 * Existing or corrupt data is never formatted automatically. */
esp_err_t vk_screen_product_prepare(void);
void vk_screen_product_registrations(vk_usb_capability_provider_registration_t *capabilities,
                                     vk_usb_asset_handler_registration_t *assets,
                                     vk_usb_screen_handler_registration_t *screen,
                                     vk_usb_widget_handler_registration_t *widget);
esp_err_t vk_screen_product_stop(void);
#endif

#ifdef __cplusplus
}
#endif
