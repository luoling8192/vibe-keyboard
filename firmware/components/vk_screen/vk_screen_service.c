#include "vk_screen_service.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

typedef struct { uint8_t *bytes; size_t capacity; size_t used; } manifest_builder_t;

static bool manifest_append(manifest_builder_t *builder, const char *format, ...)
{
    if (builder == NULL || builder->used >= builder->capacity) return false;
    va_list arguments; va_start(arguments, format);
    int count = vsnprintf((char *)builder->bytes + builder->used,
                          builder->capacity - builder->used, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= builder->capacity - builder->used) return false;
    builder->used += (size_t)count; return true;
}

static bool manifest_string(manifest_builder_t *builder, const char *value)
{
    if (!manifest_append(builder, "\"")) return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != 0U; ++cursor) {
        switch (*cursor) {
        case '\"': if (!manifest_append(builder, "\\\"")) return false; break;
        case '\\': if (!manifest_append(builder, "\\\\")) return false; break;
        case '\b': if (!manifest_append(builder, "\\b")) return false; break;
        case '\f': if (!manifest_append(builder, "\\f")) return false; break;
        case '\n': if (!manifest_append(builder, "\\n")) return false; break;
        case '\r': if (!manifest_append(builder, "\\r")) return false; break;
        case '\t': if (!manifest_append(builder, "\\t")) return false; break;
        default:
            if (*cursor < 0x20U) { if (!manifest_append(builder, "\\u%04x", *cursor)) return false; }
            else if (builder->used == builder->capacity) return false;
            else builder->bytes[builder->used++] = *cursor;
        }
    }
    return manifest_append(builder, "\"");
}

static bool manifest_node(manifest_builder_t *builder, const vk_usb_json_document_t *document,
                          uint16_t node, unsigned depth);

static bool manifest_object(manifest_builder_t *builder, const vk_usb_json_document_t *document,
                            uint16_t object, unsigned depth)
{
    if (!manifest_append(builder, "{")) return false;
    char previous[VK_USB_JSON_MAX_STRING_BYTES + 1U] = {0}; bool have_previous = false, first = true;
    size_t members = 0U;
    for (uint16_t key = vk_usb_json_first_child(document, object); key != VK_USB_JSON_NO_NODE;) {
        uint16_t value = vk_usb_json_next_sibling(document, key);
        if (!vk_usb_json_node_is_object_key(document, key) || value == VK_USB_JSON_NO_NODE) return false;
        ++members; key = vk_usb_json_next_sibling(document, value);
    }
    for (size_t emitted = 0U; emitted < members; ++emitted) {
        uint16_t selected_key = VK_USB_JSON_NO_NODE, selected_value = VK_USB_JSON_NO_NODE;
        char selected[VK_USB_JSON_MAX_STRING_BYTES + 1U] = {0};
        for (uint16_t key = vk_usb_json_first_child(document, object); key != VK_USB_JSON_NO_NODE;) {
            uint16_t value = vk_usb_json_next_sibling(document, key); char decoded[sizeof(selected)];
            if (!vk_usb_json_node_is_object_key(document, key) || value == VK_USB_JSON_NO_NODE ||
                vk_usb_json_string_copy(document, key, decoded, sizeof(decoded)) != VK_USB_JSON_OK) return false;
            if ((!have_previous || strcmp(decoded, previous) > 0) &&
                (selected_key == VK_USB_JSON_NO_NODE || strcmp(decoded, selected) < 0)) {
                selected_key = key; selected_value = value; snprintf(selected, sizeof(selected), "%s", decoded);
            }
            key = vk_usb_json_next_sibling(document, value);
        }
        if (selected_key == VK_USB_JSON_NO_NODE || (!first && !manifest_append(builder, ",")) ||
            !manifest_string(builder, selected) || !manifest_append(builder, ":") ||
            !manifest_node(builder, document, selected_value, depth + 1U)) return false;
        snprintf(previous, sizeof(previous), "%s", selected); have_previous = true; first = false;
    }
    return manifest_append(builder, "}");
}

