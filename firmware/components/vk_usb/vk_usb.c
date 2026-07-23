#include "vk_usb.h"
#include "vk_usb_runtime.h"
#include "vk_usb_owner.h"
#include "vk_usb_facade.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef VK_USB_PRODUCTION_NATIVE_TEST
#include "vk_usb_native_platform.h"
#else
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define VK_USB_TASK_STACK_BYTES 32768U
#define VK_USB_TASK_PRIORITY 6U
#define VK_USB_DRIVER_BUFFER_BYTES 4096U
#define VK_USB_FACADE_CLOSE_TICKS 3000U

typedef struct {
    vk_usb_service_t *service;
    vk_usb_owner_t owner;
    SemaphoreHandle_t owner_ready;
    SemaphoreHandle_t started;
    SemaphoreHandle_t stopped;
    esp_err_t startup_error;
    esp_err_t runtime_error;
    esp_err_t cleanup_error;
    SemaphoreHandle_t state_mutex;
} production_context_t;

static production_context_t s_context;
static vk_usb_runtime_t s_runtime;
static vk_usb_facade_t s_facade;
static SemaphoreHandle_t s_facade_mutex;
static bool s_runtime_initialized;
static vk_usb_input_handler_registration_t s_input_registration;
static vk_usb_input_lifecycle_registration_t s_input_lifecycle_registration;
static vk_usb_led_lifecycle_registration_t s_led_lifecycle_registration;
static vk_usb_capability_provider_registration_t s_capability_registration;
static vk_usb_asset_handler_registration_t s_asset_registration;
static vk_usb_screen_handler_registration_t s_screen_registration;
static vk_usb_widget_handler_registration_t s_widget_registration;
static vk_usb_led_handler_registration_t s_led_registration;
#ifdef VK_USB_PRODUCTION_NATIVE_TEST
static vk_usb_stop_requested_hook_t s_stop_requested_hook;
static void *s_stop_requested_hook_context;
static vk_usb_stop_requested_hook_t s_poll_returned_hook;
static void *s_poll_returned_hook_context;
#endif

static esp_err_t production_install(void *context)
{
    (void)context;
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = VK_USB_DRIVER_BUFFER_BYTES, .rx_buffer_size = VK_USB_DRIVER_BUFFER_BYTES};
    return usb_serial_jtag_driver_install(&config);
}
static esp_err_t production_uninstall(void *context) { (void)context; return usb_serial_jtag_driver_uninstall(); }
static int production_read(void *context, uint8_t *bytes, size_t capacity, uint32_t timeout_ms) { (void)context; return usb_serial_jtag_read_bytes(bytes, (uint32_t)capacity, pdMS_TO_TICKS(timeout_ms)); }
static int production_write(void *context, const uint8_t *bytes, size_t length, uint32_t timeout_ms) { (void)context; return usb_serial_jtag_write_bytes(bytes, length, pdMS_TO_TICKS(timeout_ms)); }
static uint64_t production_now_ms(void *context) { (void)context; return (uint64_t)(esp_timer_get_time() / 1000); }
static void production_state_lock(void *context) { xSemaphoreTake(((production_context_t *)context)->state_mutex, portMAX_DELAY); }
static void production_state_unlock(void *context) { xSemaphoreGive(((production_context_t *)context)->state_mutex); }
static void production_notify(void *context, void *handle) { (void)context; xTaskNotifyGive((TaskHandle_t)handle); }
static void facade_lock(void *context) { (void)context; xSemaphoreTake(s_facade_mutex, portMAX_DELAY); }
static void facade_unlock(void *context) { (void)context; xSemaphoreGive(s_facade_mutex); }
static void facade_wait_tick(void *context) { (void)context; vTaskDelay(pdMS_TO_TICKS(1U)); }

