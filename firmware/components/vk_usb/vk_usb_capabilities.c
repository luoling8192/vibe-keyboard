#include "vk_usb_capabilities.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ALL_SCREEN_MODES ((uint8_t)(VK_USB_SCREEN_MODE_IMAGE | VK_USB_SCREEN_MODE_PET | \
                                    VK_USB_SCREEN_MODE_DASHBOARD | VK_USB_SCREEN_MODE_CUSTOM))

typedef struct { char *bytes; size_t capacity; size_t count; bool failed; } json_builder_t;

static void appendf(json_builder_t *builder, const char *format, ...)
{
    if (builder->failed || builder->count >= builder->capacity) { builder->failed = true; return; }
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(builder->bytes + builder->count, builder->capacity - builder->count,
                            format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= builder->capacity - builder->count) {
        builder->failed = true;
        return;
    }
    builder->count += (size_t)written;
}

static bool valid_identifier(const char *value)
{
    size_t length = strnlen(value, 33U);
    if (length == 0U || length > 32U) return false;
    for (size_t index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') || byte == '-' || byte == '_')) return false;
    }
    return true;
}

static bool valid_sha256(const char *value)
{
    if (strnlen(value, 65U) != 64U) return false;
    for (size_t index = 0; index < 64U; ++index) {
        char byte = value[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
    }
    return true;
}

static bool valid_assets(const vk_usb_assets_capability_t *assets)
{
    if (assets->state == VK_USB_CAPABILITY_ABSENT) return true;
    if (assets->state == VK_USB_CAPABILITY_UNAVAILABLE)
        return assets->unavailable_reason <= VK_USB_ASSETS_REASON_POLICY_BLOCKED;
    if (assets->state != VK_USB_CAPABILITY_AVAILABLE ||
        assets->storage_state > VK_USB_STORAGE_BUSY || assets->reserve_bytes == 0U ||
        assets->max_asset_bytes == 0U || assets->chunk_bytes == 0U ||
        assets->chunk_bytes > VK_USB_ASSET_CHUNK_MAX_BYTES || assets->max_assets == 0U ||
        assets->max_assets > 1024U || assets->max_frames == 0U ||
        assets->min_frame_ms == 0U || assets->max_frame_ms == 0U ||
        assets->min_frame_ms > assets->max_frame_ms || assets->max_active_decoded_bytes == 0U ||
        assets->decoder_scratch_bytes == 0U ||
        assets->decoder_scratch_bytes > assets->max_active_decoded_bytes) return false;
    if (assets->storage_state != VK_USB_STORAGE_READY &&
        assets->storage_state != VK_USB_STORAGE_BUSY && assets->free_bytes != 0U) return false;
    if (assets->storage_state != VK_USB_STORAGE_READY) return assets->upload_max_bytes == 0U;
    uint32_t usable = assets->free_bytes > assets->reserve_bytes
        ? assets->free_bytes - assets->reserve_bytes : 0U;
    return assets->upload_max_bytes <= usable &&
           assets->upload_max_bytes <= assets->max_asset_bytes;
}

static bool valid_screen(const vk_usb_screen_capability_t *screen,
                         const vk_usb_assets_capability_t *assets)
{
    if (screen->state == VK_USB_CAPABILITY_ABSENT) return true;
    if (screen->state == VK_USB_CAPABILITY_UNAVAILABLE)
        return screen->unavailable_reason <= VK_USB_SCREEN_REASON_POLICY_BLOCKED;
    if (screen->state != VK_USB_CAPABILITY_AVAILABLE ||
        assets->state != VK_USB_CAPABILITY_AVAILABLE || screen->modes == 0U ||
        (screen->modes & (uint8_t)~ALL_SCREEN_MODES) != 0U ||
        screen->max_commit_bytes == 0U || screen->max_commit_bytes > VK_USB_MAX_JSON_BYTES ||
        screen->max_layout_bytes == 0U || screen->max_layout_bytes > screen->max_commit_bytes ||
        screen->max_assets == 0U || screen->max_assets > 1024U ||
        screen->max_assets > assets->max_assets || screen->max_objects == 0U ||
        screen->max_depth == 0U || screen->max_depth > 8U || screen->max_widgets == 0U ||
        screen->max_fonts == 0U || screen->max_fonts > VK_USB_CAPABILITY_MAX_FONTS ||
        screen->max_pet_states == 0U || screen->max_pet_states > 6U ||
        screen->max_string_bytes == 0U || screen->max_string_bytes > 512U ||
        screen->max_json_tokens < 32U || screen->max_json_tokens > 1024U ||
        screen->max_widget_value_bytes == 0U || screen->max_widget_value_bytes > 512U ||
        screen->font_count == 0U || screen->font_count > screen->max_fonts ||
        screen->font_count > VK_USB_CAPABILITY_MAX_FONTS ||
        (screen->configured && screen->revision == 0U) ||
        (!screen->configured && screen->revision != 0U)) return false;
    for (uint16_t index = 0; index < screen->font_count; ++index) {
        const vk_usb_font_capability_t *font = &screen->fonts[index];
        if (!valid_identifier(font->id) || font->version == 0U ||
            !valid_sha256(font->metrics_sha256)) return false;
        if (index != 0U && strcmp(screen->fonts[index - 1U].id, font->id) >= 0) return false;
    }
    return true;
}

static bool valid_led(const vk_usb_led_capability_t *led)
{
    if (led->state == VK_USB_CAPABILITY_ABSENT) return true;
    if (led->state == VK_USB_CAPABILITY_UNAVAILABLE)
        return led->unavailable_reason <= VK_USB_LED_REASON_TAINTED;
    if (led->state != VK_USB_CAPABILITY_AVAILABLE || led->strip_first != 4U ||
        led->strip_count != 13U || led->max_brightness == 0U ||
        led->max_frame_channel_sum == 0U ||
        led->max_frame_channel_sum > (uint16_t)(VK_USB_LED_PIXEL_COUNT * 3U * 255U)) return false;
    uint8_t seen = 0U;
    for (size_t index = 0; index < 4U; ++index) {
        uint8_t pixel = led->key_pixels[index];
        if (pixel > 3U || (seen & (uint8_t)(1U << pixel)) != 0U) return false;
        seen |= (uint8_t)(1U << pixel);
    }
    return seen == 0x0fU;
}

static bool valid_update(const vk_usb_update_capability_t *update, bool policy)
{
    (void)policy;
    if (update->state == VK_USB_CAPABILITY_ABSENT) return true;
    if (update->state == VK_USB_CAPABILITY_UNAVAILABLE)
        return update->unavailable_reason <= VK_USB_UPDATE_REASON_POLICY_BLOCKED;
    /* Available update capability remains unrepresentable until the reviewed
     * bootloader/update owner supplies an evidence-bound running/target tuple. */
    return false;
}

bool vk_usb_capability_snapshot_validate(const vk_usb_capability_snapshot_t *snapshot,
                                         bool update_boot_policy_enabled)
{
    return snapshot != NULL && valid_assets(&snapshot->assets) &&
           valid_screen(&snapshot->screen, &snapshot->assets) && valid_led(&snapshot->led) &&
           valid_update(&snapshot->update, update_boot_policy_enabled);
}

static const char *assets_reason(vk_usb_assets_unavailable_reason_t reason)
{
    static const char *const values[] = {"display_acceptance_required", "storage_unavailable",
                                         "integrity_unavailable", "policy_blocked"};
    return reason <= VK_USB_ASSETS_REASON_POLICY_BLOCKED ? values[reason] : NULL;
}
static const char *screen_reason(vk_usb_screen_unavailable_reason_t reason)
{
    static const char *const values[] = {"display_acceptance_required", "panel_unavailable",
                                         "model_unavailable", "storage_unavailable", "policy_blocked"};
    return reason <= VK_USB_SCREEN_REASON_POLICY_BLOCKED ? values[reason] : NULL;
}
static const char *led_reason(vk_usb_led_unavailable_reason_t reason)
{
    static const char *const values[] = {"calibration_required", "hardware_failed", "tainted"};
    return reason <= VK_USB_LED_REASON_TAINTED ? values[reason] : NULL;
}
static const char *update_reason(vk_usb_update_unavailable_reason_t reason)
{
    static const char *const values[] = {"bootloader_migration_required", "busy",
        "wrong_running_slot", "target_unavailable", "integrity_unavailable", "policy_blocked"};
    return reason <= VK_USB_UPDATE_REASON_POLICY_BLOCKED ? values[reason] : NULL;
}
static const char *storage_state(vk_usb_storage_state_t state)
{
    static const char *const values[] = {"unformatted", "ready", "corrupt", "mount_failed", "busy"};
    return state <= VK_USB_STORAGE_BUSY ? values[state] : NULL;
}

static void encode_assets(json_builder_t *builder, const vk_usb_assets_capability_t *assets)
{
    if (assets->state == VK_USB_CAPABILITY_UNAVAILABLE) {
        appendf(builder, "\"assets\":{\"available\":false,\"reason\":\"%s\",\"version\":1}",
                assets_reason(assets->unavailable_reason));
    } else {
        appendf(builder, "\"assets\":{\"available\":true,\"chunk_bytes\":%u,"
                "\"decoder_scratch_bytes\":%" PRIu32 ",\"encodings\":[\"raw\",\"row_rle\"],"
                "\"free_bytes\":%" PRIu32 ",\"management\":true,"
                "\"max_active_decoded_bytes\":%" PRIu32 ",\"max_asset_bytes\":%" PRIu32 ","
                "\"max_assets\":%u,\"max_frame_ms\":%u,\"max_frames\":%u,"
                "\"min_frame_ms\":%u,\"reserve_bytes\":%" PRIu32 ",\"revision\":%" PRIu32 ","
                "\"storage_state\":\"%s\",\"upload_max_bytes\":%" PRIu32 ",\"version\":1}",
                (unsigned)assets->chunk_bytes, assets->decoder_scratch_bytes, assets->free_bytes,
                assets->max_active_decoded_bytes, assets->max_asset_bytes,
                (unsigned)assets->max_assets, (unsigned)assets->max_frame_ms,
                (unsigned)assets->max_frames, (unsigned)assets->min_frame_ms,
                assets->reserve_bytes, assets->revision, storage_state(assets->storage_state),
                assets->upload_max_bytes);
    }
}

static void encode_modes(json_builder_t *builder, uint8_t modes)
{
    appendf(builder, "[");
    const struct { uint8_t bit; const char *name; } values[] = {
        {VK_USB_SCREEN_MODE_IMAGE, "image"}, {VK_USB_SCREEN_MODE_PET, "pet"},
        {VK_USB_SCREEN_MODE_DASHBOARD, "dashboard"}, {VK_USB_SCREEN_MODE_CUSTOM, "custom"}};
    bool comma = false;
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        if ((modes & values[index].bit) != 0U) {
            appendf(builder, "%s\"%s\"", comma ? "," : "", values[index].name);
            comma = true;
        }
    }
    appendf(builder, "]");
}

