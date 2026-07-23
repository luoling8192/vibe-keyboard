#include "vk_audio_pipeline.h"

#include <string.h>

uint32_t vk_audio_next_session(uint32_t current)
{
    ++current;
    return current == 0U ? 1U : current;
}

esp_err_t vk_audio_pipeline_init(vk_audio_pipeline_t *pipeline,
                                 const vk_audio_pipeline_ops_t *ops,
                                 uint32_t session_id)
{
    if (pipeline == NULL || ops == NULL || ops->encode == NULL ||
        ops->send == NULL || session_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->ops = *ops;
    pipeline->session_id = session_id;
    pipeline->started = true;
    return ESP_OK;
}

static esp_err_t encode_and_send(vk_audio_pipeline_t *pipeline)
{
    uint8_t packet[VK_AUDIO_OPUS_MAX_PACKET_BYTES];
    int packet_bytes = pipeline->ops.encode(pipeline->ops.context, pipeline->pcm,
                                            VK_AUDIO_OPUS_FRAME_SAMPLES, packet,
                                            sizeof(packet));
    if (packet_bytes <= 0 || packet_bytes > (int)sizeof(packet)) {
        pipeline->failed = true;
        return packet_bytes < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
    }

    if (pipeline->sequence == UINT32_MAX) {
        pipeline->failed = true;
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t flags = pipeline->sequence == 0U ? VK_AUDIO_FIRST_FLAG : 0U;
    esp_err_t error = pipeline->ops.send(
        pipeline->ops.context, pipeline->session_id, pipeline->sequence, flags,
        packet, (uint16_t)packet_bytes);
    if (error != ESP_OK) {
        pipeline->failed = true;
        return error;
    }
    /* Sequence represents successful ownership transfer to the bounded USB queue. */
    ++pipeline->sequence;
    pipeline->pcm_count = 0U;
    return ESP_OK;
}

esp_err_t vk_audio_pipeline_push(vk_audio_pipeline_t *pipeline,
                                 const int16_t *samples, size_t sample_count)
{
    if (pipeline == NULL || (samples == NULL && sample_count != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pipeline->started || pipeline->finalized || pipeline->failed) {
        return ESP_ERR_INVALID_STATE;
    }

    while (sample_count != 0U) {
        size_t room = VK_AUDIO_OPUS_FRAME_SAMPLES - pipeline->pcm_count;
        size_t copy = sample_count < room ? sample_count : room;
        memcpy(pipeline->pcm + pipeline->pcm_count, samples, copy * sizeof(*samples));
        pipeline->pcm_count += copy;
        samples += copy;
        sample_count -= copy;
        if (pipeline->pcm_count == VK_AUDIO_OPUS_FRAME_SAMPLES) {
            esp_err_t error = encode_and_send(pipeline);
            if (error != ESP_OK) return error;
        }
    }
    return ESP_OK;
}

esp_err_t vk_audio_pipeline_finish(vk_audio_pipeline_t *pipeline)
{
    if (pipeline == NULL) return ESP_ERR_INVALID_ARG;
    if (!pipeline->started || pipeline->finalized) return ESP_ERR_INVALID_STATE;
    if (pipeline->failed) return ESP_ERR_INVALID_STATE;

    esp_err_t error = pipeline->ops.send(pipeline->ops.context,
                                         pipeline->session_id,
                                         pipeline->sequence,
                                         VK_AUDIO_FINAL_FLAG,
                                         NULL, 0U);
    if (error != ESP_OK) {
        pipeline->failed = true;
        return error;
    }
    pipeline->finalized = true;
    pipeline->pcm_count = 0U;
    return ESP_OK;
}
