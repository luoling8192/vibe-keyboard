#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_audio_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_AUDIO_BACKEND_RING_BYTES 64000U
#define VK_AUDIO_BACKEND_READ_BYTES 4096U

typedef struct {
    const int16_t *data;
    int data_bytes;
    int result;
} vk_audio_backend_fetch_t;

typedef struct {
    esp_err_t (*i2s_create)(void *context, void **handle);
    esp_err_t (*i2s_initialize)(void *context, void *handle);
    esp_err_t (*i2s_enable)(void *context, void *handle);
    esp_err_t (*i2s_read)(void *context, void *handle, uint8_t *bytes,
                          size_t capacity, size_t *read_bytes);
    esp_err_t (*i2s_disable)(void *context, void *handle);
    esp_err_t (*i2s_destroy)(void *context, void *handle);
    esp_err_t (*afe_create)(void *context, void **handle, size_t *feed_bytes,
                            size_t *fetch_bytes);
    int (*afe_feed)(void *context, void *handle, const int16_t *samples);
    bool (*afe_fetch)(void *context, void *handle, vk_audio_backend_fetch_t *result);
    void (*afe_destroy)(void *context, void *handle);
    esp_err_t (*opus_create)(void *context, void **handle);
    int (*opus_encode)(void *context, void *handle, const int16_t *samples,
                       size_t count, uint8_t *packet, size_t capacity);
    void (*opus_destroy)(void *context, void *handle);
    void *(*allocate)(void *context, size_t bytes, bool external);
    void (*deallocate)(void *context, void *pointer);
    esp_err_t (*send)(void *context, uint32_t session, uint32_t sequence,
                      uint8_t flags, const uint8_t *payload, uint16_t length);
    void *context;
} vk_audio_backend_ops_t;

typedef struct {
    vk_audio_backend_ops_t ops;
    uint8_t *ring;
    size_t ring_head;
    size_t ring_count;
    int16_t *feed;
    size_t feed_bytes;
    size_t fetch_bytes;
    void *i2s;
    bool i2s_enabled;
    void *afe;
    void *opus;
    vk_audio_pipeline_t pipeline;
} vk_audio_backend_t;

esp_err_t vk_audio_backend_acquire(vk_audio_backend_t *backend,
                                    const vk_audio_backend_ops_t *ops,
                                    uint32_t session_id);
esp_err_t vk_audio_backend_capture(vk_audio_backend_t *backend);
esp_err_t vk_audio_backend_finish(vk_audio_backend_t *backend);
esp_err_t vk_audio_backend_release(vk_audio_backend_t *backend);

#ifdef __cplusplus
}
#endif
