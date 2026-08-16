#include "vk_audio_backend.h"

#include <string.h>

static bool valid(const vk_audio_backend_ops_t *ops, bool stream_opus)
{
    if (ops == NULL) return false;
    bool capture = ops->i2s_create != NULL && ops->i2s_initialize != NULL &&
           ops->i2s_enable != NULL && ops->i2s_read != NULL &&
           ops->i2s_disable != NULL && ops->i2s_destroy != NULL;
    bool uac = stream_opus || ops->publish_pcm != NULL;
    bool opus = !stream_opus || (ops->afe_create != NULL &&
           ops->afe_feed != NULL && ops->afe_fetch != NULL &&
           ops->afe_destroy != NULL && ops->allocate != NULL &&
           ops->deallocate != NULL && ops->opus_create != NULL &&
           ops->opus_encode != NULL && ops->opus_destroy != NULL &&
           ops->send != NULL);
    return capture && uac && opus;
}

static int encode(void *context, const int16_t *samples, size_t count,
                  uint8_t *packet, size_t capacity)
{
    vk_audio_backend_t *backend = context;
    return backend->ops.opus_encode(backend->ops.context, backend->opus,
                                    samples, count, packet, capacity);
}

static esp_err_t send_frame(void *context, uint32_t session, uint32_t sequence,
                            uint8_t flags, const uint8_t *payload, uint16_t length)
{
    vk_audio_backend_t *backend = context;
    return backend->ops.send(backend->ops.context, session, sequence, flags,
                             payload, length);
}

static esp_err_t ring_write(vk_audio_backend_t *backend, const uint8_t *bytes,
                            size_t length)
{
    if (length > VK_AUDIO_BACKEND_RING_BYTES - backend->ring_count) return ESP_ERR_NO_MEM;
    size_t tail = (backend->ring_head + backend->ring_count) % VK_AUDIO_BACKEND_RING_BYTES;
    size_t first = VK_AUDIO_BACKEND_RING_BYTES - tail;
    if (first > length) first = length;
    memcpy(backend->ring + tail, bytes, first);
    memcpy(backend->ring, bytes + first, length - first);
    backend->ring_count += length;
    return ESP_OK;
}

static void ring_read(vk_audio_backend_t *backend, uint8_t *bytes, size_t length)
{
    size_t first = VK_AUDIO_BACKEND_RING_BYTES - backend->ring_head;
    if (first > length) first = length;
    memcpy(bytes, backend->ring + backend->ring_head, first);
    memcpy(bytes + first, backend->ring, length - first);
    backend->ring_head = (backend->ring_head + length) % VK_AUDIO_BACKEND_RING_BYTES;
    backend->ring_count -= length;
}

static esp_err_t drain(vk_audio_backend_t *backend)
{
    for (;;) {
        vk_audio_backend_fetch_t result = {0};
        if (!backend->ops.afe_fetch(backend->ops.context, backend->afe, &result)) return ESP_OK;
        if (result.result < 0 || result.data_bytes < 0 ||
            (result.data_bytes % (int)sizeof(int16_t)) != 0 ||
            (result.data_bytes != 0 && result.data == NULL) ||
            (result.data_bytes != 0 &&
             (size_t)result.data_bytes != backend->fetch_bytes)) return ESP_FAIL;
        if (result.data_bytes == 0) return ESP_OK;
        if (backend->ops.publish_pcm != NULL) {
            esp_err_t publish_error = backend->ops.publish_pcm(
                backend->ops.context, result.data,
                (size_t)result.data_bytes / sizeof(int16_t));
            if (publish_error != ESP_OK) return publish_error;
        }
        if (!backend->stream_opus) continue;
        esp_err_t error = vk_audio_pipeline_push(&backend->pipeline, result.data,
                                                 (size_t)result.data_bytes / sizeof(int16_t));
        if (error != ESP_OK) return error;
    }
}

static esp_err_t publish_louder_stereo_slot(vk_audio_backend_t *backend,
                                             int16_t *samples,
                                             size_t sample_count)
{
    if ((sample_count % 2U) != 0U) return ESP_ERR_INVALID_SIZE;
    size_t frame_count = sample_count / 2U;
    if (frame_count == 0U) return ESP_OK;

    uint64_t energy[2] = {0U, 0U};
    for (size_t frame = 0U; frame < frame_count; ++frame) {
        for (size_t slot = 0U; slot < 2U; ++slot) {
            int32_t sample = samples[(frame * 2U) + slot];
            energy[slot] += (uint32_t)(sample < 0 ? -sample : sample);
        }
    }

    size_t selected_slot = energy[1] > energy[0] ? 1U : 0U;
    for (size_t frame = 0U; frame < frame_count; ++frame) {
        samples[frame] = samples[(frame * 2U) + selected_slot];
    }
    return backend->ops.publish_pcm(backend->ops.context, samples, frame_count);
}

