#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_usb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_USB_PROTOCOL_VERSION 0x01U
#define VK_USB_FRAME_TYPE_AUDIO 0x01U
#define VK_USB_FRAME_TYPE_JSON 0x10U
#define VK_USB_FRAME_TYPE_ASSET_CHUNK 0x40U
#define VK_USB_FRAME_TYPE_UPDATE_CHUNK 0x41U
#define VK_USB_MAX_FRAME_BYTES 4096U
#define VK_USB_MAX_JSON_BYTES (VK_USB_MAX_FRAME_BYTES - 4U)
#define VK_USB_IO_CHUNK_BYTES 256U
#define VK_USB_LEASE_MS 5000U
#define VK_USB_INPUT_LIFECYCLE_DEADLINE_MS 3250U
#define VK_USB_TX_TIMEOUT_MS 2000U
#define VK_USB_RX_TIMEOUT_MS 20U
#define VK_USB_AUDIO_MAX_PAYLOAD_BYTES 220U
#define VK_USB_ASSET_CHUNK_MAX_BYTES 4084U
#define VK_USB_UPDATE_CHUNK_MAX_BYTES 512U
#define VK_USB_TYPED_TX_QUEUE_CAPACITY 8U
#define VK_USB_CAPABILITY_MAX_FONTS 8U
#define VK_USB_ASSET_PAGE_MAX_ENTRIES 64U
#define VK_USB_PROTOCOL_MESSAGE_MAX_BYTES 96U
#define VK_USB_LED_PIXEL_COUNT 17U

typedef enum {
    VK_USB_CAPABILITY_ABSENT = 0,
    VK_USB_CAPABILITY_UNAVAILABLE,
    VK_USB_CAPABILITY_AVAILABLE,
} vk_usb_capability_state_t;

typedef enum {
    VK_USB_ASSETS_REASON_DISPLAY_ACCEPTANCE_REQUIRED = 0,
    VK_USB_ASSETS_REASON_STORAGE_UNAVAILABLE,
    VK_USB_ASSETS_REASON_INTEGRITY_UNAVAILABLE,
    VK_USB_ASSETS_REASON_POLICY_BLOCKED,
} vk_usb_assets_unavailable_reason_t;

typedef enum {
    VK_USB_SCREEN_REASON_DISPLAY_ACCEPTANCE_REQUIRED = 0,
    VK_USB_SCREEN_REASON_PANEL_UNAVAILABLE,
    VK_USB_SCREEN_REASON_MODEL_UNAVAILABLE,
    VK_USB_SCREEN_REASON_STORAGE_UNAVAILABLE,
    VK_USB_SCREEN_REASON_POLICY_BLOCKED,
} vk_usb_screen_unavailable_reason_t;

typedef enum {
    VK_USB_UPDATE_REASON_BOOTLOADER_MIGRATION_REQUIRED = 0,
    VK_USB_UPDATE_REASON_BUSY,
    VK_USB_UPDATE_REASON_WRONG_RUNNING_SLOT,
    VK_USB_UPDATE_REASON_TARGET_UNAVAILABLE,
    VK_USB_UPDATE_REASON_INTEGRITY_UNAVAILABLE,
    VK_USB_UPDATE_REASON_POLICY_BLOCKED,
} vk_usb_update_unavailable_reason_t;

typedef enum {
    VK_USB_STORAGE_UNFORMATTED = 0,
    VK_USB_STORAGE_READY,
    VK_USB_STORAGE_CORRUPT,
    VK_USB_STORAGE_MOUNT_FAILED,
    VK_USB_STORAGE_BUSY,
} vk_usb_storage_state_t;

typedef enum {
    VK_USB_SCREEN_MODE_IMAGE = 1U << 0,
    VK_USB_SCREEN_MODE_PET = 1U << 1,
    VK_USB_SCREEN_MODE_DASHBOARD = 1U << 2,
    VK_USB_SCREEN_MODE_CUSTOM = 1U << 3,
} vk_usb_screen_mode_t;

typedef struct {
    char id[33];
    uint16_t version;
    char metrics_sha256[65];
} vk_usb_font_capability_t;