static void production_task(void *argument)
{
    production_context_t *context = argument;
    xSemaphoreTake(context->owner_ready, portMAX_DELAY);
    context->startup_error = vk_usb_service_start(context->service);
    xSemaphoreGive(context->started);
    if (context->startup_error == ESP_OK) {
        while (ulTaskNotifyTake(pdTRUE, 0) == 0U) {
            esp_err_t error = vk_usb_service_poll(context->service);
#ifdef VK_USB_PRODUCTION_NATIVE_TEST
            vk_usb_stop_requested_hook_t poll_hook;
            void *poll_hook_context;
            production_state_lock(context);
            poll_hook = s_poll_returned_hook;
            poll_hook_context = s_poll_returned_hook_context;
            production_state_unlock(context);
            if (poll_hook != NULL) poll_hook(poll_hook_context);
#endif
            if (error != ESP_OK) {
                /* request_stop closes service admission before notifying. A poll
                 * cancelled by that notification is a clean stop, not runtime failure. */
                if (ulTaskNotifyTake(pdTRUE, 0) == 0U) context->runtime_error = error;
                break;
            }
            if (!vk_usb_service_has_epoch(context->service)) {
                vTaskDelay(pdMS_TO_TICKS(1U));
            }
        }
        context->cleanup_error = vk_usb_service_stop(context->service);
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    vk_usb_owner_quiesce(&context->owner, (void *)self);
    xSemaphoreGive(context->stopped);
    vTaskDelete(NULL);
}

static void release_context(void *argument)
{
    production_context_t *context = argument;
    if (context->owner_ready != NULL) vSemaphoreDelete(context->owner_ready);
    if (context->started != NULL) vSemaphoreDelete(context->started);
    if (context->stopped != NULL) vSemaphoreDelete(context->stopped);
    if (context->state_mutex != NULL) vSemaphoreDelete(context->state_mutex);
    free(context->service);
    *context = (production_context_t){0};
}

static esp_err_t prepare_context(void *argument)
{
    production_context_t *context = argument;
    *context = (production_context_t){0};
    context->service = calloc(1U, vk_usb_service_size());
    context->owner_ready = xSemaphoreCreateBinary();
    context->started = xSemaphoreCreateBinary();
    context->stopped = xSemaphoreCreateBinary();
    context->state_mutex = xSemaphoreCreateMutex();
    if (context->service == NULL || context->owner_ready == NULL || context->started == NULL || context->stopped == NULL || context->state_mutex == NULL) { release_context(context); return ESP_ERR_NO_MEM; }
    vk_usb_owner_ops_t owner_ops = {.lock = production_state_lock, .unlock = production_state_unlock, .notify = production_notify, .context = context};
    vk_usb_owner_init(&context->owner, &owner_ops);
    uint8_t mac[6];
    esp_err_t error = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (error != ESP_OK) { release_context(context); return error; }
    char device_id[32];
    int count = snprintf(device_id, sizeof(device_id), "VS-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (count < 0 || (size_t)count >= sizeof(device_id)) { release_context(context); return ESP_ERR_INVALID_SIZE; }
    const esp_app_desc_t *description = esp_app_get_description();
    vk_usb_identity_t identity = {.hardware = "vibe_keyboard", .firmware_version = description->version, .device_id = device_id};
    vk_usb_transport_ops_t transport = {.install = production_install, .uninstall = production_uninstall, .read = production_read, .write = production_write, .now_ms = production_now_ms, .state_lock = production_state_lock, .state_unlock = production_state_unlock, .context = context};
    vk_usb_service_policy_t policy = {.update_boot_policy_enabled = false};
    error = vk_usb_service_init(context->service, &transport, &identity, &policy);
    if (error == ESP_OK && s_input_registration.handle_command != NULL) {
        error = vk_usb_service_register_input_handler(context->service, &s_input_registration);
    }
    if (error == ESP_OK && s_input_lifecycle_registration.begin != NULL) {
        error = vk_usb_service_register_input_lifecycle(context->service, &s_input_lifecycle_registration);
    }
    if (error == ESP_OK && s_led_lifecycle_registration.begin != NULL) {
        error = vk_usb_service_register_led_lifecycle(context->service, &s_led_lifecycle_registration);
    }
    if (error == ESP_OK && s_capability_registration.get_snapshot != NULL) {
        error = vk_usb_service_register_capability_provider(context->service, &s_capability_registration);
    }
    if (error == ESP_OK && s_asset_registration.handle_command != NULL) {
        error = vk_usb_service_register_asset_handler(context->service, &s_asset_registration);
    }
    if (error == ESP_OK && s_screen_registration.handle_command != NULL) {
        error = vk_usb_service_register_screen_handler(context->service, &s_screen_registration);
    }
    if (error == ESP_OK && s_widget_registration.handle_command != NULL) {
        error = vk_usb_service_register_widget_handler(context->service, &s_widget_registration);
    }
    if (error == ESP_OK && s_led_registration.handle_command != NULL) {
        error = vk_usb_service_register_led_handler(context->service, &s_led_registration);
    }
    if (error != ESP_OK) release_context(context);
    return error;
}

static esp_err_t create_task(void *argument)
{
    production_context_t *context = argument;
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(production_task, "vk_usb", VK_USB_TASK_STACK_BYTES, context, VK_USB_TASK_PRIORITY, &task, tskNO_AFFINITY);
    if (created == pdPASS) {
        vk_usb_owner_attach(&context->owner, (void *)task);
        xSemaphoreGive(context->owner_ready);
    }
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
static bool wait_started(void *argument, uint32_t timeout_ms) { return xSemaphoreTake(((production_context_t *)argument)->started, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
static void request_stop(void *argument)
{
    production_context_t *context = argument;
    /* Close service admission in the same synchronization domain before waking
     * the poll owner. A dequeued value that has not committed is then cancelled. */
    if (context->service != NULL) vk_usb_service_request_stop(context->service);
#ifdef VK_USB_PRODUCTION_NATIVE_TEST
    if (s_stop_requested_hook != NULL) {
        s_stop_requested_hook(s_stop_requested_hook_context);
    }
#endif
    (void)vk_usb_owner_request_stop(&context->owner);
}
static bool wait_stopped(void *argument, uint32_t timeout_ms) { return xSemaphoreTake(((production_context_t *)argument)->stopped, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
static esp_err_t startup_error(void *argument) { return ((production_context_t *)argument)->startup_error; }
static esp_err_t runtime_error(void *argument) { return ((production_context_t *)argument)->runtime_error; }
static esp_err_t cleanup_error(void *argument) { return ((production_context_t *)argument)->cleanup_error; }

static void initialize_runtime(void)
{
    if (s_runtime_initialized) return;
    s_facade_mutex = xSemaphoreCreateMutex();
    if (s_facade_mutex == NULL) return;
    vk_usb_facade_ops_t facade_ops = {
        .lock = facade_lock,
        .unlock = facade_unlock,
        .wait_tick = facade_wait_tick,
        .context = NULL,
    };
    vk_usb_facade_init(&s_facade, &facade_ops);
    vk_usb_runtime_ops_t ops = {.prepare = prepare_context, .create_task = create_task, .wait_started = wait_started, .request_stop = request_stop, .wait_stopped = wait_stopped, .startup_error = startup_error, .runtime_error = runtime_error, .cleanup_error = cleanup_error, .release = release_context, .context = &s_context};
    vk_usb_runtime_init(&s_runtime, &ops);
    s_runtime_initialized = true;
}

esp_err_t vk_usb_start(void)
{
    initialize_runtime();
    if (!s_runtime_initialized) return ESP_ERR_NO_MEM;
    esp_err_t error = vk_usb_runtime_start(&s_runtime);
    if (error != ESP_OK) return error;
    error = vk_usb_facade_publish(&s_facade, s_context.service);
    if (error != ESP_OK) {
        (void)vk_usb_runtime_stop(&s_runtime);
        return error;
    }
    return ESP_OK;
}

esp_err_t vk_usb_stop(void)
{
    initialize_runtime();
    if (!s_runtime_initialized) return ESP_ERR_NO_MEM;
    esp_err_t error = vk_usb_facade_close(&s_facade, VK_USB_FACADE_CLOSE_TICKS);
    if (error != ESP_OK) return error;
    return vk_usb_runtime_stop(&s_runtime);
}

esp_err_t vk_usb_current_epoch(uint32_t *epoch)
{
    if (epoch == NULL) return ESP_ERR_INVALID_ARG;
    initialize_runtime();
    if (!s_runtime_initialized) return ESP_ERR_NO_MEM;
    void *pinned = NULL;
    esp_err_t error = vk_usb_facade_acquire(&s_facade, &pinned);
    if (error != ESP_OK) return error;
    vk_usb_service_t *service = pinned;
    if (!vk_usb_service_has_epoch(service)) {
        error = ESP_ERR_INVALID_STATE;
    } else {
        uint32_t current = vk_usb_service_epoch(service);
        if (current == 0U) error = ESP_ERR_INVALID_STATE;
        else *epoch = current;
    }
    vk_usb_facade_release(&s_facade);
    return error;
}

#if defined(VK_USB_NATIVE_TEST) && defined(VK_USB_PRODUCTION_NATIVE_TEST)
void vk_usb_set_before_tx_commit_hook_for_test(vk_usb_before_tx_commit_hook_t hook,
                                               void *context)
{
    if (s_context.service != NULL) {
        vk_usb_service_set_before_tx_commit_hook_for_test(s_context.service,
                                                         hook, context);
    }
}
void vk_usb_set_stop_requested_hook_for_test(vk_usb_stop_requested_hook_t hook,
                                             void *context)
{
    s_stop_requested_hook = hook;
    s_stop_requested_hook_context = context;
}
void vk_usb_set_poll_returned_hook_for_test(vk_usb_stop_requested_hook_t hook,
                                            void *context)
{
    if (s_context.state_mutex != NULL) production_state_lock(&s_context);
    s_poll_returned_hook = hook;
    s_poll_returned_hook_context = context;
    if (s_context.state_mutex != NULL) production_state_unlock(&s_context);
}
esp_err_t vk_usb_consume_for_test(const uint8_t *bytes, size_t length)
{
    return s_context.service == NULL ? ESP_ERR_INVALID_STATE :
           vk_usb_service_consume_for_test(s_context.service, bytes, length);
}
#endif

esp_err_t vk_usb_register_input_handler(const vk_usb_input_handler_registration_t *registration)
{
    if (registration == NULL || registration->handle_command == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_input_registration.handle_command != NULL) return ESP_ERR_INVALID_STATE;
    s_input_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_input_lifecycle(const vk_usb_input_lifecycle_registration_t *registration)
{
    if (registration == NULL || registration->begin == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_input_lifecycle_registration.begin != NULL) return ESP_ERR_INVALID_STATE;
    s_input_lifecycle_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_led_lifecycle(const vk_usb_led_lifecycle_registration_t *registration)
{
    if (registration == NULL || registration->begin == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_led_lifecycle_registration.begin != NULL) return ESP_ERR_INVALID_STATE;
    s_led_lifecycle_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_capability_provider(const vk_usb_capability_provider_registration_t *registration)
{
    if (registration == NULL || registration->get_snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_capability_registration.get_snapshot != NULL) return ESP_ERR_INVALID_STATE;
    s_capability_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_asset_handler(const vk_usb_asset_handler_registration_t *registration)
{
    if (registration == NULL || registration->handle_command == NULL || registration->handle_chunk == NULL ||
        registration->chunk_bytes == 0U || registration->max_asset_bytes == 0U) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_asset_registration.handle_command != NULL) return ESP_ERR_INVALID_STATE;
    s_asset_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_screen_handler(const vk_usb_screen_handler_registration_t *registration)
{
    if (registration == NULL || registration->handle_command == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_screen_registration.handle_command != NULL) return ESP_ERR_INVALID_STATE;
    s_screen_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_widget_handler(const vk_usb_widget_handler_registration_t *registration)
{
    if (registration == NULL || registration->handle_command == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_widget_registration.handle_command != NULL) return ESP_ERR_INVALID_STATE;
    s_widget_registration = *registration;
    return ESP_OK;
}
esp_err_t vk_usb_register_led_handler(const vk_usb_led_handler_registration_t *registration)
{
    if (registration == NULL || registration->handle_command == NULL) return ESP_ERR_INVALID_ARG;
    if (s_runtime_initialized || s_led_registration.handle_command != NULL) return ESP_ERR_INVALID_STATE;
    s_led_registration = *registration;
    return ESP_OK;
}

vk_usb_handoff_result_t vk_usb_send_button(uint32_t expected_epoch, const vk_usb_button_event_t *event)
{
    void *pinned = NULL;
    if (expected_epoch == 0U || vk_usb_facade_acquire(&s_facade, &pinned) != ESP_OK) return VK_USB_HANDOFF_EPOCH_CLOSED;
    vk_usb_handoff_result_t result = vk_usb_service_send_button_for_epoch(pinned, expected_epoch, event);
    vk_usb_facade_release(&s_facade);
    return result;
}

static esp_err_t with_input_service(uint32_t epoch, esp_err_t (*operation)(vk_usb_service_t *, uint32_t, vk_usb_input_error_t), vk_usb_input_error_t error)
{
    void *pinned = NULL;
    if (epoch == 0U || vk_usb_facade_acquire(&s_facade, &pinned) != ESP_OK) return ESP_ERR_INVALID_STATE;
    esp_err_t result = operation(pinned, epoch, error);
    vk_usb_facade_release(&s_facade);
    return result;
}
esp_err_t vk_usb_send_input_state(uint32_t epoch, vk_usb_input_mode_t mode, vk_usb_key_t key){void*p=NULL;if(epoch==0U||vk_usb_facade_acquire(&s_facade,&p)!=ESP_OK)return ESP_ERR_INVALID_STATE;esp_err_t e=vk_usb_service_send_input_state_for_epoch(p,epoch,mode,key);vk_usb_facade_release(&s_facade);return e;}
esp_err_t vk_usb_send_input_error(uint32_t epoch, vk_usb_input_error_t error){return with_input_service(epoch,vk_usb_service_send_input_error_for_epoch,error);}
esp_err_t vk_usb_fail_input_epoch(uint32_t epoch, vk_usb_input_error_t error){return with_input_service(epoch,vk_usb_service_fail_input_epoch,error);}

esp_err_t vk_usb_send_audio(uint32_t expected_epoch, const vk_usb_audio_frame_t *frame)
{
    if (expected_epoch == 0U) return ESP_ERR_INVALID_STATE;
    initialize_runtime();
    if (!s_runtime_initialized) return ESP_ERR_NO_MEM;
    void *pinned = NULL;
    esp_err_t error = vk_usb_facade_acquire(&s_facade, &pinned);
    if (error != ESP_OK) return error;
    error = vk_usb_service_send_audio_for_epoch((vk_usb_service_t *)pinned,
                                                expected_epoch, frame);
    vk_usb_facade_release(&s_facade);
    return error;
}
