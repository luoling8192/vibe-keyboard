#include "vk_asset_transfer.h"

#include <stdio.h>
#include <string.h>

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

#define ASSET_RESERVE_FLOOR 1U

static uint64_t now_ms(vk_asset_transfer_service_t *service)
{
    return service->config.now_ms == NULL ? 0U : service->config.now_ms(service->config.clock_context);
}

static uint64_t deadline(uint64_t now, uint32_t duration)
{
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static bool parse_sha(const char *text, uint8_t output[VK_ASSET_SHA256_BYTES])
{
    if (text == NULL || strlen(text) != VK_ASSET_SHA256_HEX_BYTES) return false;
    for (size_t index = 0; index < VK_ASSET_SHA256_BYTES; ++index) {
        unsigned values[2];
        for (size_t digit = 0; digit < 2U; ++digit) {
            char value = text[index * 2U + digit];
            values[digit] = value >= '0' && value <= '9' ? (unsigned)(value - '0') :
                            value >= 'a' && value <= 'f' ? (unsigned)(value - 'a' + 10) : 16U;
        }
        if (values[0] > 15U || values[1] > 15U) return false;
        output[index] = (uint8_t)((values[0] << 4U) | values[1]);
    }
    return true;
}

static void format_sha(const uint8_t input[VK_ASSET_SHA256_BYTES], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < VK_ASSET_SHA256_BYTES; ++index) {
        output[index * 2U] = digits[input[index] >> 4U];
        output[index * 2U + 1U] = digits[input[index] & 15U];
    }
    output[64] = '\0';
}

static vk_asset_kind_t store_kind(vk_usb_asset_kind_t kind)
{
    return kind == VK_USB_ASSET_KIND_IMAGE ? VK_ASSET_KIND_IMAGE :
           kind == VK_USB_ASSET_KIND_ANIMATION ? VK_ASSET_KIND_ANIMATION :
           VK_ASSET_KIND_GLYPH_BITMAP;
}

static vk_usb_asset_kind_t usb_kind(vk_asset_kind_t kind)
{
    return kind == VK_ASSET_KIND_IMAGE ? VK_USB_ASSET_KIND_IMAGE :
           kind == VK_ASSET_KIND_ANIMATION ? VK_USB_ASSET_KIND_ANIMATION :
           VK_USB_ASSET_KIND_GLYPH_BITMAP;
}

static const char *kind_name(vk_usb_asset_kind_t kind)
{
    return kind == VK_USB_ASSET_KIND_IMAGE ? "image" :
           kind == VK_USB_ASSET_KIND_ANIMATION ? "animation" : "glyph_bitmap";
}

static void invalidate_catalog(vk_asset_transfer_service_t *service)
{
    service->catalog_epoch = 0U;
    service->catalog_snapshot_id = 0U;
    service->catalog_revision = 0U;
    service->catalog_next_cursor = 0U;
    service->catalog_deadline_ms = 0U;
    service->catalog_count = 0U;
}