typedef struct {
    vk_usb_capability_state_t state;
    vk_usb_assets_unavailable_reason_t unavailable_reason;
    vk_usb_storage_state_t storage_state;
    uint32_t free_bytes;
    uint32_t reserve_bytes;
    uint32_t upload_max_bytes;
    uint32_t max_asset_bytes;
    uint16_t chunk_bytes;
    uint16_t max_assets;
    uint16_t max_frames;
    uint16_t min_frame_ms;
    uint16_t max_frame_ms;
    uint32_t max_active_decoded_bytes;
    uint32_t decoder_scratch_bytes;
    uint32_t revision;
} vk_usb_assets_capability_t;

typedef struct {
    vk_usb_capability_state_t state;
    vk_usb_screen_unavailable_reason_t unavailable_reason;
    uint8_t modes;
    uint16_t max_commit_bytes;
    uint16_t max_layout_bytes;
    uint16_t max_assets;
    uint16_t max_objects;
    uint8_t max_depth;
    uint16_t max_widgets;
    uint16_t max_fonts;
    uint8_t max_pet_states;
    uint16_t max_string_bytes;
    uint16_t max_json_tokens;
    uint16_t max_widget_value_bytes;
    uint32_t revision;
    bool configured;
    uint16_t font_count;
    vk_usb_font_capability_t fonts[VK_USB_CAPABILITY_MAX_FONTS];
} vk_usb_screen_capability_t;

typedef enum { VK_USB_UPDATE_TARGET_OTA_0 = 0, VK_USB_UPDATE_TARGET_OTA_1 } vk_usb_update_target_t;

typedef struct {
    vk_usb_capability_state_t state;
    vk_usb_update_unavailable_reason_t unavailable_reason;
    uint16_t chunk_bytes;
    uint32_t max_image_bytes;
    vk_usb_update_target_t target;
} vk_usb_update_capability_t;

typedef enum {
    VK_USB_LED_REASON_CALIBRATION_REQUIRED = 0,
    VK_USB_LED_REASON_HARDWARE_FAILED,
    VK_USB_LED_REASON_TAINTED,
} vk_usb_led_unavailable_reason_t;

typedef struct {
    vk_usb_capability_state_t state;
    vk_usb_led_unavailable_reason_t unavailable_reason;
    uint8_t key_pixels[4];
    uint8_t strip_first;
    uint8_t strip_count;
    uint8_t max_brightness;
    uint16_t max_frame_channel_sum;
} vk_usb_led_capability_t;

typedef struct {
    vk_usb_assets_capability_t assets;
    vk_usb_screen_capability_t screen;
    vk_usb_update_capability_t update;
    vk_usb_led_capability_t led;
} vk_usb_capability_snapshot_t;

typedef esp_err_t (*vk_usb_capability_provider_t)(void *context, uint32_t expected_epoch,
                                                  vk_usb_capability_snapshot_t *snapshot);
/* The registrant retains context until vk_usb_service_stop() succeeds. A timeout
 * means an in-flight callback still owns the context and teardown must retry. */
typedef struct { vk_usb_capability_provider_t get_snapshot; void *context; } vk_usb_capability_provider_registration_t;

typedef enum {
    VK_USB_KEY_K1 = 0,
    VK_USB_KEY_K2,
    VK_USB_KEY_K3,
    VK_USB_KEY_K4,
    VK_USB_KEY_NONE,
} vk_usb_key_t;
typedef enum { VK_USB_BUTTON_DOWN = 0, VK_USB_BUTTON_UP, VK_USB_BUTTON_CLICK } vk_usb_button_kind_t;
typedef enum {
    VK_USB_HANDOFF_ACCEPTED = 0,
    VK_USB_HANDOFF_RETRY,
    VK_USB_HANDOFF_EPOCH_CLOSED,
    VK_USB_HANDOFF_OVERFLOW,
} vk_usb_handoff_result_t;
typedef enum {
    VK_USB_INPUT_MODE_HOLD_TO_TALK = 0,
    VK_USB_INPUT_MODE_CLICK_TO_TALK,
} vk_usb_input_mode_t;
typedef enum {
    VK_USB_INPUT_COMMAND_MODE = 0,
    VK_USB_INPUT_COMMAND_VOICE_KEY,
} vk_usb_input_command_kind_t;
typedef struct {
    vk_usb_input_command_kind_t kind;
    vk_usb_input_mode_t mode;
    vk_usb_key_t key;
    uint32_t expected_epoch;
} vk_usb_input_command_t;
typedef esp_err_t (*vk_usb_input_handler_t)(void *, const vk_usb_input_command_t *);
typedef struct { vk_usb_input_handler_t handle_command; void *context; } vk_usb_input_handler_registration_t;

