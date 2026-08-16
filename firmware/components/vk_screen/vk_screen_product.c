#include "vk_screen_service.h"

#ifdef ESP_PLATFORM

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "vk_vka1.h"

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

static vk_asset_fs_ops_t s_fs;
static vk_asset_store_t s_store;
static vk_asset_transfer_service_t s_transfer;
static vk_screen_t s_screen;
static vk_screen_service_t s_service;
static bool s_mounted;
static bool s_screen_initialized;
static vk_usb_json_document_t s_restore_document;
static uint8_t s_restore_envelope[VK_USB_MAX_JSON_BYTES];
static struct _lv_timer_t *s_tick_timer;
static lv_font_t s_product_font;

static esp_err_t show_acceptance_screen(void)
{
    if (!lvgl_port_lock(0)) return ESP_ERR_TIMEOUT;
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    static const uint32_t colors[] = {0x00C2A8, 0xFFB000, 0xFF4D6D};
    for (size_t index = 0U; index < sizeof(colors) / sizeof(colors[0]); ++index) {
        lv_obj_t *bar = lv_obj_create(screen);
        if (bar == NULL) {
            lvgl_port_unlock();
            return ESP_ERR_NO_MEM;
        }
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, (int32_t)(index * 6U), 0);
        lv_obj_set_size(bar, 6, VK_SCREEN_HEIGHT);
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors[index]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_t *status = lv_label_create(screen);
    if (title == NULL || status == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(title, "VIBEBOARD");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(title, 34, 42);
    lv_label_set_text(status, "USB MICROPHONE  /  READY");
    lv_obj_set_style_text_color(status, lv_color_hex(0x8EA6B8), 0);
    lv_obj_set_pos(status, 34, 76);
    lv_obj_invalidate(screen);
    lvgl_port_unlock();
    return ESP_OK;
}

static void tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    vk_screen_lvgl_tick(lv_tick_get());
}

static void tick_timer_ensure(void)
{
    if (s_tick_timer == NULL && lv_is_initialized()) {
        s_tick_timer = lv_timer_create(tick_timer_cb, 30U, NULL);
    }
}

static void tick_timer_stop(void)
{
    if (s_tick_timer != NULL) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
    }
}

