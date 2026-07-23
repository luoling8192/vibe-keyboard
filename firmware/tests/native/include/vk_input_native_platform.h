#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef int gpio_num_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(value) (value)
#define VK_KEY_COUNT 4U
#define VK_KEY_SCAN_PERIOD_MS 5U
#define VK_KEY_ACTIVE_LEVEL 0

extern const gpio_num_t vk_key_gpios[VK_KEY_COUNT];

QueueHandle_t xQueueCreate(size_t length, size_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
BaseType_t xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, uint32_t stack,
                                  void *argument, uint32_t priority, TaskHandle_t *handle,
                                  int core);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *wake, TickType_t increment);
void vTaskDelay(TickType_t delay);
int gpio_get_level(gpio_num_t gpio);
int64_t esp_timer_get_time(void);
