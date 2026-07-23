#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "vk_input.h"
#include "vk_audio.h"
#include "vk_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef VK_INPUT_NATIVE_TEST
typedef enum {
    VK_INPUT_TEST_LIFECYCLE_OWNERS_QUIESCENT = 0,
    VK_INPUT_TEST_LIFECYCLE_BEFORE_ABORT,
    VK_INPUT_TEST_LIFECYCLE_BEFORE_PUBLISH,
} vk_input_test_lifecycle_stage_t;

typedef enum {
    VK_INPUT_TEST_RESULT_PREPARED = 0,
    VK_INPUT_TEST_RESULT_RUNNING,
    VK_INPUT_TEST_RESULT_CANCELLED,
    VK_INPUT_TEST_RESULT_STOPPED,
    VK_INPUT_TEST_RESULT_RUNTIME_FAILED,
    VK_INPUT_TEST_RESULT_FAILED,
    VK_INPUT_TEST_RESULT_TAINTED,
} vk_input_test_result_kind_t;

typedef void (*vk_input_test_lifecycle_hook_t)(void *context,
                                                vk_input_test_lifecycle_stage_t stage);
typedef void (*vk_input_test_command_hook_t)(void *context);

void vk_input_test_set_audio_api(const vk_audio_control_api_t *api);
void vk_input_test_set_lifecycle_hook(vk_input_test_lifecycle_hook_t hook, void *context);
void vk_input_test_set_command_hook(vk_input_test_command_hook_t hook, void *context);
void vk_input_test_set_task_creation_failure(unsigned ordinal);
vk_usb_input_lifecycle_begin_result_t vk_input_test_begin_lifecycle(
    const vk_usb_input_lifecycle_request_t *request,
    const vk_usb_input_lifecycle_sink_t *sink);
bool vk_input_test_started(void);
bool vk_input_test_enqueue_transition(vk_input_transition_t transition);
bool vk_input_test_fill_ordinary_mailboxes(void);
bool vk_input_test_fill_transition_fifo_and_overflow(void);
void vk_input_test_force_result_overflow(void);
void vk_input_test_force_join_timeout(bool enabled);
bool vk_input_test_inject_result(
    vk_input_test_result_kind_t kind,
    uint32_t epoch,
    uint32_t generation,
    uint32_t session);
uint32_t vk_input_test_active_epoch(void);
uint32_t vk_input_test_generation(void);
uint32_t vk_input_test_session(void);
#endif

#ifdef __cplusplus
}
#endif
