#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_AUDIO_SAMPLE_RATE_HZ 16000U
#define VK_AUDIO_OPUS_FRAME_SAMPLES 960U
#define VK_AUDIO_OPUS_MAX_PACKET_BYTES 220U
#define VK_AUDIO_FIRST_FLAG 0x01U
#define VK_AUDIO_FINAL_FLAG 0x02U

typedef struct {
    int (*encode)(void *context, const int16_t *samples, size_t sample_count,
                  uint8_t *packet, size_t packet_capacity);
    esp_err_t (*send)(void *context, uint32_t session_id, uint32_t sequence,
                      uint8_t flags, const uint8_t *payload, uint16_t payload_length);
    void *context;
} vk_audio_pipeline_ops_t;

typedef struct {
    vk_audio_pipeline_ops_t ops;
    uint32_t session_id;
    uint32_t sequence;
    size_t pcm_count;
    int16_t pcm[VK_AUDIO_OPUS_FRAME_SAMPLES];
    bool started;
    bool finalized;
    bool failed;
} vk_audio_pipeline_t;

esp_err_t vk_audio_pipeline_init(vk_audio_pipeline_t *pipeline,
                                 const vk_audio_pipeline_ops_t *ops,
                                 uint32_t session_id);
esp_err_t vk_audio_pipeline_push(vk_audio_pipeline_t *pipeline,
                                 const int16_t *samples, size_t sample_count);
esp_err_t vk_audio_pipeline_finish(vk_audio_pipeline_t *pipeline);
uint32_t vk_audio_next_session(uint32_t current);

#ifdef __cplusplus
}
#endif