typedef enum {
    VK_USB_INPUT_LIFECYCLE_NEW_EPOCH = 0,
    VK_USB_INPUT_LIFECYCLE_LEASE_EXPIRED,
    VK_USB_INPUT_LIFECYCLE_STOPPING,
} vk_usb_input_lifecycle_kind_t;
typedef enum {
    VK_USB_INPUT_LIFECYCLE_ACCEPTED = 0,
    VK_USB_INPUT_LIFECYCLE_TAINTED,
} vk_usb_input_lifecycle_begin_result_t;
typedef enum {
    VK_USB_INPUT_LIFECYCLE_QUIESCENT = 0,
    VK_USB_INPUT_LIFECYCLE_ACK_TAINTED,
} vk_usb_input_lifecycle_ack_result_t;
typedef struct {
    vk_usb_input_lifecycle_kind_t kind;
    uint32_t token;
    uint32_t old_epoch;
    uint32_t proposed_epoch;
    uint32_t lifecycle_generation;
    uint64_t absolute_deadline_ms;
} vk_usb_input_lifecycle_request_t;
typedef struct {
    uint32_t token;
    uint32_t lifecycle_generation;
    vk_usb_input_lifecycle_ack_result_t result;
} vk_usb_input_lifecycle_ack_t;
typedef struct vk_usb_input_lifecycle_sink {
    bool (*publish)(void *context, const vk_usb_input_lifecycle_ack_t *ack);
    void *context;
} vk_usb_input_lifecycle_sink_t;
typedef vk_usb_input_lifecycle_begin_result_t (*vk_usb_input_lifecycle_begin_t)(
    void *context, const vk_usb_input_lifecycle_request_t *request,
    const vk_usb_input_lifecycle_sink_t *sink);
typedef struct {
    vk_usb_input_lifecycle_begin_t begin;
    void *context;
} vk_usb_input_lifecycle_registration_t;
/* LED is an independent participant in the same USB-owned transition. It uses
 * the common token/generation/deadline and cannot open an epoch by itself. */
typedef vk_usb_input_lifecycle_registration_t vk_usb_led_lifecycle_registration_t;

typedef enum {
    VK_USB_INPUT_ERROR_INVALID_REQUEST = 0,
    VK_USB_INPUT_ERROR_WRONG_EPOCH,
    VK_USB_INPUT_ERROR_BUSY,
    VK_USB_INPUT_ERROR_QUEUE_OVERFLOW,
    VK_USB_INPUT_ERROR_AUDIO_START_FAILED,
    VK_USB_INPUT_ERROR_AUDIO_STOP_FAILED,
    VK_USB_INPUT_ERROR_AUDIO_RUNTIME_FAILED,
    VK_USB_INPUT_ERROR_TAINTED,
} vk_usb_input_error_t;

typedef struct {
    vk_usb_button_kind_t kind;
    vk_usb_key_t key;
    bool has_session_id;
    uint32_t session_id;
    bool has_duration_ms;
    uint32_t duration_ms;
} vk_usb_button_event_t;

typedef struct {
    uint32_t session_id;
    uint32_t sequence;
    uint8_t flags;
    const uint8_t *payload;
    uint16_t payload_length;
} vk_usb_audio_frame_t;

typedef enum {
    VK_USB_ASSET_BEGIN = 0, VK_USB_ASSET_QUERY, VK_USB_ASSET_END,
    VK_USB_ASSET_ABORT, VK_USB_ASSET_LIST, VK_USB_ASSET_DELETE,
} vk_usb_asset_command_kind_t;

typedef enum {
    VK_USB_ASSET_KIND_IMAGE = 0,
    VK_USB_ASSET_KIND_ANIMATION,
    VK_USB_ASSET_KIND_GLYPH_BITMAP,
} vk_usb_asset_kind_t;

typedef struct {
    vk_usb_asset_command_kind_t kind;
    uint32_t expected_epoch;
    uint32_t snapshot_generation;
    uint32_t transfer_id;
    uint32_t total_bytes;
    uint32_t snapshot_id;
    uint32_t cursor;
    uint32_t expected_revision;
    uint8_t limit;
    char sha256[65];
    char asset_kind[17];
    vk_usb_asset_kind_t asset_kind_value;
} vk_usb_asset_command_t;

