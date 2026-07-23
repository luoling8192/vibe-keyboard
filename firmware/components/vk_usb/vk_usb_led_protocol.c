#include "vk_usb_led_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint16_t member(const vk_usb_json_document_t *document, uint16_t root, const char *key)
{
    return vk_usb_json_object_find(document, root, key);
}

static bool exact(const vk_usb_json_document_t *document, uint16_t root,
                  const char *const *keys, size_t count)
{
    return vk_usb_json_object_exact_keys(document, root, keys, count);
}

esp_err_t vk_usb_led_command_decode(const vk_usb_json_document_t *document, uint16_t root,
                                    const vk_usb_led_capability_t *capability,
                                    vk_usb_led_command_t *command)
{
    if (document == NULL || capability == NULL || command == NULL ||
        vk_usb_json_kind(document, root) != VK_USB_JSON_OBJECT) return ESP_ERR_INVALID_ARG;
    memset(command, 0, sizeof(*command));
    uint16_t event = member(document, root, "event");
    uint16_t request = member(document, root, "request_id");
    if (event == VK_USB_JSON_NO_NODE || request == VK_USB_JSON_NO_NODE ||
        vk_usb_json_uint32(document, request, UINT32_MAX, true, &command->request_id) != VK_USB_JSON_OK)
        return ESP_ERR_INVALID_ARG;
    if (vk_usb_json_string_equal(document, event, "vk_led_query")) {
        const char *const keys[] = {"event", "request_id"};
        if (!exact(document, root, keys, 2U)) return ESP_ERR_INVALID_ARG;
        command->kind = VK_USB_LED_QUERY;
        return ESP_OK;
    }
    if (!vk_usb_json_string_equal(document, event, "vk_led_config")) return ESP_ERR_INVALID_ARG;
    const char *const keys[] = {"event", "request_id", "enabled", "brightness"};
    uint16_t enabled = member(document, root, "enabled");
    uint16_t brightness = member(document, root, "brightness");
    uint32_t value = 0U;
    if (!exact(document, root, keys, 4U) || enabled == VK_USB_JSON_NO_NODE ||
        brightness == VK_USB_JSON_NO_NODE || !vk_usb_json_boolean(document, enabled, &command->enabled) ||
        vk_usb_json_uint32(document, brightness, 255U, false, &value) != VK_USB_JSON_OK)
        return ESP_ERR_INVALID_ARG;
    command->kind = VK_USB_LED_CONFIG;
    command->brightness = (uint8_t)value;
    if (capability->state != VK_USB_CAPABILITY_AVAILABLE) return ESP_ERR_NOT_SUPPORTED;
    if (command->brightness > capability->max_brightness) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static const char *reason(vk_usb_led_unavailable_reason_t value)
{
    static const char *const values[] = {"calibration_required", "hardware_failed", "tainted"};
    return value <= VK_USB_LED_REASON_TAINTED ? values[value] : NULL;
}
static const char *effective(vk_usb_led_effective_t value)
{
    static const char *const values[] = {"off", "connected", "recording", "mutation"};
    return value <= VK_USB_LED_EFFECTIVE_MUTATION ? values[value] : NULL;
}

esp_err_t vk_usb_led_state_encode(const vk_usb_led_state_event_t *event,
                                  char *output, size_t capacity, size_t *output_length)
{
    if (event == NULL || output == NULL || output_length == NULL || capacity == 0U ||
        event->request_id == 0U || event->source > VK_USB_LED_STATE_APPLIED) return ESP_ERR_INVALID_ARG;
    const char *source = event->source == VK_USB_LED_STATE_QUERY ? "query" : "applied";
    int written;
    if (!event->available) {
        const char *why = reason(event->unavailable_reason);
        if (why == NULL) return ESP_ERR_INVALID_ARG;
        written = snprintf(output, capacity,
            "{\"available\":false,\"event\":\"vk_led_state\",\"reason\":\"%s\",\"request_id\":%" PRIu32 ",\"source\":\"%s\"}",
            why, event->request_id, source);
    } else {
        const char *state = effective(event->effective);
        if (state == NULL || ((!event->enabled || event->brightness == 0U) &&
                             event->effective != VK_USB_LED_EFFECTIVE_OFF)) return ESP_ERR_INVALID_ARG;
        written = snprintf(output, capacity,
            "{\"available\":true,\"brightness\":%u,\"effective\":\"%s\",\"enabled\":%s,\"event\":\"vk_led_state\",\"request_id\":%" PRIu32 ",\"source\":\"%s\"}",
            (unsigned)event->brightness, state, event->enabled ? "true" : "false",
            event->request_id, source);
    }
    if (written < 0 || (size_t)written >= capacity) return ESP_ERR_INVALID_SIZE;
    *output_length = (size_t)written;
    return ESP_OK;
}

esp_err_t vk_usb_led_error_encode(const vk_usb_led_error_event_t *event,
                                  char *output, size_t capacity, size_t *output_length)
{
    static const char *const codes[] = {"invalid_request", "wrong_epoch", "unavailable", "busy",
        "queue_overflow", "hardware_failed", "tainted"};
    if (event == NULL || output == NULL || output_length == NULL || capacity == 0U ||
        event->code > VK_USB_LED_ERROR_TAINTED) return ESP_ERR_INVALID_ARG;
    if (event->message != NULL && strnlen(event->message, VK_USB_PROTOCOL_MESSAGE_MAX_BYTES + 1U) >
        VK_USB_PROTOCOL_MESSAGE_MAX_BYTES) return ESP_ERR_INVALID_ARG;
    int written;
    if (event->request_id != 0U && event->message != NULL)
        written = snprintf(output, capacity,
            "{\"code\":\"%s\",\"event\":\"vk_error\",\"message\":\"%s\",\"operation\":\"led\",\"request_id\":%" PRIu32 "}",
            codes[event->code], event->message, event->request_id);
    else if (event->request_id != 0U)
        written = snprintf(output, capacity,
            "{\"code\":\"%s\",\"event\":\"vk_error\",\"operation\":\"led\",\"request_id\":%" PRIu32 "}",
            codes[event->code], event->request_id);
    else
        written = snprintf(output, capacity,
            "{\"code\":\"%s\",\"event\":\"vk_error\",\"operation\":\"led\"}", codes[event->code]);
    if (written < 0 || (size_t)written >= capacity) return ESP_ERR_INVALID_SIZE;
    *output_length = (size_t)written;
    return ESP_OK;
}
