#include "vk_led.h"

#include <string.h>

struct vk_led {
    vk_led_transport_ops_t transport;
    vk_led_profile_t profile;
    vk_led_intent_t pending[VK_LED_SOURCE_COUNT];
    bool pending_valid[VK_LED_SOURCE_COUNT];
    bool active[VK_LED_SOURCE_COUNT];
    uint8_t current[VK_LED_PIXEL_COUNT][3];
    uint8_t target[VK_LED_PIXEL_COUNT][3];
    size_t pending_count;
    uint32_t epoch;
    uint64_t next_tick_ms;
    bool profile_admitted;
    bool enabled;
    uint8_t brightness;
    bool started;
    bool admission_open;
    bool hardware_failure;
    bool tainted;
    bool stopping;
    bool cleanup_pending;
    bool cleanup_complete;
    bool ack_pending;
    vk_led_lifecycle_request_t lifecycle;
    vk_led_lifecycle_publish_t publish;
    void *publish_context;
    bool fail_dark_only;
};

static void lock(vk_led_t *led) { if (led->transport.lock != NULL) led->transport.lock(led->transport.context); }
static void unlock(vk_led_t *led) { if (led->transport.unlock != NULL) led->transport.unlock(led->transport.context); }

static bool valid_sha(const char value[VK_LED_PROFILE_HASH_BYTES])
{
    if (value == NULL || strnlen(value, VK_LED_PROFILE_HASH_BYTES) != 64U) return false;
    for (size_t index = 0; index < 64U; ++index) {
        char byte = value[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return false;
    }
    return true;
}

bool vk_led_profile_validate(const vk_led_profile_t *profile)
{
    if (profile == NULL || !profile->mapping_reviewed || !profile->sustained_current_reviewed ||
        !profile->allowlisted_build || profile->strip_first != 4U || profile->strip_count != 13U ||
        profile->max_brightness == 0U || profile->max_frame_channel_sum == 0U ||
        profile->max_frame_channel_sum > (uint16_t)(VK_LED_PIXEL_COUNT * 3U * 255U) ||
        !valid_sha(profile->artifact_sha256) || !valid_sha(profile->board_profile_sha256) ||
        !valid_sha(profile->firmware_policy_sha256)) return false;
    uint8_t seen = 0U;
    for (size_t index = 0; index < 4U; ++index) {
        uint8_t pixel = profile->key_pixels[index];
        if (pixel > 3U || (seen & (uint8_t)(1U << pixel)) != 0U) return false;
        seen |= (uint8_t)(1U << pixel);
    }
    return seen == 0x0fU;
}

static void clear_pending(vk_led_t *led)
{
    memset(led->pending_valid, 0, sizeof(led->pending_valid));
    led->pending_count = 0U;
}

static esp_err_t prove_all_off(vk_led_t *led)
{
    memset(led->current, 0, sizeof(led->current));
    memset(led->target, 0, sizeof(led->target));
    if (led->fail_dark_only) return ESP_OK;
    if (led->transport.apply_all_off == NULL || led->transport.apply_all_off(led->transport.context) != ESP_OK) {
        led->hardware_failure = true;
        led->tainted = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t vk_led_size(void) { return sizeof(vk_led_t); }

esp_err_t vk_led_init(vk_led_t *led, const vk_led_transport_ops_t *transport,
                      const vk_led_profile_t *profile)
{
    if (led == NULL || transport == NULL || transport->apply_complete_frame == NULL ||
        transport->apply_all_off == NULL || transport->release == NULL ||
        ((transport->lock == NULL) != (transport->unlock == NULL))) return ESP_ERR_INVALID_ARG;
    memset(led, 0, sizeof(*led));
    led->transport = *transport;
    if (!vk_led_profile_validate(profile)) return ESP_ERR_NOT_SUPPORTED;
    led->profile = *profile;
    led->profile_admitted = true;
    return prove_all_off(led);
}

esp_err_t vk_led_init_fail_dark(vk_led_t *led)
{
    if (led == NULL) return ESP_ERR_INVALID_ARG;
    memset(led, 0, sizeof(*led));
    led->fail_dark_only = true;
    return ESP_OK;
}

esp_err_t vk_led_start(vk_led_t *led, uint32_t epoch)
{
    if (led == NULL || epoch == 0U) return ESP_ERR_INVALID_ARG;
    lock(led);
    if (led->started || led->tainted) { unlock(led); return ESP_ERR_INVALID_STATE; }
    led->epoch = epoch;
    led->started = true;
    led->admission_open = true;
    led->next_tick_ms = 0U;
    unlock(led);
    return ESP_OK;
}

esp_err_t vk_led_submit(vk_led_t *led, const vk_led_intent_t *intent)
{
    if (led == NULL || intent == NULL || intent->source >= VK_LED_SOURCE_COUNT ||
        intent->expected_epoch == 0U) return ESP_ERR_INVALID_ARG;
    lock(led);
    if (!led->started || !led->admission_open || led->tainted ||
        intent->expected_epoch != led->epoch) { unlock(led); return ESP_ERR_INVALID_STATE; }
    size_t source = (size_t)intent->source;
    if (!led->pending_valid[source] && led->pending_count >= VK_LED_ORDINARY_CAPACITY) {
        led->admission_open = false;
        led->tainted = true;
        clear_pending(led);
        (void)prove_all_off(led);
        unlock(led);
        return ESP_ERR_NO_MEM;
    }
    if (!led->pending_valid[source]) ++led->pending_count;
    led->pending[source] = *intent;
    led->pending_valid[source] = true;
    unlock(led);
    return ESP_OK;
}

esp_err_t vk_led_configure(vk_led_t *led, uint32_t expected_epoch, bool enabled,
                           uint8_t brightness)
{
    if (led == NULL || expected_epoch == 0U) return ESP_ERR_INVALID_ARG;
    lock(led);
    if (!led->started || !led->admission_open || expected_epoch != led->epoch || led->tainted) {
        unlock(led); return ESP_ERR_INVALID_STATE;
    }
    if (!led->profile_admitted || led->fail_dark_only) { unlock(led); return ESP_ERR_NOT_SUPPORTED; }
    if (brightness > led->profile.max_brightness) { unlock(led); return ESP_ERR_INVALID_ARG; }
    led->enabled = enabled;
    led->brightness = brightness;
    unlock(led);
    return ESP_OK;
}

static vk_led_effective_t effective(const vk_led_t *led)
{
    if (!led->profile_admitted || !led->enabled || led->brightness == 0U)
        return VK_LED_EFFECTIVE_OFF;
    if (led->active[VK_LED_SOURCE_UPDATE] || led->active[VK_LED_SOURCE_SCREEN]) return VK_LED_EFFECTIVE_MUTATION;
    if (led->active[VK_LED_SOURCE_AUDIO]) return VK_LED_EFFECTIVE_RECORDING;
    return VK_LED_EFFECTIVE_CONNECTED;
}

static void base_color(vk_led_effective_t state, uint8_t output[3])
{
    switch (state) {
    case VK_LED_EFFECTIVE_CONNECTED: output[0] = 0U; output[1] = 32U; output[2] = 64U; break;
    case VK_LED_EFFECTIVE_RECORDING: output[0] = 255U; output[1] = 0U; output[2] = 0U; break;
    case VK_LED_EFFECTIVE_MUTATION: output[0] = 128U; output[1] = 32U; output[2] = 0U; break;
    default: output[0] = output[1] = output[2] = 0U; break;
    }
}

static esp_err_t build_target(vk_led_t *led)
{
    vk_led_effective_t state = effective(led);
    uint8_t base[3];
    base_color(state, base);
    uint8_t brightness = led->brightness;
    uint32_t sum = 0U;
    for (size_t pixel = 0; pixel < VK_LED_PIXEL_COUNT; ++pixel) {
        for (size_t channel = 0; channel < 3U; ++channel) {
            uint32_t scaled = ((uint32_t)base[channel] * brightness) / 255U;
            if (scaled > led->profile.max_brightness) return ESP_ERR_INVALID_SIZE;
            uint32_t next = sum + scaled;
            if (next < sum || next > led->profile.max_frame_channel_sum) return ESP_ERR_INVALID_SIZE;
            sum = next;
            led->target[pixel][channel] = (uint8_t)scaled;
        }
    }
    return ESP_OK;
}

esp_err_t vk_led_process(vk_led_t *led, uint64_t now_ms)
{
    if (led == NULL) return ESP_ERR_INVALID_ARG;
    lock(led);
    if (!led->started || led->tainted) { unlock(led); return ESP_ERR_INVALID_STATE; }
    for (size_t source = 0; source < VK_LED_SOURCE_COUNT; ++source) {
        if (led->pending_valid[source]) {
            led->active[source] = led->pending[source].active;
            led->pending_valid[source] = false;
        }
    }
    led->pending_count = 0U;
    if (!led->profile_admitted || led->fail_dark_only) { unlock(led); return ESP_OK; }
    if (now_ms < led->next_tick_ms) { unlock(led); return ESP_OK; }
    if (build_target(led) != ESP_OK) {
        led->tainted = true; led->admission_open = false; (void)prove_all_off(led); unlock(led); return ESP_ERR_INVALID_SIZE;
    }
    bool changed = false;
    for (size_t pixel = 0; pixel < VK_LED_PIXEL_COUNT; ++pixel) {
        for (size_t channel = 0; channel < 3U; ++channel) {
            uint8_t current = led->current[pixel][channel];
            uint8_t target = led->target[pixel][channel];
            if (current < target) {
                uint8_t delta = (uint8_t)(target - current);
                led->current[pixel][channel] = (uint8_t)(current + (delta > VK_LED_MAX_CHANNEL_STEP ? VK_LED_MAX_CHANNEL_STEP : delta));
                changed = true;
            } else if (current > target) {
                uint8_t delta = (uint8_t)(current - target);
                led->current[pixel][channel] = (uint8_t)(current - (delta > VK_LED_MAX_CHANNEL_STEP ? VK_LED_MAX_CHANNEL_STEP : delta));
                changed = true;
            }
        }
    }
    led->next_tick_ms = now_ms + VK_LED_TICK_MS;
    if (changed && led->transport.apply_complete_frame(led->transport.context, led->current) != ESP_OK) {
        led->hardware_failure = true; led->tainted = true; led->admission_open = false;
        (void)prove_all_off(led); unlock(led); return ESP_FAIL;
    }
    unlock(led);
    return ESP_OK;
}

vk_led_lifecycle_begin_result_t vk_led_begin_lifecycle(
    vk_led_t *led, const vk_led_lifecycle_request_t *request,
    vk_led_lifecycle_publish_t publish, void *publish_context)
{
    if (led == NULL || request == NULL || publish == NULL || request->token == 0U ||
        request->lifecycle_generation == 0U || request->kind > VK_LED_LIFECYCLE_STOPPING ||
        request->absolute_deadline_ms == 0U) return VK_LED_LIFECYCLE_TAINTED;
    lock(led);
    if (led->ack_pending) {
        bool same = led->lifecycle.token == request->token &&
                    led->lifecycle.lifecycle_generation == request->lifecycle_generation &&
                    led->lifecycle.kind == request->kind;
        bool supersede = request->kind == VK_LED_LIFECYCLE_STOPPING &&
                         led->lifecycle.kind == VK_LED_LIFECYCLE_EPOCH_OFF;
        if (!same && !supersede) {
            led->tainted = true;
            unlock(led);
            return VK_LED_LIFECYCLE_TAINTED;
        }
        if (same) { unlock(led); return VK_LED_LIFECYCLE_ACCEPTED; }
        /* Retarget the retained cleanup proof; the old sink is no longer publishable. */
    } else if (led->cleanup_pending) {
        /* A proof without a live sink can only be retargeted by stopping. */
        if (request->kind != VK_LED_LIFECYCLE_STOPPING) {
            led->tainted = true;
            unlock(led);
            return VK_LED_LIFECYCLE_TAINTED;
        }
    } else {
        led->cleanup_pending = true;
        led->cleanup_complete = led->fail_dark_only;
    }
    led->admission_open = false;
    clear_pending(led);
    memset(led->active, 0, sizeof(led->active));
    led->lifecycle = *request;
    led->publish = publish;
    led->publish_context = publish_context;
    led->ack_pending = true;
    if (request->kind == VK_LED_LIFECYCLE_STOPPING) led->stopping = true;
    unlock(led);
    return VK_LED_LIFECYCLE_ACCEPTED;
}

esp_err_t vk_led_process_lifecycle(vk_led_t *led)
{
    if (led == NULL) return ESP_ERR_INVALID_ARG;

    /* Snapshot the obligation under the lock, then perform cleanup and publish
     * outside the lock. A generation guard prevents stale sinks from publishing
     * after a fresh supersession. */
    vk_led_lifecycle_publish_t publish = NULL;
    void *publish_context = NULL;
    vk_led_lifecycle_ack_t ack = {0};
    bool needs_transport = false;
    uint32_t committed_generation = 0U;
    bool committed = false;

    lock(led);
    if (!led->cleanup_pending || !led->ack_pending) { unlock(led); return ESP_OK; }
    needs_transport = !led->cleanup_complete && !led->fail_dark_only;
    publish = led->publish;
    publish_context = led->publish_context;
    committed_generation = led->lifecycle.lifecycle_generation;
    memset(led->current, 0, sizeof(led->current));
    memset(led->target, 0, sizeof(led->target));
    unlock(led);

    /* Transport cleanup outside the lock. */
    esp_err_t cleanup_result = ESP_OK;
    if (needs_transport) {
        cleanup_result = led->transport.apply_all_off == NULL ? ESP_FAIL :
            led->transport.apply_all_off(led->transport.context);
    }

    lock(led);
    /* Guard: a superseding begin may have installed a fresh obligation with a
     * higher generation. If our generation is stale, discard silently — the new
     * obligation owns the publish. */
    if (!led->ack_pending || led->lifecycle.lifecycle_generation != committed_generation) {
        unlock(led);
        return ESP_OK;
    }
    if (cleanup_result != ESP_OK) {
        led->hardware_failure = true;
        led->tainted = true;
    } else {
        led->cleanup_complete = true;
    }
    uint64_t now = led->transport.monotonic_ms != NULL ?
        led->transport.monotonic_ms(led->transport.context) : 0U;
    bool in_deadline = now < led->lifecycle.absolute_deadline_ms;
    ack.token = led->lifecycle.token;
    ack.lifecycle_generation = led->lifecycle.lifecycle_generation;
    ack.result = led->cleanup_complete && in_deadline && !led->tainted && !led->hardware_failure ?
        VK_LED_LIFECYCLE_QUIESCENT : VK_LED_LIFECYCLE_ACK_TAINTED;
    if (ack.result == VK_LED_LIFECYCLE_ACK_TAINTED) led->tainted = true;
    /* Commit: clear the obligation, then publish outside the lock. */
    committed = true;
    led->ack_pending = false;
    led->cleanup_pending = false;
    led->publish = NULL;
    led->publish_context = NULL;
    unlock(led);

    /* Publish outside the lock. The generation guard ensures only the winning
     * obligation publishes; a superseding begin between the two locks would have
     * incremented the generation, causing the first lock's commit to be discarded
     * and a fresh obligation to take ownership. */
    if (!committed) return ESP_OK;
    bool published = publish(publish_context, &ack);
    if (!published) {
        lock(led);
        led->tainted = true;
        unlock(led);
    }
    return published ? ESP_OK : ESP_FAIL;
}

esp_err_t vk_led_stop(vk_led_t *led)
{
    if (led == NULL) return ESP_ERR_INVALID_ARG;
    lock(led);
    led->admission_open = false;
    clear_pending(led);
    esp_err_t result = prove_all_off(led);
    if (result == ESP_OK && !led->fail_dark_only && led->transport.release(led->transport.context) != ESP_OK) {
        led->tainted = true;
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        led->started = false;
        led->epoch = 0U;
    }
    unlock(led);
    return result;
}

void vk_led_state(vk_led_t *led, vk_led_state_t *state)
{
    if (led == NULL || state == NULL) return;
    lock(led);
    memset(state, 0, sizeof(*state));
    state->available = led->profile_admitted && !led->hardware_failure && !led->tainted;
    state->unavailable_reason = led->tainted ? VK_LED_UNAVAILABLE_TAINTED :
        (led->hardware_failure ? VK_LED_UNAVAILABLE_HARDWARE_FAILED : VK_LED_UNAVAILABLE_CALIBRATION_REQUIRED);
    state->brightness = led->profile_admitted ? led->brightness : 0U;
    state->enabled = state->available && led->enabled;
    state->effective = state->available ? effective(led) : VK_LED_EFFECTIVE_OFF;
    state->tainted = led->tainted;
    unlock(led);
}