static bool manifest_node(manifest_builder_t *builder, const vk_usb_json_document_t *document,
                          uint16_t node, unsigned depth)
{
    if (builder == NULL || document == NULL || node == VK_USB_JSON_NO_NODE || depth > VK_USB_JSON_MAX_DEPTH) return false;
    vk_usb_json_kind_t kind = vk_usb_json_kind(document, node);
    if (kind == VK_USB_JSON_OBJECT) return manifest_object(builder, document, node, depth);
    if (kind == VK_USB_JSON_ARRAY) {
        if (!manifest_append(builder, "[")) return false;
        bool first = true;
        for (uint16_t child = vk_usb_json_first_child(document, node); child != VK_USB_JSON_NO_NODE;
             child = vk_usb_json_next_sibling(document, child)) {
            if ((!first && !manifest_append(builder, ",")) || !manifest_node(builder, document, child, depth + 1U)) return false;
            first = false;
        }
        return manifest_append(builder, "]");
    }
    if (kind == VK_USB_JSON_STRING) {
        char decoded[VK_USB_JSON_MAX_STRING_BYTES + 1U];
        return vk_usb_json_string_copy(document, node, decoded, sizeof(decoded)) == VK_USB_JSON_OK && manifest_string(builder, decoded);
    }
    size_t start = 0U, end = 0U;
    if (!vk_usb_json_node_range(document, node, &start, &end) || end <= start || end > document->length ||
        end - start >= builder->capacity - builder->used) return false;
    memcpy(builder->bytes + builder->used, document->bytes + start, end - start); builder->used += end - start; return true;
}

static esp_err_t publish_screen_revision(void *context,
                                         const vk_usb_screen_command_t *command,
                                         vk_screen_model_t *candidate)
{
    vk_screen_service_t *service = context;
    if (service == NULL || service->store == NULL || command == NULL || candidate == NULL ||
        command->document == NULL) return ESP_ERR_INVALID_ARG;
    uint16_t assets = vk_usb_json_object_find(command->document, command->assets_node, "assets");
    uint16_t configured_mode = vk_usb_json_object_find(command->document, command->screen_node, "configured_mode");
    uint16_t image = vk_usb_json_object_find(command->document, command->screen_node, "image");
    uint16_t layout = vk_usb_json_object_find(command->document, command->screen_node, "layout");
    uint16_t pet = vk_usb_json_object_find(command->document, command->screen_node, "pet");
    manifest_builder_t asset_builder = {.bytes=service->publish_assets_manifest,.capacity=sizeof(service->publish_assets_manifest)};
    manifest_builder_t screen_builder = {.bytes=service->publish_screen_manifest,.capacity=sizeof(service->publish_screen_manifest)};
    bool valid = manifest_append(&asset_builder, "{\"assets\":") &&
                 manifest_node(&asset_builder, command->document, assets, 1U) &&
                 manifest_append(&asset_builder, ",\"previous_revision\":%" PRIu32 ",\"revision\":%" PRIu32 ",\"schema\":1}", candidate->previous_revision, candidate->revision) &&
                 manifest_append(&screen_builder, "{\"configured_mode\":") &&
                 manifest_node(&screen_builder, command->document, configured_mode, 1U) &&
                 manifest_append(&screen_builder, ",\"image\":") && manifest_node(&screen_builder, command->document, image, 1U) &&
                 manifest_append(&screen_builder, ",\"layout\":") && manifest_node(&screen_builder, command->document, layout, 1U) &&
                 manifest_append(&screen_builder, ",\"pet\":") && manifest_node(&screen_builder, command->document, pet, 1U) &&
                 manifest_append(&screen_builder, ",\"previous_revision\":%" PRIu32 ",\"revision\":%" PRIu32 ",\"schema\":1}", candidate->previous_revision, candidate->revision);
    if (!valid || asset_builder.used > VK_USB_MAX_JSON_BYTES || screen_builder.used > VK_USB_MAX_JSON_BYTES) return ESP_ERR_INVALID_SIZE;
    vk_asset_revision_t revision = {
        .revision = candidate->revision, .previous_revision = candidate->previous_revision,
        .assets_manifest = asset_builder.bytes, .assets_manifest_bytes = asset_builder.used,
        .screen_manifest = screen_builder.bytes, .screen_manifest_bytes = screen_builder.used,
    };
    uint8_t screen_hash[VK_ASSET_SHA256_BYTES], assets_hash[VK_ASSET_SHA256_BYTES];
    esp_err_t result = vk_asset_store_publish_revision(service->store, &revision, screen_hash, assets_hash);
    if (result == ESP_OK) {
        static const char hex[] = "0123456789abcdef";
        for (size_t index = 0U; index < VK_ASSET_SHA256_BYTES; ++index) {
            candidate->screen_manifest_sha256[index * 2U] = hex[screen_hash[index] >> 4U];
            candidate->screen_manifest_sha256[index * 2U + 1U] = hex[screen_hash[index] & 15U];
            candidate->assets_manifest_sha256[index * 2U] = hex[assets_hash[index] >> 4U];
            candidate->assets_manifest_sha256[index * 2U + 1U] = hex[assets_hash[index] & 15U];
        }
        candidate->screen_manifest_sha256[64] = 0;
        candidate->assets_manifest_sha256[64] = 0;
    }
    return result;
}

