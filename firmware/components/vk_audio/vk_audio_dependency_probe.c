#include "vk_audio_dependency_probe.h"

#include <stdint.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "opus.h"

#define VK_AUDIO_PROBE_SAMPLE_RATE 16000
#define VK_AUDIO_PROBE_CHANNELS 1
#define VK_AUDIO_PROBE_FRAME_SAMPLES 960
#define VK_AUDIO_PROBE_MAX_PACKET_BYTES 220

esp_err_t vk_audio_dependency_probe(void)
{
    afe_config_t *config = afe_config_init("MM", NULL, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }

    config->aec_init = false;
    config->se_init = true;
    config->vad_init = false;
    config->wakenet_init = false;
    config->agc_init = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = 0;
    config->afe_perferred_priority = 20;
    config->afe_ringbuf_size = 64;
    config->afe_linear_gain = 1.0f;
    config->debug_init = false;

    esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(config);
    if (afe == NULL || afe->create_from_config == NULL || afe->destroy == NULL || afe->reset_buffer == NULL ||
        afe->feed == NULL || afe->fetch == NULL || afe->get_feed_chunksize == NULL ||
        afe->get_fetch_chunksize == NULL || afe->get_feed_channel_num == NULL ||
        afe->get_fetch_channel_num == NULL || afe->get_samp_rate == NULL) {
        afe_config_free(config);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_afe_sr_data_t *afe_data = afe->create_from_config(config);
    if (afe_data == NULL) {
        afe_config_free(config);
        return ESP_ERR_NO_MEM;
    }

    const int feed_samples = afe->get_feed_chunksize(afe_data);
    const int fetch_samples = afe->get_fetch_chunksize(afe_data);
    const int feed_channels = afe->get_feed_channel_num(afe_data);
    const int fetch_channels = afe->get_fetch_channel_num(afe_data);
    const int sample_rate = afe->get_samp_rate(afe_data);
    const int reset_result = afe->reset_buffer(afe_data);
    afe->destroy(afe_data);
    afe_config_free(config);

    if (feed_samples <= 0 || fetch_samples <= 0 || feed_channels != 2 || fetch_channels != 1 ||
        sample_rate != VK_AUDIO_PROBE_SAMPLE_RATE || reset_result < 0) {
        return ESP_FAIL;
    }

    int opus_error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(VK_AUDIO_PROBE_SAMPLE_RATE, VK_AUDIO_PROBE_CHANNELS,
                                               OPUS_APPLICATION_VOIP, &opus_error);
    if (encoder == NULL || opus_error != OPUS_OK) {
        return ESP_FAIL;
    }

    opus_error = opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    if (opus_error == OPUS_OK) {
        opus_error = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
    }
    if (opus_error == OPUS_OK) {
        opus_error = opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
    }
    if (opus_error == OPUS_OK) {
        opus_error = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(4));
    }
    if (opus_error == OPUS_OK) {
        opus_error = opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    int16_t silence[VK_AUDIO_PROBE_FRAME_SAMPLES] = {0};
    uint8_t packet[VK_AUDIO_PROBE_MAX_PACKET_BYTES];
    const int encoded_bytes = opus_error == OPUS_OK
                                  ? opus_encode(encoder, silence, VK_AUDIO_PROBE_FRAME_SAMPLES, packet, sizeof(packet))
                                  : opus_error;
    opus_encoder_destroy(encoder);

    return encoded_bytes > 0 ? ESP_OK : ESP_FAIL;
}