static void encode_screen(json_builder_t *builder, const vk_usb_screen_capability_t *screen)
{
    if (screen->state == VK_USB_CAPABILITY_UNAVAILABLE) {
        appendf(builder, "\"screen\":{\"available\":false,\"reason\":\"%s\",\"version\":1}",
                screen_reason(screen->unavailable_reason));
        return;
    }
    appendf(builder, "\"screen\":{\"available\":true,\"configured\":%s,\"fonts\":[",
            screen->configured ? "true" : "false");
    for (uint16_t index = 0; index < screen->font_count; ++index) {
        const vk_usb_font_capability_t *font = &screen->fonts[index];
        appendf(builder, "%s{\"id\":\"%s\",\"metrics_sha256\":\"%s\",\"version\":%u}",
                index == 0U ? "" : ",", font->id, font->metrics_sha256,
                (unsigned)font->version);
    }
    appendf(builder, "],\"max_assets\":%u,\"max_commit_bytes\":%u,\"max_depth\":%u,"
            "\"max_fonts\":%u,\"max_json_tokens\":%u,\"max_layout_bytes\":%u,"
            "\"max_objects\":%u,\"max_pet_states\":%u,\"max_string_bytes\":%u,"
            "\"max_widget_value_bytes\":%u,\"max_widgets\":%u,\"modes\":",
            (unsigned)screen->max_assets, (unsigned)screen->max_commit_bytes,
            (unsigned)screen->max_depth, (unsigned)screen->max_fonts,
            (unsigned)screen->max_json_tokens, (unsigned)screen->max_layout_bytes,
            (unsigned)screen->max_objects, (unsigned)screen->max_pet_states,
            (unsigned)screen->max_string_bytes, (unsigned)screen->max_widget_value_bytes,
            (unsigned)screen->max_widgets);
    encode_modes(builder, screen->modes);
    appendf(builder, ",\"revision\":%" PRIu32 ",\"version\":1}", screen->revision);
}