typedef enum { VK_USB_SCREEN_QUERY = 0, VK_USB_SCREEN_COMMIT } vk_usb_screen_command_kind_t;
typedef enum {
    VK_USB_SCREEN_IMAGE = 0,
    VK_USB_SCREEN_PET,
    VK_USB_SCREEN_DASHBOARD,
    VK_USB_SCREEN_CUSTOM,
} vk_usb_screen_configured_mode_t;

typedef struct {
    vk_usb_screen_command_kind_t kind;
    uint32_t expected_epoch;
    uint32_t snapshot_generation;
    uint32_t expected_revision;
    uint32_t revision;
    vk_usb_screen_configured_mode_t configured_mode;
    /* Immutable parsed document; valid only for the duration of handle_command. */
    const vk_usb_json_document_t *document;
    uint16_t assets_node;
    uint16_t screen_node;
} vk_usb_screen_command_t;

typedef struct {
    uint32_t expected_epoch;
    uint32_t snapshot_generation;
    uint32_t transfer_id;
    uint32_t offset;
    uint16_t payload_length;
    uint8_t payload[VK_USB_ASSET_CHUNK_MAX_BYTES];
} vk_usb_asset_chunk_t;

typedef enum {
    VK_USB_UPDATE_BEGIN = 0, VK_USB_UPDATE_SEAL, VK_USB_UPDATE_QUERY,
    VK_USB_UPDATE_CANCEL, VK_USB_UPDATE_ACTIVATE,
} vk_usb_update_command_kind_t;

typedef struct {
    vk_usb_update_command_kind_t kind;
    uint32_t transfer_id;
    uint32_t size;
    char sha256[65];
} vk_usb_update_command_t;

typedef struct {
    uint32_t transfer_id;
    uint32_t offset;
    uint16_t payload_length;
    uint8_t payload[VK_USB_UPDATE_CHUNK_MAX_BYTES];
} vk_usb_update_chunk_t;

typedef struct {
    char sha256[65];
    uint32_t total_bytes;
    vk_usb_asset_kind_t kind;
    bool referenced;
} vk_usb_asset_list_entry_t;

typedef enum {
    VK_USB_ASSET_EVENT_STORAGE_FORMATTED = 0,
    VK_USB_ASSET_EVENT_READY,
    VK_USB_ASSET_EVENT_PROGRESS,
    VK_USB_ASSET_EVENT_STORED,
    VK_USB_ASSET_EVENT_ABORTED,
    VK_USB_ASSET_EVENT_PAGE,
    VK_USB_ASSET_EVENT_DELETED,
} vk_usb_asset_event_kind_t;

typedef struct {
    vk_usb_asset_event_kind_t kind;
    uint32_t transfer_id;
    uint32_t total_bytes;
    uint32_t next_offset;
    uint32_t snapshot_id;
    uint32_t cursor;
    uint32_t next_cursor;
    uint32_t revision;
    uint16_t chunk_bytes;
    bool has_next_cursor;
    char sha256[65];
    vk_usb_asset_kind_t asset_kind;
    size_t entry_count;
    const vk_usb_asset_list_entry_t *entries;
} vk_usb_asset_event_t;

typedef esp_err_t (*vk_usb_asset_handler_t)(void *, const vk_usb_asset_command_t *);
typedef esp_err_t (*vk_usb_asset_chunk_handler_t)(void *, const vk_usb_asset_chunk_t *);
typedef esp_err_t (*vk_usb_asset_transfer_state_t)(void *, uint32_t transfer_id,
                                                   uint32_t expected_epoch,
                                                   uint32_t snapshot_generation,
                                                   vk_usb_asset_command_t *tuple,
                                                   uint32_t *next_offset);
typedef esp_err_t (*vk_usb_asset_event_handler_t)(void *, const vk_usb_asset_command_t *,
                                                  vk_usb_asset_event_t *event);
typedef size_t (*vk_usb_asset_error_detail_t)(void *, char *output, size_t capacity);
typedef struct vk_usb_screen_event vk_usb_screen_event_t;
typedef esp_err_t (*vk_usb_screen_handler_t)(void *, const vk_usb_screen_command_t *,
                                             vk_usb_screen_event_t *);
typedef size_t (*vk_usb_screen_error_detail_t)(void *, char *output, size_t capacity);
typedef esp_err_t (*vk_usb_update_handler_t)(void *, const vk_usb_update_command_t *);
typedef esp_err_t (*vk_usb_update_chunk_handler_t)(void *, const vk_usb_update_chunk_t *);