static uint64_t product_now_ms(void *context)
{
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static esp_err_t read_exact(const char *path, uint8_t *bytes, size_t count)
{
    size_t offset = 0U;
    while (offset < count) {
        size_t got = 0U;
        esp_err_t result = s_fs.read_file(NULL, path, offset, bytes + offset, count - offset, &got);
        if (result != ESP_OK) return result;
        if (got == 0U) return ESP_ERR_INVALID_SIZE;
        offset += got;
    }
    return ESP_OK;
}

static bool parse_sha(const char *text, uint8_t output[32])
{
    if (text == NULL || strlen(text) != 64U) return false;
    for (size_t index = 0U; index < 32U; ++index) {
        unsigned high = text[index * 2U] >= '0' && text[index * 2U] <= '9' ? (unsigned)(text[index * 2U] - '0') :
                        text[index * 2U] >= 'a' && text[index * 2U] <= 'f' ? (unsigned)(text[index * 2U] - 'a' + 10) : 16U;
        unsigned low = text[index * 2U + 1U] >= '0' && text[index * 2U + 1U] <= '9' ? (unsigned)(text[index * 2U + 1U] - '0') :
                       text[index * 2U + 1U] >= 'a' && text[index * 2U + 1U] <= 'f' ? (unsigned)(text[index * 2U + 1U] - 'a' + 10) : 16U;
        if (high > 15U || low > 15U) return false;
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

static esp_err_t validate_vka1_file(void *context, const char *name,
                                    const uint8_t expected_hash[32],
                                    uint32_t exact_bytes, vk_asset_kind_t kind)
{
    (void)context;
    if (exact_bytes == 0U || exact_bytes > VK_SCREEN_SERVICE_MAX_ASSET_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *asset_bytes = malloc(exact_bytes);
    if (asset_bytes == NULL) return ESP_ERR_NO_MEM;
    esp_err_t result = read_exact(name, asset_bytes, exact_bytes);
    if (result != ESP_OK) { free(asset_bytes); return result; }
    vk_vka1_info_t info;
    vk_vka1_limits_t limits = {
        .max_frames = VK_SCREEN_SERVICE_MAX_FRAMES,
        .min_frame_ms = VK_SCREEN_SERVICE_MIN_FRAME_MS,
        .max_frame_ms = VK_SCREEN_SERVICE_MAX_FRAME_MS,
        .max_container_bytes = VK_SCREEN_SERVICE_MAX_ASSET_BYTES,
        .max_decoded_bytes = VK_SCREEN_SERVICE_DECODE_BUDGET_BYTES,
    };
    bool valid = vk_vka1_validate(asset_bytes, exact_bytes, &limits, &info) == VK_VKA1_OK &&
                 info.kind == (vk_vka1_kind_t)(kind + 1U) && memcmp(info.sha256, expected_hash, 32U) == 0;
    free(asset_bytes);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

typedef struct {
    uint8_t digest[VK_ASSET_SHA256_BYTES];
    uint32_t bytes;
    vk_asset_kind_t kind;
} revision_asset_t;

static bool revision_uint32(const vk_usb_json_document_t *document, uint16_t object,
                            const char *key, uint32_t expected)
{
    uint32_t value = 0U;
    uint16_t node = vk_usb_json_object_find(document, object, key);
    return node != VK_USB_JSON_NO_NODE &&
           vk_usb_json_uint32(document, node, UINT32_MAX, false, &value) == VK_USB_JSON_OK &&
           value == expected;
}

static bool revision_sha(const vk_usb_json_document_t *document, uint16_t node,
                         uint8_t digest[VK_ASSET_SHA256_BYTES])
{
    char value[VK_ASSET_SHA256_HEX_BYTES + 1U];
    return vk_usb_json_kind(document, node) == VK_USB_JSON_STRING &&
           vk_usb_json_string_copy(document, node, value, sizeof(value)) == VK_USB_JSON_OK &&
           parse_sha(value, digest);
}

static bool revision_reference_known(const revision_asset_t *assets, size_t count,
                                     const uint8_t digest[VK_ASSET_SHA256_BYTES])
{
    for (size_t index = 0U; index < count; ++index)
        if (memcmp(assets[index].digest, digest, VK_ASSET_SHA256_BYTES) == 0) return true;
    return false;
}

static bool revision_references_valid(const vk_usb_json_document_t *document, uint16_t node,
                                      const revision_asset_t *assets, size_t count)
{
    if (node == VK_USB_JSON_NO_NODE) return false;
    if (vk_usb_json_kind(document, node) == VK_USB_JSON_OBJECT) {
        for (uint16_t key = vk_usb_json_first_child(document, node); key != VK_USB_JSON_NO_NODE;) {
            uint16_t value = vk_usb_json_next_sibling(document, key);
            char name[VK_USB_JSON_MAX_STRING_BYTES + 1U];
            if (!vk_usb_json_node_is_object_key(document, key) || value == VK_USB_JSON_NO_NODE ||
                vk_usb_json_string_copy(document, key, name, sizeof(name)) != VK_USB_JSON_OK) return false;
            if (strcmp(name, "sha256") == 0) {
                uint8_t digest[VK_ASSET_SHA256_BYTES];
                if (!revision_sha(document, value, digest) || !revision_reference_known(assets, count, digest)) return false;
            } else if (!revision_references_valid(document, value, assets, count)) return false;
            key = vk_usb_json_next_sibling(document, value);
        }
    } else if (vk_usb_json_kind(document, node) == VK_USB_JSON_ARRAY) {
        for (uint16_t child = vk_usb_json_first_child(document, node); child != VK_USB_JSON_NO_NODE;
             child = vk_usb_json_next_sibling(document, child))
            if (!revision_references_valid(document, child, assets, count)) return false;
    }
    return true;
}

static esp_err_t validate_revision(void *context, uint32_t revision,
                                   uint32_t previous_revision,
                                   const uint8_t *assets_manifest, size_t assets_bytes,
                                   const uint8_t *screen_manifest, size_t screen_bytes)
{
    typedef struct {
        vk_usb_json_document_t assets_document;
        vk_usb_json_document_t screen_document;
        revision_asset_t catalog[VK_SCREEN_SERVICE_MAX_ASSETS];
    } revision_workspace_t;

    vk_asset_store_t *store = context;
    static const char *const assets_keys[] = {"assets", "previous_revision", "revision", "schema"};
    static const char *const entry_keys[] = {"bytes", "kind", "sha256"};
    static const char *const screen_keys[] = {"configured_mode", "image", "layout", "pet", "previous_revision", "revision", "schema"};
    if (store == NULL || assets_manifest == NULL || screen_manifest == NULL || assets_bytes == 0U ||
        screen_bytes == 0U || assets_bytes > VK_USB_MAX_JSON_BYTES || screen_bytes > VK_USB_MAX_JSON_BYTES)
        return ESP_ERR_INVALID_ARG;

    revision_workspace_t *workspace = heap_caps_calloc(
        1U, sizeof(*workspace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (workspace == NULL) return ESP_ERR_NO_MEM;
    vk_usb_json_document_t *assets_document = &workspace->assets_document;
    vk_usb_json_document_t *screen_document = &workspace->screen_document;
    revision_asset_t *catalog = workspace->catalog;
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;

    if (vk_usb_json_parse(assets_document, assets_manifest, assets_bytes) != VK_USB_JSON_OK ||
        vk_usb_json_parse(screen_document, screen_manifest, screen_bytes) != VK_USB_JSON_OK) {
        goto cleanup;
    }
    uint16_t assets_root = vk_usb_json_root(assets_document);
    uint16_t screen_root = vk_usb_json_root(screen_document);
    if (!vk_usb_json_object_exact_keys(assets_document, assets_root, assets_keys, 4U) ||
        !vk_usb_json_object_exact_keys(screen_document, screen_root, screen_keys, 7U) ||
        !revision_uint32(assets_document, assets_root, "schema", 1U) ||
        !revision_uint32(assets_document, assets_root, "revision", revision) ||
        !revision_uint32(assets_document, assets_root, "previous_revision", previous_revision) ||
        !revision_uint32(screen_document, screen_root, "schema", 1U) ||
        !revision_uint32(screen_document, screen_root, "revision", revision) ||
        !revision_uint32(screen_document, screen_root, "previous_revision", previous_revision)) {
        goto cleanup;
    }
    uint16_t list = vk_usb_json_object_find(assets_document, assets_root, "assets");
    size_t count = vk_usb_json_array_count(assets_document, list);
    if (vk_usb_json_kind(assets_document, list) != VK_USB_JSON_ARRAY ||
        count > VK_SCREEN_SERVICE_MAX_ASSETS) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    size_t used = 0U;
    for (uint16_t entry = vk_usb_json_first_child(assets_document, list);
         entry != VK_USB_JSON_NO_NODE;
         entry = vk_usb_json_next_sibling(assets_document, entry)) {
        if (!vk_usb_json_object_exact_keys(assets_document, entry, entry_keys, 3U)) goto cleanup;
        uint32_t bytes = 0U; uint8_t digest[VK_ASSET_SHA256_BYTES]; char kind_name[20];
        uint16_t bytes_node = vk_usb_json_object_find(assets_document, entry, "bytes");
        uint16_t kind_node = vk_usb_json_object_find(assets_document, entry, "kind");
        uint16_t sha_node = vk_usb_json_object_find(assets_document, entry, "sha256");
        if (vk_usb_json_uint32(assets_document, bytes_node, VK_SCREEN_SERVICE_MAX_ASSET_BYTES, true, &bytes) != VK_USB_JSON_OK ||
            vk_usb_json_string_copy(assets_document, kind_node, kind_name, sizeof(kind_name)) != VK_USB_JSON_OK ||
            !revision_sha(assets_document, sha_node, digest)) goto cleanup;
        vk_asset_kind_t kind = strcmp(kind_name, "image") == 0 ? VK_ASSET_KIND_IMAGE :
                               strcmp(kind_name, "animation") == 0 ? VK_ASSET_KIND_ANIMATION :
                               strcmp(kind_name, "glyph_bitmap") == 0 ? VK_ASSET_KIND_GLYPH_BITMAP : (vk_asset_kind_t)99;
        if (kind > VK_ASSET_KIND_GLYPH_BITMAP ||
            (used != 0U && memcmp(catalog[used - 1U].digest, digest, 32U) >= 0)) goto cleanup;
        char hash[65], path[VK_ASSET_PATH_BYTES]; static const char digits[] = "0123456789abcdef";
        for (size_t index = 0U; index < 32U; ++index) { hash[index * 2U] = digits[digest[index] >> 4U]; hash[index * 2U + 1U] = digits[digest[index] & 15U]; }
        hash[64] = 0; int path_bytes = snprintf(path, sizeof(path), "/assets/%s.vka", hash); size_t actual = 0U;
        uint8_t actual_digest[VK_ASSET_SHA256_BYTES];
        if (path_bytes <= 0 || (size_t)path_bytes >= sizeof(path) || store->config.fs->file_size(store->config.fs_context, path, &actual) != ESP_OK ||
            actual != bytes || vk_asset_sha256_file(store, path, actual_digest) != ESP_OK || memcmp(actual_digest, digest, 32U) != 0 ||
            store->config.validate_vka1(store->config.vka1_context, path, digest, bytes, kind) != ESP_OK) goto cleanup;
        catalog[used++] = (revision_asset_t){.bytes=bytes,.kind=kind}; memcpy(catalog[used - 1U].digest, digest, 32U);
    }
    uint16_t mode = vk_usb_json_object_find(screen_document, screen_root, "configured_mode");
    uint16_t image = vk_usb_json_object_find(screen_document, screen_root, "image");
    uint16_t layout = vk_usb_json_object_find(screen_document, screen_root, "layout");
    uint16_t pet = vk_usb_json_object_find(screen_document, screen_root, "pet");
    bool image_mode = vk_usb_json_string_equal(screen_document, mode, "image");
    bool pet_mode = vk_usb_json_string_equal(screen_document, mode, "pet");
    bool layout_mode = vk_usb_json_string_equal(screen_document, mode, "dashboard") ||
        vk_usb_json_string_equal(screen_document, mode, "custom");
    if ((!image_mode && !pet_mode && !layout_mode) ||
        image_mode != !vk_usb_json_is_null(screen_document, image) ||
        pet_mode != !vk_usb_json_is_null(screen_document, pet) ||
        layout_mode != !vk_usb_json_is_null(screen_document, layout) ||
        !revision_references_valid(screen_document, screen_root, catalog, used)) {
        goto cleanup;
    }
    result = ESP_OK;

cleanup:
    free(workspace);
    return result;
}

static esp_err_t resolve_asset(void *context, const char *sha, vk_screen_asset_info_t *info)
{
    (void)context;
    uint8_t expected[32];
    if (info == NULL || !parse_sha(sha, expected)) return ESP_ERR_INVALID_ARG;
    char path[VK_ASSET_PATH_BYTES];
    int count = snprintf(path, sizeof(path), "/assets/%s.vka", sha);
    size_t size = 0U;
    if (count <= 0 || (size_t)count >= sizeof(path) || s_fs.file_size(NULL, path, &size) != ESP_OK ||
        size == 0U || size > VK_SCREEN_SERVICE_MAX_ASSET_BYTES) return ESP_ERR_NOT_FOUND;
    uint8_t *asset_bytes = malloc(size);
    if (asset_bytes == NULL) return ESP_ERR_NO_MEM;
    esp_err_t result = read_exact(path, asset_bytes, size);
    if (result != ESP_OK) { free(asset_bytes); return result; }
    vk_vka1_info_t decoded;
    vk_vka1_limits_t limits = {
        .max_frames = VK_SCREEN_SERVICE_MAX_FRAMES,
        .min_frame_ms = VK_SCREEN_SERVICE_MIN_FRAME_MS,
        .max_frame_ms = VK_SCREEN_SERVICE_MAX_FRAME_MS,
        .max_container_bytes = VK_SCREEN_SERVICE_MAX_ASSET_BYTES,
        .max_decoded_bytes = VK_SCREEN_SERVICE_DECODE_BUDGET_BYTES,
    };
    if (vk_vka1_validate(asset_bytes, size, &limits, &decoded) != VK_VKA1_OK ||
        memcmp(decoded.sha256, expected, 32U) != 0) { free(asset_bytes); return ESP_ERR_INVALID_RESPONSE; }
    *info = (vk_screen_asset_info_t){
        .kind = decoded.kind,
        .width = decoded.width,
        .height = decoded.height,
        .frame_count = decoded.frame_count,
        .decoded_bytes_per_frame = decoded.decoded_bytes_per_frame,
        .container = asset_bytes,
        .container_bytes = size,
    };
    return ESP_OK;
}

static void release_asset(void *context, vk_screen_asset_info_t *info)
{
    (void)context;
    if (info != NULL) { free((void *)info->container); memset(info, 0, sizeof(*info)); }
}

static vk_usb_screen_capability_t screen_capability(void)
{
    vk_usb_screen_capability_t capability = {
        .state = VK_USB_CAPABILITY_AVAILABLE,
        .modes = VK_USB_SCREEN_MODE_IMAGE | VK_USB_SCREEN_MODE_PET |
                 VK_USB_SCREEN_MODE_DASHBOARD | VK_USB_SCREEN_MODE_CUSTOM,
        .max_commit_bytes = VK_USB_MAX_JSON_BYTES,
        .max_layout_bytes = 3072U,
        .max_assets = VK_SCREEN_SERVICE_MAX_ASSETS,
        .max_objects = VK_SCREEN_MAX_OBJECTS,
        .max_depth = 8U,
        .max_widgets = VK_SCREEN_MAX_WIDGETS,
        .max_fonts = 1U,
        .max_pet_states = VK_SCREEN_MAX_PET_STATES,
        .max_string_bytes = VK_SCREEN_MAX_TEXT_BYTES,
        .max_json_tokens = VK_USB_JSON_MAX_TOKENS,
        .max_widget_value_bytes = VK_SCREEN_MAX_TEXT_BYTES,
        .font_count = 1U,
    };
    snprintf(capability.fonts[0].id, sizeof(capability.fonts[0].id), "vk-sans");
    capability.fonts[0].version = 1U;
    snprintf(capability.fonts[0].metrics_sha256, sizeof(capability.fonts[0].metrics_sha256),
             "%s", VK_SCREEN_FONT_METRICS_SHA256);
    return capability;
}

esp_err_t vk_screen_product_prepare(void)
{
    esp_err_t result = show_acceptance_screen();
    if (result != ESP_OK) return result;

    result = vk_asset_spiffs_make_ops(&s_fs);
    if (result != ESP_OK) return result;
    vk_asset_store_config_t store_config = {
        .fs = &s_fs,
        .validate_vka1 = validate_vka1_file,
        .validate_revision = validate_revision,
        .revision_context = &s_store,
        .partition_offset = VK_SCREEN_SERVICE_STORAGE_OFFSET,
        .partition_size = VK_SCREEN_SERVICE_STORAGE_SIZE,
        .reserve_bytes = VK_SCREEN_SERVICE_RESERVE_BYTES,
        .max_asset_bytes = VK_SCREEN_SERVICE_MAX_ASSET_BYTES,
        .max_assets = VK_SCREEN_SERVICE_MAX_ASSETS,
    };
    result = vk_asset_store_init(&s_store, &store_config);
    if (result != ESP_OK) return result;
    result = vk_asset_store_mount(&s_store);
    if (result != ESP_OK) {
        vk_asset_format_token_t token;
        result = vk_asset_store_authorize_format(&s_store, 1U, 1U, &token);
        if (result != ESP_OK) return result;
        result = vk_asset_store_format(
            &s_store, 1U, &token, VK_ASSET_FORMAT_CONFIRMATION
        );
        if (result != ESP_OK) return result;
        result = vk_asset_store_mount(&s_store);
        if (result != ESP_OK) return result;
    }
    s_mounted = true;
    vk_asset_recovery_t recovery = {0};
    esp_err_t recovery_result = vk_asset_store_recover(&s_store, &recovery);
    bool recovered = recovery_result == ESP_OK ||
                     (recovery_result == ESP_ERR_NOT_FOUND && vk_asset_store_state(&s_store) == VK_ASSET_STORE_READY);
    vk_asset_transfer_config_t transfer_config = {
        .store = &s_store,
        .now_ms = product_now_ms,
        .chunk_bytes = VK_SCREEN_SERVICE_CHUNK_BYTES,
        .max_frames = VK_SCREEN_SERVICE_MAX_FRAMES,
        .min_frame_ms = VK_SCREEN_SERVICE_MIN_FRAME_MS,
        .max_frame_ms = VK_SCREEN_SERVICE_MAX_FRAME_MS,
        .max_active_decoded_bytes = VK_SCREEN_SERVICE_DECODE_BUDGET_BYTES,
        .decoder_scratch_bytes = VK_SCREEN_SERVICE_DECODER_SCRATCH_BYTES,
        .production_available = VK_SCREEN_SERVICE_PHYSICAL_ACCEPTANCE_ADMITTED != 0,
    };
    result = vk_asset_transfer_init(&s_transfer, &transfer_config);
    if (result != ESP_OK) goto fail;
    vk_screen_renderer_ops_t renderer;
    vk_screen_asset_resolver_t asset_resolver = {.resolve = resolve_asset, .release = release_asset};
    s_product_font = lv_font_montserrat_14;
    lv_font_set_kerning(&s_product_font, LV_FONT_KERNING_NONE);
    result = vk_screen_lvgl_make_renderer(&renderer, &asset_resolver, &s_product_font);
    if (result != ESP_OK) goto fail;
    vk_screen_config_t screen_config = {
        .capability = screen_capability(),
        .assets = asset_resolver,
        .renderer = renderer,
        .root_budget_bytes = VK_SCREEN_SERVICE_DECODE_BUDGET_BYTES,
        .decoder_scratch_bytes = VK_SCREEN_SERVICE_DECODER_SCRATCH_BYTES,
        .production_profile_admitted = true,
    };
    result = vk_screen_init(&s_screen, &screen_config);
    if (result != ESP_OK) goto fail;
    s_screen_initialized = true;
    vk_display_dependencies_t dependencies = {
        .store_ready = recovered,
        .screen_owner_ready = true,
        .font_profile_ready = true,
        .physical_acceptance_admitted = VK_SCREEN_SERVICE_PHYSICAL_ACCEPTANCE_ADMITTED != 0,
    };
    result = vk_display_product_set_dependencies(&dependencies);
    if (result != ESP_OK) goto fail;
    vk_screen_service_readiness_t readiness = {
        .mounted = true,
        .recovered = recovered,
        .transfer_ready = true,
        .screen_ready = true,
        .display_ready = vk_display_transport_ready(vk_display_product_instance()),
        .font_ready = true,
        .physical_acceptance_admitted = VK_SCREEN_SERVICE_PHYSICAL_ACCEPTANCE_ADMITTED != 0,
    };
    result = vk_screen_service_init(&s_service, &s_store, &s_transfer, &s_screen,
                                    vk_display_product_instance(), &readiness);
    if (result == ESP_OK && recovery.has_current) {
        /* A failed candidate and its previous revision are both checked by the
         * service. Failure leaves the safe empty root and capability unavailable. */
        result = vk_screen_service_restore(&s_service, &recovery, &s_restore_document,
                                           s_restore_envelope, sizeof(s_restore_envelope));
        if (result != ESP_OK) {
            s_service.store_recovered = false;
            result = ESP_OK;
        }
    }
    if (result == ESP_OK) {
        tick_timer_ensure();
        return ESP_OK;
    }
fail:
    (void)vk_screen_product_stop();
    return result;
}

void vk_screen_product_registrations(vk_usb_capability_provider_registration_t *capabilities,
                                     vk_usb_asset_handler_registration_t *assets,
                                     vk_usb_screen_handler_registration_t *screen,
                                     vk_usb_widget_handler_registration_t *widget)
{
    vk_screen_service_registrations(&s_service, capabilities, assets, screen, widget);
}

esp_err_t vk_screen_product_stop(void)
{
    esp_err_t first = ESP_OK;
    tick_timer_stop();
    if (s_screen_initialized) {
        esp_err_t result = vk_screen_stop(&s_screen);
        if (result != ESP_OK) first = result;
        else s_screen_initialized = false;
    }
    if (s_mounted) {
        esp_err_t result = vk_asset_store_unmount(&s_store);
        if (result != ESP_OK && first == ESP_OK) first = result;
        else if (result == ESP_OK) s_mounted = false;
    }
    memset(&s_service, 0, sizeof(s_service));
    return first;
}

#endif