esp_err_t vk_audio_backend_acquire(vk_audio_backend_t *backend,
                                    const vk_audio_backend_ops_t *ops,
                                    uint32_t session_id, bool stream_opus)
{
    if (backend == NULL || !valid(ops, stream_opus) || session_id == 0U) return ESP_ERR_INVALID_ARG;
    memset(backend, 0, sizeof(*backend)); backend->ops = *ops;
    backend->stream_opus = stream_opus;
    if (stream_opus) {
        backend->ring = ops->allocate(ops->context, VK_AUDIO_BACKEND_RING_BYTES, true);
        if (backend->ring == NULL) return ESP_ERR_NO_MEM;
    }
    esp_err_t error = ops->i2s_create(ops->context, &backend->i2s);
    if (error != ESP_OK) return error;
    error = ops->i2s_initialize(ops->context, backend->i2s);
    if (error != ESP_OK) return error;
    if (stream_opus) {
        error = ops->afe_create(ops->context, &backend->afe, &backend->feed_bytes,
                                &backend->fetch_bytes);
        if (error != ESP_OK) return error;
        if (backend->feed_bytes == 0U || backend->feed_bytes > VK_AUDIO_BACKEND_RING_BYTES ||
            (backend->feed_bytes % (2U * sizeof(int16_t))) != 0U ||
            backend->fetch_bytes == 0U ||
            backend->fetch_bytes > VK_AUDIO_BACKEND_RING_BYTES ||
            (backend->fetch_bytes % sizeof(int16_t)) != 0U) return ESP_ERR_INVALID_SIZE;
        backend->feed = ops->allocate(ops->context, backend->feed_bytes, true);
        if (backend->feed == NULL) return ESP_ERR_NO_MEM;
    }
    error = ops->i2s_enable(ops->context, backend->i2s);
    if (error != ESP_OK) return error;
    backend->i2s_enabled = true;
    if (!stream_opus) return ESP_OK;
    error = ops->opus_create(ops->context, &backend->opus);
    if (error != ESP_OK) return error;
    vk_audio_pipeline_ops_t pipeline_ops = {.encode=encode,.send=send_frame,.context=backend};
    return vk_audio_pipeline_init(&backend->pipeline, &pipeline_ops, session_id);
}

esp_err_t vk_audio_backend_capture(vk_audio_backend_t *backend)
{
    if (backend == NULL || backend->i2s == NULL || !backend->i2s_enabled) return ESP_ERR_INVALID_STATE;
    int16_t samples[VK_AUDIO_BACKEND_READ_BYTES / sizeof(int16_t)];
    uint8_t *bytes = (uint8_t *)samples;
    size_t count = 0U;
    esp_err_t error = backend->ops.i2s_read(backend->ops.context, backend->i2s,
                                           bytes, VK_AUDIO_BACKEND_READ_BYTES, &count);
    bool timed_out = error == ESP_ERR_TIMEOUT;
    if (error != ESP_OK && !timed_out) return ESP_FAIL;
    if (count > VK_AUDIO_BACKEND_READ_BYTES ||
        (count % sizeof(int16_t)) != 0U) return ESP_FAIL;
    if (count == 0U) {
        return backend->stream_opus ? drain(backend) : ESP_OK;
    }
    if (!backend->stream_opus) {
        return publish_louder_stereo_slot(backend, samples,
                                          count / sizeof(int16_t));
    }
    if (backend->ring == NULL || backend->feed == NULL || backend->afe == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    error = ring_write(backend, bytes, count); if (error != ESP_OK) return error;
    while (backend->ring_count >= backend->feed_bytes) {
        ring_read(backend, (uint8_t *)backend->feed, backend->feed_bytes);
        if (backend->ops.afe_feed(backend->ops.context, backend->afe, backend->feed) < 0) return ESP_FAIL;
        error = drain(backend); if (error != ESP_OK) return error;
    }
    return timed_out ? drain(backend) : ESP_OK;
}

esp_err_t vk_audio_backend_finish(vk_audio_backend_t *backend)
{
    if (backend == NULL) return ESP_ERR_INVALID_ARG;
    return backend->stream_opus ? vk_audio_pipeline_finish(&backend->pipeline) : ESP_OK;
}

esp_err_t vk_audio_backend_release(vk_audio_backend_t *backend)
{
    if (backend == NULL || !valid(&backend->ops, backend->stream_opus)) return ESP_ERR_INVALID_ARG;
    if (backend->opus != NULL) {
        backend->ops.opus_destroy(backend->ops.context, backend->opus);
        backend->opus = NULL;
    }
    if (backend->i2s_enabled) {
        esp_err_t error = backend->ops.i2s_disable(backend->ops.context,
                                                   backend->i2s);
        if (error != ESP_OK) return error;
        backend->i2s_enabled = false;
    }
    if (backend->feed != NULL) {
        backend->ops.deallocate(backend->ops.context, backend->feed);
        backend->feed = NULL;
    }
    if (backend->afe != NULL) {
        backend->ops.afe_destroy(backend->ops.context, backend->afe);
        backend->afe = NULL;
    }
    if (backend->i2s != NULL) {
        esp_err_t error = backend->ops.i2s_destroy(backend->ops.context,
                                                   backend->i2s);
        if (error != ESP_OK) return error;
        backend->i2s = NULL;
    }
    if (backend->ring != NULL) {
        backend->ops.deallocate(backend->ops.context, backend->ring);
        backend->ring = NULL;
    }
    return ESP_OK;
}
