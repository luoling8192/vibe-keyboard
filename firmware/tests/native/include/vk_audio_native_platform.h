#pragma once

#include <stdbool.h>
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
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define AFE_TYPE_SR 0
#define AFE_MODE_HIGH_PERF 0
#define AFE_MEMORY_ALLOC_MORE_PSRAM 0
#define OPUS_OK 0
#define OPUS_BAD_ARG (-1)
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SIGNAL_VOICE 3001
#define OPUS_SET_VBR(x) (1000 + (x))
#define OPUS_SET_BITRATE(x) (2000 + (x))
#define OPUS_SET_DTX(x) (3000 + (x))
#define OPUS_SET_COMPLEXITY(x) (4000 + (x))
#define OPUS_SET_SIGNAL(x) (5000 + (x))
#define pdMS_TO_TICKS(x) (x)
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) ((i2s_pdm_rx_clk_config_t){0})
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(width, mode) ((i2s_pdm_rx_slot_config_t){0})

typedef void *i2s_chan_handle_t;
typedef struct { uint32_t dma_desc_num; uint32_t dma_frame_num; } i2s_chan_config_t;
typedef struct { int unused; } i2s_pdm_rx_clk_config_t;
typedef struct { int unused; } i2s_pdm_rx_slot_config_t;
typedef struct { int clk; int din; struct { int clk_inv; } invert_flags; } i2s_pdm_rx_gpio_config_t;
typedef struct { i2s_pdm_rx_clk_config_t clk_cfg; i2s_pdm_rx_slot_config_t slot_cfg; i2s_pdm_rx_gpio_config_t gpio_cfg; } i2s_pdm_rx_config_t;

typedef struct afe_config {
    bool aec_init, se_init, vad_init, wakenet_init, agc_init, debug_init;
    int memory_alloc_mode, afe_perferred_core, afe_perferred_priority, afe_ringbuf_size;
    float afe_linear_gain;
} afe_config_t;
typedef void esp_afe_sr_data_t;
typedef struct { const int16_t *data; int data_size; int ret_value; } afe_fetch_result_t;
typedef struct esp_afe_sr_iface {
    esp_afe_sr_data_t *(*create_from_config)(afe_config_t *);
    void (*destroy)(esp_afe_sr_data_t *);
    int (*feed)(esp_afe_sr_data_t *, const int16_t *);
    afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *, uint32_t);
    int (*get_feed_chunksize)(esp_afe_sr_data_t *);
    int (*get_fetch_chunksize)(esp_afe_sr_data_t *);
    int (*get_feed_channel_num)(esp_afe_sr_data_t *);
    int (*get_fetch_channel_num)(esp_afe_sr_data_t *);
    int (*get_samp_rate)(esp_afe_sr_data_t *);
} esp_afe_sr_iface_t;
typedef void OpusEncoder;
typedef int opus_int32;

void *heap_caps_malloc(size_t, uint32_t);
void *heap_caps_calloc(size_t, size_t, uint32_t);
esp_err_t i2s_new_channel(const i2s_chan_config_t *, void *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t, const i2s_pdm_rx_config_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
afe_config_t *afe_config_init(const char *, void *, int, int);
void afe_config_free(afe_config_t *);
esp_afe_sr_iface_t *esp_afe_handle_from_config(afe_config_t *);
OpusEncoder *opus_encoder_create(int, int, int, int *);
int opus_encoder_ctl(OpusEncoder *, int);
int opus_encode(OpusEncoder *, const int16_t *, int, uint8_t *, opus_int32);
void opus_encoder_destroy(OpusEncoder *);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, uint32_t, TaskHandle_t *, int);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void xTaskNotifyGive(TaskHandle_t);
void vTaskDelete(TaskHandle_t);

typedef enum {
    VK_AUDIO_NATIVE_RESOURCE_SESSION = 0,
    VK_AUDIO_NATIVE_RESOURCE_RING,
    VK_AUDIO_NATIVE_RESOURCE_FEED,
    VK_AUDIO_NATIVE_RESOURCE_AFE_CONFIG,
    VK_AUDIO_NATIVE_RESOURCE_AFE_DATA,
    VK_AUDIO_NATIVE_RESOURCE_OPUS,
    VK_AUDIO_NATIVE_RESOURCE_I2S,
    VK_AUDIO_NATIVE_RESOURCE_WORKER,
    VK_AUDIO_NATIVE_RESOURCE_STARTED_SEMAPHORE,
    VK_AUDIO_NATIVE_RESOURCE_STOPPED_SEMAPHORE,
    VK_AUDIO_NATIVE_RESOURCE_COUNT,
} vk_audio_native_resource_t;
void vk_audio_native_resource_allocated(vk_audio_native_resource_t resource);
void vk_audio_native_resource_freed(vk_audio_native_resource_t resource);
void vk_audio_native_stop_requested(void);
