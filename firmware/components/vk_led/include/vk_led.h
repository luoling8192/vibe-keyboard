#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_LED_PIXEL_COUNT 17U
#define VK_LED_ORDINARY_CAPACITY 8U
#define VK_LED_TICK_MS 30U
#define VK_LED_MAX_CHANNEL_STEP 32U
#define VK_LED_PROFILE_HASH_BYTES 65U

/* No raw pixel API is exposed. Producers submit semantic state only. */
typedef enum {
    VK_LED_SOURCE_USB = 0,
    VK_LED_SOURCE_AUDIO,
    VK_LED_SOURCE_SCREEN,
    VK_LED_SOURCE_UPDATE,
    VK_LED_SOURCE_COUNT,
} vk_led_source_t;

typedef enum {
    VK_LED_EFFECTIVE_OFF = 0,
    VK_LED_EFFECTIVE_CONNECTED,
    VK_LED_EFFECTIVE_RECORDING,
    VK_LED_EFFECTIVE_MUTATION,
} vk_led_effective_t;

typedef enum {
    VK_LED_UNAVAILABLE_CALIBRATION_REQUIRED = 0,
    VK_LED_UNAVAILABLE_HARDWARE_FAILED,
    VK_LED_UNAVAILABLE_TAINTED,
} vk_led_unavailable_reason_t;

typedef struct {
    uint8_t key_pixels[4];
    uint8_t strip_first;
    uint8_t strip_count;
    uint8_t max_brightness;
    uint16_t max_frame_channel_sum;
    char artifact_sha256[VK_LED_PROFILE_HASH_BYTES];
    char board_profile_sha256[VK_LED_PROFILE_HASH_BYTES];
    char firmware_policy_sha256[VK_LED_PROFILE_HASH_BYTES];
    bool mapping_reviewed;
    bool sustained_current_reviewed;
    bool allowlisted_build;
} vk_led_profile_t;

typedef struct {
    vk_led_source_t source;
    bool active;
    uint32_t expected_epoch;
} vk_led_intent_t;

typedef struct {
    bool available;
    vk_led_unavailable_reason_t unavailable_reason;
    bool enabled;
    uint8_t brightness;
    vk_led_effective_t effective;
    bool tainted;
} vk_led_state_t;

typedef enum {
    VK_LED_LIFECYCLE_EPOCH_OFF = 0,
    VK_LED_LIFECYCLE_STOPPING,
} vk_led_lifecycle_kind_t;

typedef enum {
    VK_LED_LIFECYCLE_ACCEPTED = 0,
    VK_LED_LIFECYCLE_TAINTED,
} vk_led_lifecycle_begin_result_t;

typedef enum {
    VK_LED_LIFECYCLE_QUIESCENT = 0,
    VK_LED_LIFECYCLE_ACK_TAINTED,
} vk_led_lifecycle_ack_result_t;

typedef struct {
    vk_led_lifecycle_kind_t kind;
    uint32_t token;
    uint32_t old_epoch;
    uint32_t proposed_epoch;
    uint32_t lifecycle_generation;
    uint64_t absolute_deadline_ms;
} vk_led_lifecycle_request_t;

typedef struct {
    uint32_t token;
    uint32_t lifecycle_generation;
    vk_led_lifecycle_ack_result_t result;
} vk_led_lifecycle_ack_t;

typedef bool (*vk_led_lifecycle_publish_t)(void *context, const vk_led_lifecycle_ack_t *ack);

/* Complete logical-RGB frame transport. Implementations must write all 17 pixels then refresh. */
typedef struct {
    esp_err_t (*apply_complete_frame)(void *context, const uint8_t logical_rgb[VK_LED_PIXEL_COUNT][3]);
    esp_err_t (*apply_all_off)(void *context);
    esp_err_t (*release)(void *context);
    void (*lock)(void *context);
    void (*unlock)(void *context);
    uint64_t (*monotonic_ms)(void *context);
    void *context;
} vk_led_transport_ops_t;

typedef struct vk_led vk_led_t;

size_t vk_led_size(void);
esp_err_t vk_led_init(vk_led_t *led, const vk_led_transport_ops_t *transport,
                      const vk_led_profile_t *profile);
esp_err_t vk_led_start(vk_led_t *led, uint32_t epoch);
esp_err_t vk_led_submit(vk_led_t *led, const vk_led_intent_t *intent);
esp_err_t vk_led_configure(vk_led_t *led, uint32_t expected_epoch, bool enabled,
                           uint8_t brightness);
esp_err_t vk_led_process(vk_led_t *led, uint64_t now_ms);
vk_led_lifecycle_begin_result_t vk_led_begin_lifecycle(
    vk_led_t *led, const vk_led_lifecycle_request_t *request,
    vk_led_lifecycle_publish_t publish, void *publish_context);
/* Called by the LED owner task; transport cleanup never runs in begin(). */
esp_err_t vk_led_process_lifecycle(vk_led_t *led);
esp_err_t vk_led_stop(vk_led_t *led);
void vk_led_state(vk_led_t *led, vk_led_state_t *state);
bool vk_led_profile_validate(const vk_led_profile_t *profile);

/* Safe production composition: no driver acquisition and no GPIO/RMT operation. */
esp_err_t vk_led_init_fail_dark(vk_led_t *led);

#ifdef __cplusplus
}
#endif
