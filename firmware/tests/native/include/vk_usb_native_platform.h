#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY (-1)
#define ESP_MAC_EFUSE_FACTORY 0
#define pdMS_TO_TICKS(x) (x)
typedef struct { uint32_t tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
typedef struct { char version[32]; } esp_app_desc_t;
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *);
esp_err_t usb_serial_jtag_driver_uninstall(void);
int usb_serial_jtag_read_bytes(void *, uint32_t, TickType_t);
int usb_serial_jtag_write_bytes(const void *, size_t, TickType_t);
int64_t esp_timer_get_time(void);
esp_err_t esp_read_mac(uint8_t *, int);
const esp_app_desc_t *esp_app_get_description(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, uint32_t, TaskHandle_t *, int);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void xTaskNotifyGive(TaskHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
