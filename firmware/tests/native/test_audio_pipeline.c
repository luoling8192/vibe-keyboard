#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "vk_audio_pipeline.h"

typedef struct {
    int encode_result;
    unsigned encode_count;
    unsigned send_count;
    unsigned fail_send_at;
    uint32_t session[8];
    uint32_t sequence[8];
    uint8_t flags[8];
    uint16_t lengths[8];
} fake_t;

static int encode(void *context, const int16_t *samples, size_t count,
                  uint8_t *packet, size_t capacity)
{
    fake_t *fake = context;
    assert(samples != NULL);
    assert(count == VK_AUDIO_OPUS_FRAME_SAMPLES);
    ++fake->encode_count;
    if (fake->encode_result > 0) {
        assert((size_t)fake->encode_result <= capacity || fake->encode_result > 220);
        if ((size_t)fake->encode_result <= capacity) memset(packet, 0x5a, (size_t)fake->encode_result);
    }
    return fake->encode_result;
}

static esp_err_t send_frame(void *context, uint32_t session, uint32_t sequence,
                            uint8_t flags, const uint8_t *payload,
                            uint16_t length)
{
    fake_t *fake = context;
    unsigned index = fake->send_count++;
    if (fake->fail_send_at != 0U && fake->send_count == fake->fail_send_at) {
        return ESP_ERR_NO_MEM;
    }
    assert(index < 8U);
    fake->session[index] = session;
    fake->sequence[index] = sequence;
    fake->flags[index] = flags;
    fake->lengths[index] = length;
    assert((length == 0U) == (payload == NULL));
    return ESP_OK;
}

static vk_audio_pipeline_t make(fake_t *fake)
{
    vk_audio_pipeline_t pipeline;
    vk_audio_pipeline_ops_t ops = {encode, send_frame, fake};
    assert(vk_audio_pipeline_init(&pipeline, &ops, 7U) == ESP_OK);
    return pipeline;
}

static void test_chunk_accumulation_and_eos(void)
{
    fake_t fake = {.encode_result = 20};
    vk_audio_pipeline_t pipeline = make(&fake);
    int16_t samples[2000] = {0};
    assert(vk_audio_pipeline_push(&pipeline, samples, 17U) == ESP_OK);
    assert(vk_audio_pipeline_push(&pipeline, samples + 17, 943U) == ESP_OK);
    assert(vk_audio_pipeline_push(&pipeline, samples + 960, 960U) == ESP_OK);
    assert(fake.encode_count == 2U && fake.send_count == 2U);
    assert(fake.session[0] == 7U && fake.sequence[0] == 0U && fake.flags[0] == VK_AUDIO_FIRST_FLAG);
    assert(fake.sequence[1] == 1U && fake.flags[1] == 0U);
    assert(vk_audio_pipeline_finish(&pipeline) == ESP_OK);
    assert(fake.send_count == 3U);
    assert(fake.sequence[2] == 2U && fake.flags[2] == VK_AUDIO_FINAL_FLAG && fake.lengths[2] == 0U);
    assert(vk_audio_pipeline_finish(&pipeline) == ESP_ERR_INVALID_STATE);
}

static void test_encoder_bounds(void)
{
    int16_t samples[VK_AUDIO_OPUS_FRAME_SAMPLES] = {0};
    const int bad[] = {-1, 0, 221};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fake_t fake = {.encode_result = bad[i]};
        vk_audio_pipeline_t pipeline = make(&fake);
        esp_err_t expected = bad[i] < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
        assert(vk_audio_pipeline_push(&pipeline, samples, VK_AUDIO_OPUS_FRAME_SAMPLES) == expected);
        assert(fake.send_count == 0U);
        assert(vk_audio_pipeline_finish(&pipeline) == ESP_ERR_INVALID_STATE);
    }
}

static void test_send_failure_does_not_advance(void)
{
    fake_t fake = {.encode_result = 20, .fail_send_at = 1U};
    vk_audio_pipeline_t pipeline = make(&fake);
    int16_t samples[VK_AUDIO_OPUS_FRAME_SAMPLES] = {0};
    assert(vk_audio_pipeline_push(&pipeline, samples, VK_AUDIO_OPUS_FRAME_SAMPLES) == ESP_ERR_NO_MEM);
    assert(pipeline.sequence == 0U && pipeline.failed);
    assert(vk_audio_pipeline_push(&pipeline, samples, 1U) == ESP_ERR_INVALID_STATE);
}

static void test_session_wrap(void)
{
    assert(vk_audio_next_session(0U) == 1U);
    assert(vk_audio_next_session(41U) == 42U);
    assert(vk_audio_next_session(UINT32_MAX) == 1U);
}

int main(void)
{
    test_chunk_accumulation_and_eos();
    test_encoder_bounds();
    test_send_failure_does_not_advance();
    test_session_wrap();
    return 0;
}