esp_err_t vk_asset_transfer_init(vk_asset_transfer_service_t *service,
                                 const vk_asset_transfer_config_t *config)
{
    if (service == NULL || config == NULL || config->store == NULL || config->chunk_bytes == 0U ||
        config->chunk_bytes > VK_USB_ASSET_CHUNK_MAX_BYTES || config->max_frames == 0U ||
        config->min_frame_ms == 0U || config->max_frame_ms < config->min_frame_ms ||
        config->max_active_decoded_bytes == 0U || config->decoder_scratch_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(service, 0, sizeof(*service));
    service->config = *config;
    return ESP_OK;
}

esp_err_t vk_asset_transfer_get_capabilities(void *context, uint32_t expected_epoch,
                                             vk_usb_capability_snapshot_t *snapshot)
{
    vk_asset_transfer_service_t *service = context;
    if (service == NULL || snapshot == NULL || expected_epoch == 0U) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->update.state = VK_USB_CAPABILITY_UNAVAILABLE;
    snapshot->update.unavailable_reason = VK_USB_UPDATE_REASON_BOOTLOADER_MIGRATION_REQUIRED;

    vk_asset_store_state_t state = vk_asset_store_state(service->config.store);
    if (!service->config.production_available) {
        snapshot->assets.state = VK_USB_CAPABILITY_UNAVAILABLE;
        snapshot->assets.unavailable_reason = VK_USB_ASSETS_REASON_DISPLAY_ACCEPTANCE_REQUIRED;
        return ESP_OK;
    }
    if (state == VK_ASSET_STORE_UNMOUNTED || state == VK_ASSET_STORE_MOUNT_FAILED) {
        snapshot->assets.state = VK_USB_CAPABILITY_UNAVAILABLE;
        snapshot->assets.unavailable_reason = VK_USB_ASSETS_REASON_STORAGE_UNAVAILABLE;
        return ESP_OK;
    }
    if (state == VK_ASSET_STORE_CORRUPT) {
        snapshot->assets.state = VK_USB_CAPABILITY_UNAVAILABLE;
        snapshot->assets.unavailable_reason = VK_USB_ASSETS_REASON_INTEGRITY_UNAVAILABLE;
        return ESP_OK;
    }

    uint32_t free_bytes = 0U, revision = 0U;
    bool transfer_active = false;
    if (vk_asset_store_status(service->config.store, &free_bytes, &revision, &transfer_active) != ESP_OK) {
        snapshot->assets.state = VK_USB_CAPABILITY_UNAVAILABLE;
        snapshot->assets.unavailable_reason = VK_USB_ASSETS_REASON_STORAGE_UNAVAILABLE;
        return ESP_OK;
    }
    vk_usb_assets_capability_t *assets = &snapshot->assets;
    assets->state = VK_USB_CAPABILITY_AVAILABLE;
    assets->storage_state = state == VK_ASSET_STORE_UNFORMATTED ? VK_USB_STORAGE_UNFORMATTED :
                            transfer_active ? VK_USB_STORAGE_BUSY : VK_USB_STORAGE_READY;
    assets->free_bytes = free_bytes;
    assets->reserve_bytes = service->config.store->config.reserve_bytes;
    uint32_t usable = free_bytes > assets->reserve_bytes ? free_bytes - assets->reserve_bytes : 0U;
    assets->upload_max_bytes = assets->storage_state == VK_USB_STORAGE_READY ? usable : 0U;
    if (assets->upload_max_bytes > service->config.store->config.max_asset_bytes) {
        assets->upload_max_bytes = service->config.store->config.max_asset_bytes;
    }
    assets->max_asset_bytes = service->config.store->config.max_asset_bytes;
    assets->chunk_bytes = service->config.chunk_bytes;
    assets->max_assets = service->config.store->config.max_assets;
    assets->max_frames = service->config.max_frames;
    assets->min_frame_ms = service->config.min_frame_ms;
    assets->max_frame_ms = service->config.max_frame_ms;
    assets->max_active_decoded_bytes = service->config.max_active_decoded_bytes;
    assets->decoder_scratch_bytes = service->config.decoder_scratch_bytes;
    assets->revision = revision;
    service->bound_epoch = expected_epoch;
    return ESP_OK;
}

static esp_err_t begin_or_resume(vk_asset_transfer_service_t *service,
                                 const vk_usb_asset_command_t *command)
{
    uint8_t digest[VK_ASSET_SHA256_BYTES];
    if (!parse_sha(command->sha256, digest)) return ESP_ERR_INVALID_ARG;
    vk_asset_transfer_t existing;
    esp_err_t result = vk_asset_store_resume(service->config.store, command->transfer_id, &existing);
    if (result == ESP_OK) {
        if (existing.total_bytes != command->total_bytes || existing.kind != store_kind(command->asset_kind_value) ||
            memcmp(existing.sha256, digest, sizeof(digest)) != 0) return ESP_ERR_INVALID_STATE;
    } else if (result == ESP_ERR_NOT_FOUND) {
        vk_asset_transfer_t transfer = {
            .transfer_id = command->transfer_id,
            .total_bytes = command->total_bytes,
            .next_offset = 0U,
            .kind = store_kind(command->asset_kind_value),
        };
        memcpy(transfer.sha256, digest, sizeof(digest));
        result = vk_asset_store_begin(service->config.store, &transfer);
    }
    if (result == ESP_OK) {
        service->bound_epoch = command->expected_epoch;
        service->bound_snapshot_generation = command->snapshot_generation;
        service->transfer_deadline_ms = deadline(now_ms(service), VK_ASSET_TRANSFER_IDLE_DEADLINE_MS);
        invalidate_catalog(service);
    }
    return result;
}

esp_err_t vk_asset_transfer_handle_command(void *context, const vk_usb_asset_command_t *command)
{
    vk_asset_transfer_service_t *service = context;
    if (service == NULL || command == NULL || command->expected_epoch == 0U ||
        command->snapshot_generation == 0U) return ESP_ERR_INVALID_ARG;
    if (command->kind == VK_USB_ASSET_BEGIN) return begin_or_resume(service, command);
    if (command->kind == VK_USB_ASSET_QUERY) {
        vk_asset_transfer_t transfer;
        esp_err_t result = vk_asset_store_resume(service->config.store, command->transfer_id, &transfer);
        if (result == ESP_OK) {
            service->bound_epoch = command->expected_epoch;
            service->bound_snapshot_generation = command->snapshot_generation;
            service->transfer_deadline_ms = deadline(now_ms(service), VK_ASSET_TRANSFER_IDLE_DEADLINE_MS);
        }
        return result;
    }
    if (command->kind == VK_USB_ASSET_END) {
        esp_err_t result = vk_asset_store_seal(service->config.store, command->transfer_id);
        if (result == ESP_OK) {
            service->transfer_deadline_ms = 0U;
            invalidate_catalog(service);
        }
        return result;
    }
    if (command->kind == VK_USB_ASSET_ABORT) {
        esp_err_t result = vk_asset_store_abort(service->config.store, command->transfer_id);
        if (result == ESP_ERR_NOT_FOUND) result = ESP_OK;
        if (result == ESP_OK) service->transfer_deadline_ms = 0U;
        return result;
    }
    if (command->kind == VK_USB_ASSET_DELETE) {
        uint8_t digest[VK_ASSET_SHA256_BYTES];
        if (!parse_sha(command->sha256, digest)) return ESP_ERR_INVALID_ARG;
        esp_err_t result = vk_asset_store_delete(service->config.store, digest, command->expected_revision);
        if (result == ESP_OK) invalidate_catalog(service);
        return result;
    }
    /* Page encoding is USB-owned; the immutable snapshot is retained here. */
    if (command->kind == VK_USB_ASSET_LIST) {
        uint64_t now = now_ms(service);
        bool start = command->snapshot_id == 0U && command->cursor == 0U;
        if (start) {
            vk_asset_catalog_entry_t entries[VK_ASSET_TRANSFER_MAX_CATALOG_ENTRIES];
            size_t count = 0U;uint32_t revision = 0U;
            esp_err_t result = vk_asset_store_catalog(service->config.store, entries,
                                                       VK_ASSET_TRANSFER_MAX_CATALOG_ENTRIES,
                                                       &count, &revision);
            if (result != ESP_OK) return result;
            for (size_t index = 0; index < count; ++index) {
                format_sha(entries[index].sha256, service->catalog[index].sha256);
                service->catalog[index].total_bytes = entries[index].total_bytes;
                service->catalog[index].kind = usb_kind(entries[index].kind);
                service->catalog[index].referenced = entries[index].referenced;
            }
            service->catalog_count = count;service->catalog_revision = revision;
            service->catalog_epoch = command->expected_epoch;
            service->catalog_snapshot_id = command->snapshot_generation ^ command->expected_epoch ^ revision ^ 0xa55a5aa5U;
            if (service->catalog_snapshot_id == 0U) service->catalog_snapshot_id = 1U;
            service->catalog_next_cursor = 0U;
        } else if (service->catalog_epoch != command->expected_epoch ||
                   service->catalog_snapshot_id != command->snapshot_id ||
                   command->cursor != service->catalog_next_cursor || now >= service->catalog_deadline_ms) {
            return ESP_ERR_INVALID_STATE;
        }
        size_t remaining = service->catalog_count - command->cursor;
        size_t page = remaining < command->limit ? remaining : command->limit;
        service->catalog_next_cursor = (uint32_t)(command->cursor + page);
        service->catalog_deadline_ms = deadline(now, VK_ASSET_TRANSFER_CATALOG_DEADLINE_MS);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t vk_asset_transfer_handle_chunk(void *context, const vk_usb_asset_chunk_t *chunk)
{
    vk_asset_transfer_service_t *service = context;
    if (service == NULL || chunk == NULL || chunk->expected_epoch == 0U ||
        chunk->snapshot_generation == 0U || chunk->payload_length == 0U) return ESP_ERR_INVALID_ARG;
    if (service->bound_epoch != chunk->expected_epoch ||
        service->bound_snapshot_generation != chunk->snapshot_generation ||
        now_ms(service) >= service->transfer_deadline_ms) return ESP_ERR_INVALID_STATE;
    uint32_t next_offset = 0U;
    esp_err_t result = vk_asset_store_append(service->config.store, chunk->transfer_id,
                                              chunk->offset, chunk->payload,
                                              chunk->payload_length, &next_offset);
    if (result == ESP_OK && next_offset != chunk->offset + chunk->payload_length) {
        return ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) service->transfer_deadline_ms = deadline(now_ms(service), VK_ASSET_TRANSFER_IDLE_DEADLINE_MS);
    return result;
}

esp_err_t vk_asset_transfer_get_state(void *context, uint32_t transfer_id,
                                      uint32_t expected_epoch, uint32_t snapshot_generation,
                                      vk_usb_asset_command_t *tuple, uint32_t *next_offset)
{
    vk_asset_transfer_service_t *service = context;
    if (service == NULL || tuple == NULL || next_offset == NULL || transfer_id == 0U ||
        expected_epoch == 0U || snapshot_generation == 0U) return ESP_ERR_INVALID_ARG;
    vk_asset_transfer_t transfer;
    esp_err_t result = vk_asset_store_resume(service->config.store, transfer_id, &transfer);
    if (result != ESP_OK) return result;
    memset(tuple, 0, sizeof(*tuple));
    tuple->kind = VK_USB_ASSET_BEGIN;
    tuple->expected_epoch = expected_epoch;
    tuple->snapshot_generation = snapshot_generation;
    tuple->transfer_id = transfer.transfer_id;
    tuple->total_bytes = transfer.total_bytes;
    tuple->asset_kind_value = usb_kind(transfer.kind);
    snprintf(tuple->asset_kind, sizeof(tuple->asset_kind), "%s", kind_name(tuple->asset_kind_value));
    format_sha(transfer.sha256, tuple->sha256);
    *next_offset = transfer.next_offset;
    service->bound_epoch = expected_epoch;
    service->bound_snapshot_generation = snapshot_generation;
    service->transfer_deadline_ms = deadline(now_ms(service), VK_ASSET_TRANSFER_IDLE_DEADLINE_MS);
    return ESP_OK;
}

esp_err_t vk_asset_transfer_catalog_page(vk_asset_transfer_service_t *service,
                                         uint32_t expected_epoch, uint32_t snapshot_id,
                                         uint32_t cursor, uint8_t limit,
                                         vk_usb_asset_event_t *event)
{
    if (service == NULL || event == NULL || expected_epoch == 0U || limit == 0U || limit > 64U) return ESP_ERR_INVALID_ARG;
    uint64_t now = now_ms(service);bool start = snapshot_id == 0U && cursor == 0U;
    if (start) {
        vk_asset_catalog_entry_t entries[VK_ASSET_TRANSFER_MAX_CATALOG_ENTRIES];size_t count = 0U;uint32_t revision = 0U;
        esp_err_t result = vk_asset_store_catalog(service->config.store, entries, VK_ASSET_TRANSFER_MAX_CATALOG_ENTRIES, &count, &revision);
        if (result != ESP_OK) return result;
        for (size_t index = 0; index < count; ++index) {
            format_sha(entries[index].sha256, service->catalog[index].sha256);
            service->catalog[index].total_bytes = entries[index].total_bytes;
            service->catalog[index].kind = usb_kind(entries[index].kind);
            service->catalog[index].referenced = entries[index].referenced;
        }
        service->catalog_count=count;service->catalog_revision=revision;service->catalog_epoch=expected_epoch;
        service->catalog_snapshot_id=expected_epoch^revision^0xa55a5aa5U;if(service->catalog_snapshot_id==0U)service->catalog_snapshot_id=1U;
        service->catalog_next_cursor=0U;snapshot_id=service->catalog_snapshot_id;
    } else if (service->catalog_epoch != expected_epoch || service->catalog_snapshot_id != snapshot_id ||
               cursor != service->catalog_next_cursor || now >= service->catalog_deadline_ms) return ESP_ERR_INVALID_STATE;
    size_t remaining=service->catalog_count-cursor,page=remaining<limit?remaining:limit;
    *event=(vk_usb_asset_event_t){.kind=VK_USB_ASSET_EVENT_PAGE,.snapshot_id=snapshot_id,.cursor=cursor,
        .revision=service->catalog_revision,.entry_count=page,.entries=service->catalog+cursor};
    service->catalog_next_cursor=(uint32_t)(cursor+page);event->has_next_cursor=service->catalog_next_cursor<service->catalog_count;
    event->next_cursor=event->has_next_cursor?service->catalog_next_cursor:0U;
    service->catalog_deadline_ms=deadline(now,VK_ASSET_TRANSFER_CATALOG_DEADLINE_MS);return ESP_OK;
}

static esp_err_t build_event(void *context, const vk_usb_asset_command_t *command, vk_usb_asset_event_t *event)
{
    vk_asset_transfer_service_t *service=context;if(!service||!command||!event)return ESP_ERR_INVALID_ARG;
    if(command->kind==VK_USB_ASSET_LIST)return vk_asset_transfer_catalog_page(service,command->expected_epoch,command->snapshot_id,command->cursor,command->limit,event);
    if(command->kind==VK_USB_ASSET_DELETE){*event=(vk_usb_asset_event_t){.kind=VK_USB_ASSET_EVENT_DELETED,.revision=service->config.store->selected_revision};snprintf(event->sha256,sizeof(event->sha256),"%s",command->sha256);return ESP_OK;}
    return ESP_ERR_NOT_SUPPORTED;
}

void vk_asset_transfer_registration(vk_asset_transfer_service_t *service,
                                    vk_usb_capability_provider_registration_t *capabilities,
                                    vk_usb_asset_handler_registration_t *assets)
{
    if (capabilities != NULL) {
        *capabilities = (vk_usb_capability_provider_registration_t){
            .get_snapshot = vk_asset_transfer_get_capabilities,
            .context = service,
        };
    }
    if (assets != NULL) {
        *assets = (vk_usb_asset_handler_registration_t){
            .handle_command = vk_asset_transfer_handle_command,
            .handle_chunk = vk_asset_transfer_handle_chunk,
            .context = service,
            .chunk_bytes = service == NULL ? 0U : service->config.chunk_bytes,
            .max_asset_bytes = service == NULL ? 0U : service->config.store->config.max_asset_bytes,
            .get_transfer_state = vk_asset_transfer_get_state,
            .build_event = build_event,
        };
    }
}

void vk_asset_transfer_close_epoch(vk_asset_transfer_service_t *service, uint32_t epoch)
{
    if (service == NULL || epoch == 0U || service->bound_epoch != epoch) return;
    service->bound_epoch = 0U;
    service->bound_snapshot_generation = 0U;
    service->transfer_deadline_ms = 0U;
    invalidate_catalog(service);
}
