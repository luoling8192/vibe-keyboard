#include <stdbool.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vk_audio.h"
#include "vk_board.h"
#include "vk_display.h"
#include "vk_input.h"
#include "vk_led.h"
#include "vk_screen_service.h"
#include "vk_update.h"
#include "vk_usb.h"

#define LED_OWNER_PERIOD_MS 50U
#define LED_OWNER_STACK_WORDS 2048U
#define LED_OWNER_PRIORITY 2U

static vk_led_t *s_led;
static vk_usb_capability_provider_registration_t s_screen_capabilities;
static bool s_has_screen_capabilities;
static bool s_screen_prepared;
static TaskHandle_t s_led_owner_task;

static void led_owner_task_fn(void *arg)
{
    vk_led_t *led = arg;
    for (;;) {
        if (vk_led_process_lifecycle(led) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(LED_OWNER_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

static esp_err_t product_capabilities(void *context, uint32_t epoch,
                                      vk_usb_capability_snapshot_t *snapshot)
{
    (void)context;
    if (snapshot == NULL || epoch == 0U || s_led == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = (vk_usb_capability_snapshot_t){0};
    if (s_has_screen_capabilities) {
        esp_err_t result = s_screen_capabilities.get_snapshot(
            s_screen_capabilities.context, epoch, snapshot);
        if (result != ESP_OK) return result;
    }
    vk_led_state_t state;
    vk_led_state(s_led, &state);
    snapshot->led.state = VK_USB_CAPABILITY_UNAVAILABLE;
    snapshot->led.unavailable_reason = state.unavailable_reason == VK_LED_UNAVAILABLE_TAINTED
        ? VK_USB_LED_REASON_TAINTED
        : state.unavailable_reason == VK_LED_UNAVAILABLE_HARDWARE_FAILED
            ? VK_USB_LED_REASON_HARDWARE_FAILED
            : VK_USB_LED_REASON_CALIBRATION_REQUIRED;
    return ESP_OK;
}

static vk_usb_input_lifecycle_begin_result_t product_led_lifecycle_begin(
    void *context, const vk_usb_input_lifecycle_request_t *request,
    const vk_usb_input_lifecycle_sink_t *sink)
{
    vk_led_lifecycle_request_t led_request = {
        .kind = request->kind == VK_USB_INPUT_LIFECYCLE_STOPPING
            ? VK_LED_LIFECYCLE_STOPPING : VK_LED_LIFECYCLE_EPOCH_OFF,
        .token = request->token,
        .old_epoch = request->old_epoch,
        .proposed_epoch = request->proposed_epoch,
        .lifecycle_generation = request->lifecycle_generation,
        .absolute_deadline_ms = request->absolute_deadline_ms,
    };
    /* Nonblocking: only accept the obligation. The LED owner task
     * executes transport cleanup and acknowledgement asynchronously. */
    vk_led_lifecycle_begin_result_t result = vk_led_begin_lifecycle(context, &led_request,
        (vk_led_lifecycle_publish_t)sink->publish, sink->context);
    return result == VK_LED_LIFECYCLE_ACCEPTED
        ? VK_USB_INPUT_LIFECYCLE_ACCEPTED : VK_USB_INPUT_LIFECYCLE_TAINTED;
}

void app_main(void)
{
    vk_led_t *led = calloc(1U, vk_led_size());
    s_led = led;
    vk_update_t update;
    vk_update_init(&update, NULL); /* Pre-migration fail-closed state; no flash backend. */
    esp_err_t error = led == NULL ? ESP_ERR_NO_MEM : vk_led_init_fail_dark(led);
    if (error != ESP_OK) {
        free(led);
        return;
    }
    error = vk_board_init();
    if (error != ESP_OK) {
        (void)vk_board_deinit();
        (void)vk_led_stop(led);
        free(led);
        return;
    }

    /* Display transport is initialized, but screen availability remains false
     * until storage, screen owner, fonts, and physical acceptance are admitted. */
    error = vk_display_product_init();
    if (error != ESP_OK) {
        (void)vk_display_product_deinit();
        (void)vk_board_deinit();
        (void)vk_led_stop(led);
        free(led);
        return;
    }

    /* A factory-erased storage partition is formatted after exact validation.
     * Production screen/assets stay unavailable while the compiled font and
     * physical acceptance evidence gates are false. */
    error = vk_screen_product_prepare();
    if (error == ESP_OK) {
        s_screen_prepared = true;
        vk_usb_capability_provider_registration_t capabilities;
        vk_usb_asset_handler_registration_t assets;
        vk_usb_screen_handler_registration_t screen;
        vk_usb_widget_handler_registration_t widget;
        vk_screen_product_registrations(&capabilities, &assets, &screen, &widget);
        s_screen_capabilities = capabilities;
        s_has_screen_capabilities = true;
        if (error == ESP_OK) error = vk_usb_register_asset_handler(&assets);
        if (error == ESP_OK) error = vk_usb_register_screen_handler(&screen);
        if (error == ESP_OK) error = vk_usb_register_widget_handler(&widget);
    }

    /* LED capability and lifecycle are independent from screen/storage success. */
    if (error != ESP_OK && !s_screen_prepared) error = ESP_OK;
    if (error == ESP_OK) {
        vk_usb_capability_provider_registration_t provider = {
            .get_snapshot = product_capabilities,
            .context = NULL,
        };
        vk_usb_led_lifecycle_registration_t lifecycle = {
            .begin = product_led_lifecycle_begin,
            .context = led,
        };
        if (error == ESP_OK) error = vk_usb_register_capability_provider(&provider);
        if (error == ESP_OK) error = vk_usb_register_led_lifecycle(&lifecycle);
        /* Start the async LED owner task that executes transport cleanup
         * and lifecycle acknowledgement. */
        if (error == ESP_OK && pdPASS != xTaskCreate(led_owner_task_fn, "led_owner",
            LED_OWNER_STACK_WORDS, led, LED_OWNER_PRIORITY, &s_led_owner_task)) {
            error = ESP_ERR_NO_MEM;
        }
    }

    /* Input registers its typed command boundary before USB service start. */
    if (error == ESP_OK) error = vk_audio_init();
    if (error == ESP_OK) error = vk_input_init();
    if (error == ESP_OK) error = vk_usb_start();
    if (error == ESP_OK) error = vk_input_start();
    if (error != ESP_OK) {
        /* USB owns epoch closure while the input lifecycle consumer is alive. */
        esp_err_t usb_stop = vk_usb_stop();
        esp_err_t input_stop = usb_stop == ESP_OK ? vk_input_stop() : ESP_ERR_INVALID_STATE;
        if (s_led_owner_task != NULL) vTaskDelete(s_led_owner_task);
        if (usb_stop != ESP_OK || input_stop != ESP_OK) {
            /* Ownership is intentionally retained on an unproven teardown. */
            return;
        }
        (void)vk_input_deinit();
        (void)vk_audio_deinit();
        (void)vk_screen_product_stop();
        (void)vk_display_product_deinit();
        (void)vk_board_deinit();
        (void)vk_led_stop(led);
        free(led);
        return;
    }
}