static bool ready(const vk_screen_service_t *service)
{
    return service != NULL && service->store != NULL && service->transfer != NULL &&
           service->screen != NULL && service->display != NULL && service->store_recovered &&
           service->font_profile_admitted && service->physical_acceptance_admitted &&
           vk_asset_store_state(service->store) == VK_ASSET_STORE_READY &&
           vk_display_screen_available(service->display);
}

esp_err_t vk_screen_service_init(vk_screen_service_t *service,
                                 vk_asset_store_t *store,
                                 vk_asset_transfer_service_t *transfer,
                                 vk_screen_t *screen,
                                 vk_display_t *display,
                                 const vk_screen_service_readiness_t *readiness)
{
    if (service == NULL || store == NULL || transfer == NULL || screen == NULL || display == NULL ||
        readiness == NULL || !readiness->mounted || !readiness->transfer_ready ||
        !readiness->screen_ready || !readiness->display_ready) return ESP_ERR_INVALID_ARG;
    screen->config.durable_publish = publish_screen_revision;
    screen->config.durable_context = service;
    *service = (vk_screen_service_t){
        .store = store,
        .transfer = transfer,
        .screen = screen,
        .display = display,
        .store_recovered = readiness->recovered,
        .font_profile_admitted = readiness->font_ready,
        .physical_acceptance_admitted = readiness->physical_acceptance_admitted,
    };
    return ESP_OK;
}

esp_err_t vk_screen_service_get_capabilities(void *context, uint32_t expected_epoch,
                                             vk_usb_capability_snapshot_t *snapshot)
{
    vk_screen_service_t *service = context;
    if (service == NULL || snapshot == NULL || expected_epoch == 0U) return ESP_ERR_INVALID_ARG;
    esp_err_t result = vk_asset_transfer_get_capabilities(service->transfer, expected_epoch, snapshot);
    if (result != ESP_OK) return result;
    /* LED capability is owned by an independent provider (app_main.c);
     * screen service must not supply or override it. */
    snapshot->screen.state = VK_USB_CAPABILITY_UNAVAILABLE;
    snapshot->screen.unavailable_reason = VK_USB_SCREEN_REASON_DISPLAY_ACCEPTANCE_REQUIRED;
    if (!ready(service)) return ESP_OK;
    snapshot->screen = service->screen->config.capability;
    snapshot->screen.state = VK_USB_CAPABILITY_AVAILABLE;
    snapshot->screen.revision = service->screen->current.revision;
    snapshot->screen.configured = service->screen->current.configured;
    return ESP_OK;
}

static esp_err_t restore_one(vk_screen_service_t *service, uint32_t revision,
                             vk_usb_json_document_t *document,
                             uint8_t *envelope, size_t envelope_capacity)
{
    size_t assets_bytes = 0U, screen_bytes = 0U;
    uint32_t previous = 0U;
    esp_err_t result = vk_asset_store_load_revision(service->store, revision, &previous,
                                                     service->restore_assets_manifest,
                                                     sizeof(service->restore_assets_manifest), &assets_bytes,
                                                     service->restore_screen_manifest,
                                                     sizeof(service->restore_screen_manifest), &screen_bytes);
    if (result != ESP_OK) return result;
    return vk_screen_restore(service->screen, revision, previous,
                             service->restore_assets_manifest, assets_bytes,
                             service->restore_screen_manifest, screen_bytes,
                             document, envelope, envelope_capacity);
}

