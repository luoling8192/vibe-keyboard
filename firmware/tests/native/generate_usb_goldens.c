#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_usb.h"

typedef struct {
    uint8_t input[256];
    size_t input_count;
    size_t input_offset;
    uint8_t output[2048];
    size_t output_count;
    uint64_t now_ms;
} fixture_t;

static esp_err_t install(void *context) { (void)context; return ESP_OK; }
static esp_err_t uninstall(void *context) { (void)context; return ESP_OK; }
static int read_bytes(void *context, uint8_t *bytes, size_t capacity, uint32_t timeout_ms) {
    (void)timeout_ms;
    fixture_t *fixture = context;
    size_t count = fixture->input_count - fixture->input_offset;
    if (count > capacity) count = capacity;
    memcpy(bytes, fixture->input + fixture->input_offset, count);
    fixture->input_offset += count;
    return (int)count;
}
static int write_bytes(void *context, const uint8_t *bytes, size_t length, uint32_t timeout_ms) {
    (void)timeout_ms;
    fixture_t *fixture = context;
    assert(fixture->output_count + length <= sizeof(fixture->output));
    memcpy(fixture->output + fixture->output_count, bytes, length);
    fixture->output_count += length;
    return (int)length;
}
static uint64_t now_ms(void *context) { return ((fixture_t *)context)->now_ms; }
static void append_frame(fixture_t *fixture, const char *json) {
    size_t length = strlen(json);
    uint8_t header[] = {1, VK_USB_FRAME_TYPE_JSON, (uint8_t)length, (uint8_t)(length >> 8)};
    memcpy(fixture->input + fixture->input_count, header, sizeof(header));
    fixture->input_count += sizeof(header);
    memcpy(fixture->input + fixture->input_count, json, length);
    fixture->input_count += length;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    fixture_t fixture = {0};
    vk_usb_service_t *service = calloc(1, vk_usb_service_size());
    assert(service != NULL);
    vk_usb_transport_ops_t transport = {install, uninstall, read_bytes, write_bytes, now_ms, NULL, NULL, &fixture};
    vk_usb_identity_t identity = {"vibe_keyboard", "test", "VS-TEST"};
    vk_usb_service_policy_t policy = {.update_boot_policy_enabled = false};
    assert(vk_usb_service_init(service, &transport, &identity, &policy) == ESP_OK);
    assert(vk_usb_service_start(service) == ESP_OK);
    append_frame(&fixture, "{\"event\":\"transport\",\"kind\":\"usb\"}");
    assert(vk_usb_service_poll(service) == ESP_OK);
    fixture.input_count = fixture.input_offset = 0;
    append_frame(&fixture, "{\"event\":\"get_device_info\"}");
    assert(vk_usb_service_poll(service) == ESP_OK);
    FILE *output = fopen(argv[1], "wb");
    assert(output != NULL);
    assert(fwrite(fixture.output, 1, fixture.output_count, output) == fixture.output_count);
    assert(fclose(output) == 0);
    assert(vk_usb_service_stop(service) == ESP_OK);
    free(service);
    return 0;
}
