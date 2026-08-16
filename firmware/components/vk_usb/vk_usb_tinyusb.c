#include "vk_usb_tinyusb.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_device_uac.h"
#include "vk_usb_descriptors.h"

#define VK_USB_TINYUSB_TASK_STACK_BYTES 4096U
#define VK_USB_TINYUSB_TASK_PRIORITY 5U
#define VK_USB_UAC_MONITOR_STACK_BYTES 3072U
#define VK_USB_UAC_MONITOR_PRIORITY 4U
#define VK_USB_UAC_IDLE_MS 100U

typedef struct {
    SemaphoreHandle_t mutex;
    usb_phy_handle_t phy;
    TaskHandle_t tinyusb_task;
    TaskHandle_t monitor_task;
    vk_usb_uac_source_registration_t source;
    bool initialized;
    bool source_started;
    int64_t last_uac_read_us;
} vk_usb_tinyusb_context_t;

static vk_usb_tinyusb_context_t s_tinyusb;

static void tinyusb_task(void *argument)
{
    (void)argument;
    for (;;) tud_task();
}

static void stop_source_locked(void)
{
    if (!s_tinyusb.source_started) return;
    s_tinyusb.source_started = false;
    s_tinyusb.source.stop(s_tinyusb.source.context);
}

static void uac_monitor_task(void *argument)
{
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25U));
        xSemaphoreTake(s_tinyusb.mutex, portMAX_DELAY);
        int64_t idle_us = esp_timer_get_time() - s_tinyusb.last_uac_read_us;
        if (s_tinyusb.source_started &&
            idle_us >= (int64_t)VK_USB_UAC_IDLE_MS * 1000) {
            stop_source_locked();
        }
        xSemaphoreGive(s_tinyusb.mutex);
    }
}

static esp_err_t uac_input(uint8_t *bytes, size_t length, size_t *read_bytes,
                           void *context)
{
    (void)context;
    if (bytes == NULL || read_bytes == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_tinyusb.mutex, portMAX_DELAY);
    s_tinyusb.last_uac_read_us = esp_timer_get_time();
    esp_err_t error = ESP_OK;
    if (!s_tinyusb.source_started) {
        error = s_tinyusb.source.start(s_tinyusb.source.context);
        if (error == ESP_OK) s_tinyusb.source_started = true;
    }
    if (error == ESP_OK) {
        error = s_tinyusb.source.read(s_tinyusb.source.context, bytes, length,
                                      read_bytes);
    }
    if (error != ESP_OK || *read_bytes != length) {
        stop_source_locked();
        memset(bytes, 0, length);
        *read_bytes = length;
        error = ESP_OK;
    }
    xSemaphoreGive(s_tinyusb.mutex);
    return error;
}

static esp_err_t initialize_once(void)
{
    uint8_t mac[6];
    esp_err_t error = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (error != ESP_OK) return error;
    char serial[16];
    int count = snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (count != 12) return ESP_ERR_INVALID_SIZE;
    vk_usb_descriptors_set_serial(serial);

    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
    };
    error = usb_new_phy(&phy_config, &s_tinyusb.phy);
    if (error != ESP_OK) return error;
    if (!tusb_init()) {
        (void)usb_del_phy(s_tinyusb.phy);
        s_tinyusb.phy = NULL;
        return ESP_FAIL;
    }

    uac_device_config_t uac_config = {
        .skip_tinyusb_init = true,
        .input_cb = uac_input,
        .cb_ctx = NULL,
        .spk_itf_num = -1,
        .mic_itf_num = VK_USB_AUDIO_MIC_INTERFACE,
    };
    error = uac_device_init(&uac_config);
    if (error != ESP_OK) return error;
    BaseType_t created = xTaskCreatePinnedToCore(
        uac_monitor_task, "vk_uac_monitor", VK_USB_UAC_MONITOR_STACK_BYTES,
        NULL, VK_USB_UAC_MONITOR_PRIORITY, &s_tinyusb.monitor_task,
        tskNO_AFFINITY);
    if (created != pdPASS) return ESP_ERR_NO_MEM;
    created = xTaskCreatePinnedToCore(
        tinyusb_task, "vk_tinyusb", VK_USB_TINYUSB_TASK_STACK_BYTES, NULL,
        VK_USB_TINYUSB_TASK_PRIORITY, &s_tinyusb.tinyusb_task, tskNO_AFFINITY);
    if (created != pdPASS) return ESP_ERR_NO_MEM;
    s_tinyusb.initialized = true;
    return ESP_OK;
}

esp_err_t vk_usb_tinyusb_connect(
    const vk_usb_uac_source_registration_t *source)
{
    if (source == NULL || source->start == NULL || source->read == NULL ||
        source->stop == NULL) return ESP_ERR_INVALID_ARG;
    if (s_tinyusb.mutex == NULL) {
        s_tinyusb.mutex = xSemaphoreCreateMutex();
        if (s_tinyusb.mutex == NULL) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_tinyusb.mutex, portMAX_DELAY);
    s_tinyusb.source = *source;
    esp_err_t error = s_tinyusb.initialized ? ESP_OK : initialize_once();
    if (error == ESP_OK) tud_connect();
    xSemaphoreGive(s_tinyusb.mutex);
    return error;
}

esp_err_t vk_usb_tinyusb_disconnect(void)
{
    if (s_tinyusb.mutex == NULL || !s_tinyusb.initialized) return ESP_OK;
    xSemaphoreTake(s_tinyusb.mutex, portMAX_DELAY);
    tud_disconnect();
    stop_source_locked();
    xSemaphoreGive(s_tinyusb.mutex);
    return ESP_OK;
}

int vk_usb_tinyusb_read(uint8_t *bytes, size_t capacity, uint32_t timeout_ms)
{
    if (bytes == NULL || capacity == 0U || capacity > UINT32_MAX) return -1;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        uint32_t available = tud_cdc_n_available(0);
        if (available != 0U) {
            uint32_t requested = available < capacity ? available : (uint32_t)capacity;
            return (int)tud_cdc_n_read(0, bytes, requested);
        }
        if (timeout_ms == 0U) break;
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while (esp_timer_get_time() < deadline);
    return 0;
}

int vk_usb_tinyusb_write(const uint8_t *bytes, size_t length,
                         uint32_t timeout_ms)
{
    if (bytes == NULL || length == 0U || length > UINT32_MAX) return -1;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t offset = 0U;
    do {
        uint32_t available = tud_cdc_n_write_available(0);
        if (available != 0U) {
            size_t remaining = length - offset;
            uint32_t requested = remaining < available ? (uint32_t)remaining : available;
            uint32_t written = tud_cdc_n_write(0, bytes + offset, requested);
            offset += written;
            tud_cdc_n_write_flush(0);
            if (offset == length) return (int)offset;
        }
        if (timeout_ms == 0U) break;
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while (esp_timer_get_time() < deadline);
    return offset == 0U ? -1 : (int)offset;
}