esp_err_t vk_screen_service_restore(vk_screen_service_t *service,
                                    const vk_asset_recovery_t *recovery,
                                    vk_usb_json_document_t *document,
                                    uint8_t *envelope, size_t envelope_capacity)
{
    if (service == NULL || recovery == NULL || document == NULL || envelope == NULL ||
        envelope_capacity < VK_USB_MAX_JSON_BYTES) return ESP_ERR_INVALID_ARG;
    service->store_recovered = false;
    if (recovery->has_current && restore_one(service, recovery->current_revision,
                                             document, envelope, envelope_capacity) == ESP_OK) {
        service->store_recovered = true;
        return ESP_OK;
    }
    if (recovery->has_previous && restore_one(service, recovery->previous_revision,
                                              document, envelope, envelope_capacity) == ESP_OK) {
        service->store_recovered = true;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t vk_screen_service_handle_screen(void *context,
                                          const vk_usb_screen_command_t *command,
                                          vk_usb_screen_event_t *event)
{
    vk_screen_service_t *service = context;
    if (!ready(service) || command == NULL || event == NULL) return ESP_ERR_NOT_SUPPORTED;
    if (command->kind == VK_USB_SCREEN_QUERY) {
        return vk_screen_query(service->screen, command->expected_epoch,
                               command->snapshot_generation, event);
    }
    if (command->kind != VK_USB_SCREEN_COMMIT) return ESP_ERR_NOT_SUPPORTED;

    /* vk_screen_commit constructs the candidate, invokes the configured durable
     * commit-last publisher, and swaps the root only after durable success. */
    return vk_screen_commit(service->screen, command, event);
}

esp_err_t vk_screen_service_handle_widget(void *context,
                                          const vk_usb_widget_command_t *command,
                                          vk_usb_widget_event_t *event)
{
    vk_screen_service_t *service = context;
    if (!ready(service) || command == NULL || event == NULL) return ESP_ERR_NOT_SUPPORTED;
    vk_screen_widget_update_t update = {
        .revision = command->revision,
        .sequence = command->sequence,
        .state = command->state == VK_USB_WIDGET_FRESH ? VK_SCREEN_WIDGET_FRESH :
                 command->state == VK_USB_WIDGET_STALE ? VK_SCREEN_WIDGET_STALE : VK_SCREEN_WIDGET_ERROR,
        .has_text = command->value_kind == VK_USB_WIDGET_VALUE_TEXT,
        .has_number = command->value_kind == VK_USB_WIDGET_VALUE_NUMBER,
        .number_milli = command->number_milli,
    };
    snprintf(update.widget_id, sizeof(update.widget_id), "%s", command->widget_id);
    if (update.has_text) snprintf(update.text, sizeof(update.text), "%s", command->text);
    char rendered[VK_SCREEN_MAX_TEXT_BYTES + 1U];
    esp_err_t result = vk_screen_widget_update(service->screen, &update, rendered);
    if (result != ESP_OK) return result;
    *event = (vk_usb_widget_event_t){
        .revision = command->revision,
        .sequence = command->sequence,
        .state = command->state,
    };
    snprintf(event->widget_id, sizeof(event->widget_id), "%s", command->widget_id);
    return ESP_OK;
}

void vk_screen_service_registrations(vk_screen_service_t *service,
                                     vk_usb_capability_provider_registration_t *capabilities,
                                     vk_usb_asset_handler_registration_t *assets,
                                     vk_usb_screen_handler_registration_t *screen,
                                     vk_usb_widget_handler_registration_t *widget)
{
    vk_asset_transfer_registration(service == NULL ? NULL : service->transfer, capabilities, assets);
    if (capabilities != NULL && service != NULL) {
        capabilities->get_snapshot = vk_screen_service_get_capabilities;
        capabilities->context = service;
    }
    if (screen != NULL) *screen = (vk_usb_screen_handler_registration_t){
        .handle_command = vk_screen_service_handle_screen, .context = service};
    if (widget != NULL) *widget = (vk_usb_widget_handler_registration_t){
        .handle_command = vk_screen_service_handle_widget, .context = service};
}

void vk_screen_service_close_epoch(vk_screen_service_t *service, uint32_t epoch)
{
    if (service == NULL || epoch == 0U) return;
    vk_asset_transfer_close_epoch(service->transfer, epoch);
    vk_screen_close_epoch(service->screen, epoch);
}