static void encode_led(json_builder_t *builder, const vk_usb_led_capability_t *led)
{
    if (led->state == VK_USB_CAPABILITY_UNAVAILABLE) {
        appendf(builder, "\"led\":{\"available\":false,\"reason\":\"%s\",\"version\":1}",
                led_reason(led->unavailable_reason));
        return;
    }
    appendf(builder, "\"led\":{\"available\":true,\"color_model\":\"rgb8\","
            "\"key_pixels\":{\"k1\":%u,\"k2\":%u,\"k3\":%u,\"k4\":%u},"
            "\"max_brightness\":%u,\"max_frame_channel_sum\":%u,\"pixel_count\":17,"
            "\"strip_count\":13,\"strip_first\":4,\"tick_ms\":30,\"version\":1,"
            "\"wire_order\":\"grb\"}",
            (unsigned)led->key_pixels[0], (unsigned)led->key_pixels[1],
            (unsigned)led->key_pixels[2], (unsigned)led->key_pixels[3],
            (unsigned)led->max_brightness, (unsigned)led->max_frame_channel_sum);
}

static void encode_update(json_builder_t *builder, const vk_usb_update_capability_t *update)
{
    if (update->state == VK_USB_CAPABILITY_UNAVAILABLE) {
        appendf(builder, "\"update\":{\"available\":false,\"reason\":\"%s\",\"version\":1}",
                update_reason(update->unavailable_reason));
    } else {
        appendf(builder, "\"update\":{\"available\":true,\"chunk_bytes\":%u,"
                "\"max_image_bytes\":%" PRIu32 ",\"rollback\":\"bootloader_pending_verify\","
                "\"staged_metadata\":\"ram_epoch\",\"target\":\"%s\",\"version\":1}",
                (unsigned)update->chunk_bytes, update->max_image_bytes,
                update->target == VK_USB_UPDATE_TARGET_OTA_0 ? "ota_0" : "ota_1");
    }
}