typedef struct {
    vk_usb_asset_handler_t handle_command;
    vk_usb_asset_chunk_handler_t handle_chunk;
    void *context;
    uint16_t chunk_bytes;
    uint32_t max_asset_bytes;
    vk_usb_asset_transfer_state_t get_transfer_state;
    vk_usb_asset_event_handler_t build_event;
    vk_usb_asset_error_detail_t error_detail;
} vk_usb_asset_handler_registration_t;

typedef struct {
    vk_usb_screen_handler_t handle_command;
    vk_usb_screen_error_detail_t error_detail;
    void *context;
} vk_usb_screen_handler_registration_t;

typedef enum { VK_USB_SCREEN_EVENT_STATE = 0, VK_USB_SCREEN_EVENT_COMMITTED } vk_usb_screen_event_kind_t;
struct vk_usb_screen_event {
    vk_usb_screen_event_kind_t kind;
    bool configured;
    vk_usb_screen_configured_mode_t configured_mode;
    uint32_t previous_revision;
    uint32_t revision;
    char assets_manifest_sha256[65];
    char screen_manifest_sha256[65];
};

typedef enum { VK_USB_WIDGET_FRESH = 0, VK_USB_WIDGET_STALE, VK_USB_WIDGET_ERROR } vk_usb_widget_state_t;
typedef enum { VK_USB_WIDGET_VALUE_NONE = 0, VK_USB_WIDGET_VALUE_TEXT, VK_USB_WIDGET_VALUE_NUMBER } vk_usb_widget_value_kind_t;
typedef struct {
    uint32_t expected_epoch;
    uint32_t snapshot_generation;
    uint32_t revision;
    uint32_t sequence;
    vk_usb_widget_state_t state;
    vk_usb_widget_value_kind_t value_kind;
    char widget_id[33];
    char text[513];
    int64_t number_milli;
    char message[VK_USB_PROTOCOL_MESSAGE_MAX_BYTES + 1U];
} vk_usb_widget_command_t;
typedef struct {
    uint32_t revision;
    uint32_t sequence;
    vk_usb_widget_state_t state;
    char widget_id[33];
} vk_usb_widget_event_t;
typedef esp_err_t (*vk_usb_widget_handler_t)(void *, const vk_usb_widget_command_t *,
                                             vk_usb_widget_event_t *);
typedef struct { vk_usb_widget_handler_t handle_command; void *context; } vk_usb_widget_handler_registration_t;

typedef enum { VK_USB_ERROR_ASSET = 0, VK_USB_ERROR_STORAGE, VK_USB_ERROR_SCREEN, VK_USB_ERROR_WIDGET } vk_usb_error_operation_t;
typedef struct {
    vk_usb_error_operation_t operation;
    const char *code;
    bool has_transfer_id;
    uint32_t transfer_id;
    bool has_next_offset;
    uint32_t next_offset;
    const char *sha256;
    const char *message;
} vk_usb_protocol_error_t;

typedef enum { VK_USB_LED_QUERY = 0, VK_USB_LED_CONFIG } vk_usb_led_command_kind_t;
typedef enum {
    VK_USB_LED_EFFECTIVE_OFF = 0,
    VK_USB_LED_EFFECTIVE_CONNECTED,
    VK_USB_LED_EFFECTIVE_RECORDING,
    VK_USB_LED_EFFECTIVE_MUTATION,
} vk_usb_led_effective_t;
typedef enum { VK_USB_LED_STATE_QUERY = 0, VK_USB_LED_STATE_APPLIED } vk_usb_led_state_source_t;
typedef struct {
    vk_usb_led_command_kind_t kind;
    uint32_t expected_epoch;
    uint32_t snapshot_generation;
    uint32_t request_id;
    bool enabled;
    uint8_t brightness;
} vk_usb_led_command_t;
typedef struct {
    vk_usb_led_state_source_t source;
    uint32_t request_id;
    bool available;
    vk_usb_led_unavailable_reason_t unavailable_reason;
    bool enabled;
    uint8_t brightness;
    vk_usb_led_effective_t effective;
} vk_usb_led_state_event_t;
typedef enum {
    VK_USB_LED_ERROR_INVALID_REQUEST = 0,
    VK_USB_LED_ERROR_WRONG_EPOCH,
    VK_USB_LED_ERROR_UNAVAILABLE,
    VK_USB_LED_ERROR_BUSY,
    VK_USB_LED_ERROR_QUEUE_OVERFLOW,
    VK_USB_LED_ERROR_HARDWARE_FAILED,
    VK_USB_LED_ERROR_TAINTED,
} vk_usb_led_error_t;
typedef struct {
    uint32_t request_id;
    vk_usb_led_error_t code;
    const char *message;
} vk_usb_led_error_event_t;
typedef esp_err_t (*vk_usb_led_handler_t)(void *, const vk_usb_led_command_t *,
                                         vk_usb_led_state_event_t *);