esp_err_t vk_usb_capability_snapshot_encode(const vk_usb_capability_snapshot_t *snapshot,
                                            char *output, size_t capacity,
                                            size_t *output_length)
{
    if (snapshot == NULL || output == NULL || output_length == NULL || capacity == 0U)
        return ESP_ERR_INVALID_ARG;
    if (snapshot->update.state == VK_USB_CAPABILITY_AVAILABLE)
        return ESP_ERR_NOT_SUPPORTED;
    json_builder_t builder = {.bytes = output, .capacity = capacity};
    appendf(&builder, "{\"event\":\"vk_capabilities\",\"protocol\":1,"
            "\"display\":{\"width\":428,\"height\":142,\"format\":\"rgb565\"},\"features\":{");
    bool comma = false;
    if (snapshot->assets.state != VK_USB_CAPABILITY_ABSENT) {
        encode_assets(&builder, &snapshot->assets); comma = true;
    }
    if (snapshot->screen.state != VK_USB_CAPABILITY_ABSENT) {
        appendf(&builder, "%s", comma ? "," : ""); encode_screen(&builder, &snapshot->screen); comma = true;
    }
    if (snapshot->update.state != VK_USB_CAPABILITY_ABSENT) {
        appendf(&builder, "%s", comma ? "," : ""); encode_update(&builder, &snapshot->update); comma = true;
    }
    if (snapshot->led.state != VK_USB_CAPABILITY_ABSENT) {
        appendf(&builder, "%s", comma ? "," : ""); encode_led(&builder, &snapshot->led);
    }
    appendf(&builder, "}}");
    if (builder.failed || builder.count > VK_USB_MAX_JSON_BYTES) return ESP_ERR_INVALID_SIZE;
    *output_length = builder.count;
    return ESP_OK;
}