typedef struct { vk_usb_led_handler_t handle_command; void *context; } vk_usb_led_handler_registration_t;

typedef struct {
    vk_usb_update_handler_t handle_command;
    vk_usb_update_chunk_handler_t handle_chunk;
    bool (*current_tuple_available)(void *context);
    void *context;
    uint16_t chunk_bytes;
    uint32_t max_image_bytes;
} vk_usb_update_handler_registration_t;

typedef struct {
    esp_err_t (*install)(void *context);
    esp_err_t (*uninstall)(void *context);
    int (*read)(void *context, uint8_t *bytes, size_t capacity, uint32_t timeout_ms);
    int (*write)(void *context, const uint8_t *bytes, size_t length, uint32_t timeout_ms);
    uint64_t (*now_ms)(void *context);
    void (*state_lock)(void *context);
    void (*state_unlock)(void *context);
    void *context;
} vk_usb_transport_ops_t;

typedef struct {
    const char *hardware;
    const char *firmware_version;
    const char *device_id;
} vk_usb_identity_t;

typedef struct {
    /* Immutable for the lifetime of the service. Production passes false until migration review. */
    bool update_boot_policy_enabled;
} vk_usb_service_policy_t;

typedef struct vk_usb_service vk_usb_service_t;

size_t vk_usb_service_size(void);
esp_err_t vk_usb_service_init(vk_usb_service_t *, const vk_usb_transport_ops_t *, const vk_usb_identity_t *, const vk_usb_service_policy_t *);
esp_err_t vk_usb_service_start(vk_usb_service_t *);
esp_err_t vk_usb_service_poll(vk_usb_service_t *);
/* Closes producer admission before the owner task is notified. */
void vk_usb_service_request_stop(vk_usb_service_t *);
esp_err_t vk_usb_service_stop(vk_usb_service_t *);
bool vk_usb_service_has_epoch(vk_usb_service_t *);
uint32_t vk_usb_service_epoch(vk_usb_service_t *);
#ifdef VK_USB_NATIVE_TEST
typedef void (*vk_usb_before_tx_commit_hook_t)(void *context);
typedef void (*vk_usb_stop_requested_hook_t)(void *context);
void vk_usb_service_set_epoch_for_test(vk_usb_service_t *, uint32_t);
size_t vk_usb_service_tx_queue_count_for_test(vk_usb_service_t *);
esp_err_t vk_usb_service_consume_for_test(vk_usb_service_t *, const uint8_t *, size_t);
void vk_usb_service_set_before_tx_commit_hook_for_test(vk_usb_service_t *, vk_usb_before_tx_commit_hook_t, void *);
#ifdef VK_USB_PRODUCTION_NATIVE_TEST
void vk_usb_set_before_tx_commit_hook_for_test(vk_usb_before_tx_commit_hook_t, void *);
void vk_usb_set_stop_requested_hook_for_test(vk_usb_stop_requested_hook_t, void *);
void vk_usb_set_poll_returned_hook_for_test(vk_usb_stop_requested_hook_t, void *);
esp_err_t vk_usb_consume_for_test(const uint8_t *, size_t);
#endif
size_t vk_usb_service_consume_bytes_for_test(vk_usb_service_t *);
size_t vk_usb_service_parser_steps_for_test(vk_usb_service_t *);
#endif

/* Typed integration only. No function accepts an arbitrary frame type, JSON body, or byte body. */
esp_err_t vk_usb_service_register_capability_provider(vk_usb_service_t *, const vk_usb_capability_provider_registration_t *);
esp_err_t vk_usb_service_register_asset_handler(vk_usb_service_t *, const vk_usb_asset_handler_registration_t *);
esp_err_t vk_usb_service_register_screen_handler(vk_usb_service_t *, const vk_usb_screen_handler_registration_t *);
esp_err_t vk_usb_service_register_widget_handler(vk_usb_service_t *, const vk_usb_widget_handler_registration_t *);
esp_err_t vk_usb_service_register_update_handler(vk_usb_service_t *, const vk_usb_update_handler_registration_t *);
esp_err_t vk_usb_service_register_led_handler(vk_usb_service_t *, const vk_usb_led_handler_registration_t *);
esp_err_t vk_usb_service_register_input_handler(vk_usb_service_t *, const vk_usb_input_handler_registration_t *);
esp_err_t vk_usb_service_register_input_lifecycle(vk_usb_service_t *, const vk_usb_input_lifecycle_registration_t *);
esp_err_t vk_usb_service_register_led_lifecycle(vk_usb_service_t *, const vk_usb_led_lifecycle_registration_t *);
/* USB owner lifecycle pump. ACK publication uses the captured monotonic time. */
esp_err_t vk_usb_service_process_lifecycle(vk_usb_service_t *);
bool vk_usb_service_is_tainted(vk_usb_service_t *);
vk_usb_handoff_result_t vk_usb_service_send_button_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, const vk_usb_button_event_t *);
esp_err_t vk_usb_service_send_asset_event_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, uint32_t snapshot_generation, const vk_usb_asset_event_t *);
esp_err_t vk_usb_service_send_screen_event_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, uint32_t snapshot_generation, const vk_usb_screen_event_t *);
esp_err_t vk_usb_service_send_widget_event_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, uint32_t snapshot_generation, const vk_usb_widget_event_t *);
esp_err_t vk_usb_service_send_protocol_error_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, uint32_t snapshot_generation, const vk_usb_protocol_error_t *);
esp_err_t vk_usb_service_send_led_state_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, uint32_t snapshot_generation, const vk_usb_led_state_event_t *);
esp_err_t vk_usb_service_send_led_error_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, uint32_t snapshot_generation, const vk_usb_led_error_event_t *);
esp_err_t vk_usb_service_send_input_state_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, vk_usb_input_mode_t, vk_usb_key_t);
esp_err_t vk_usb_service_send_input_error_for_epoch(vk_usb_service_t *, uint32_t expected_epoch, vk_usb_input_error_t);
esp_err_t vk_usb_service_fail_input_epoch(vk_usb_service_t *, uint32_t expected_epoch, vk_usb_input_error_t);
esp_err_t vk_usb_service_send_button(vk_usb_service_t *, const vk_usb_button_event_t *);
esp_err_t vk_usb_service_send_audio(vk_usb_service_t *, const vk_usb_audio_frame_t *);
esp_err_t vk_usb_service_send_audio_for_epoch(vk_usb_service_t *, uint32_t expected_epoch,
                                               const vk_usb_audio_frame_t *);

esp_err_t vk_usb_start(void);
esp_err_t vk_usb_stop(void);
/* Production typed façade. No service pointer or raw-frame API crosses this boundary. */
esp_err_t vk_usb_current_epoch(uint32_t *epoch);
esp_err_t vk_usb_register_input_handler(const vk_usb_input_handler_registration_t *);
esp_err_t vk_usb_register_input_lifecycle(const vk_usb_input_lifecycle_registration_t *);
esp_err_t vk_usb_register_led_lifecycle(const vk_usb_led_lifecycle_registration_t *);
esp_err_t vk_usb_register_capability_provider(const vk_usb_capability_provider_registration_t *);
esp_err_t vk_usb_register_asset_handler(const vk_usb_asset_handler_registration_t *);
esp_err_t vk_usb_register_screen_handler(const vk_usb_screen_handler_registration_t *);
esp_err_t vk_usb_register_widget_handler(const vk_usb_widget_handler_registration_t *);
esp_err_t vk_usb_register_led_handler(const vk_usb_led_handler_registration_t *);
vk_usb_handoff_result_t vk_usb_send_button(uint32_t expected_epoch, const vk_usb_button_event_t *);
esp_err_t vk_usb_send_input_state(uint32_t expected_epoch, vk_usb_input_mode_t, vk_usb_key_t);
esp_err_t vk_usb_send_input_error(uint32_t expected_epoch, vk_usb_input_error_t);
esp_err_t vk_usb_fail_input_epoch(uint32_t expected_epoch, vk_usb_input_error_t);
esp_err_t vk_usb_send_audio(uint32_t expected_epoch, const vk_usb_audio_frame_t *frame);

#ifdef __cplusplus
}
#endif
