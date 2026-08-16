#include "vk_input.h"

#include <stdatomic.h>
#include <string.h>

#ifdef VK_INPUT_NATIVE_TEST
#include "vk_input_native_platform.h"
#include "vk_input_test.h"
#else
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif
#include "vk_audio.h"
#ifndef VK_INPUT_NATIVE_TEST
#include "vk_board.h"
#endif
#include "vk_usb.h"

#ifndef VK_INPUT_NATIVE_TEST
_Static_assert(CONFIG_FREERTOS_HZ == 1000,
               "Production firmware requires a 1000 Hz FreeRTOS tick rate");
#endif
_Static_assert(pdMS_TO_TICKS(VK_KEY_SCAN_PERIOD_MS) > 0U,
               "VK_KEY_SCAN_PERIOD_MS must be representable by the FreeRTOS tick rate");

#define VK_INPUT_SCANNER_STACK 3072U
#define VK_INPUT_OWNER_STACK 5120U
#define VK_INPUT_AUDIO_STACK 4096U
#define VK_INPUT_TASK_PRIORITY 5U
#define VK_INPUT_JOIN_TIMEOUT_MS 3250U

typedef enum { AUDIO_PREPARE = 0, AUDIO_RELEASE, AUDIO_CANCEL, AUDIO_STOP, AUDIO_ABORT } audio_command_kind_t;
typedef enum { AUDIO_PREPARED = 0, AUDIO_RUNNING, AUDIO_CANCELLED, AUDIO_STOPPED, AUDIO_RUNTIME_FAILED, AUDIO_FAILED, AUDIO_TAINTED } audio_result_kind_t;
typedef struct { audio_command_kind_t kind; uint32_t epoch, generation, session; } audio_command_t;
typedef struct { audio_result_kind_t kind; audio_command_kind_t command; uint32_t epoch, generation, session; } audio_result_t;
typedef enum { VOICE_IDLE = 0, VOICE_PREPARING, VOICE_PREPARED, VOICE_RELEASING, VOICE_RUNNING, VOICE_CANCELLING, VOICE_STOPPING, VOICE_TAINTED } voice_state_t;
typedef enum { LIFECYCLE_IDLE = 0, LIFECYCLE_QUEUED, LIFECYCLE_EXECUTING } lifecycle_state_t;
typedef struct { vk_usb_input_lifecycle_request_t request; vk_usb_input_lifecycle_sink_t sink; } lifecycle_item_t;

typedef struct {
    QueueHandle_t transitions;
    QueueHandle_t commands;
    QueueHandle_t results;
    QueueHandle_t lifecycle_wake;
    QueueHandle_t configurations;
    SemaphoreHandle_t control_mutex;
    SemaphoreHandle_t scanner_done;
    SemaphoreHandle_t owner_done;
    SemaphoreHandle_t audio_done;
    TaskHandle_t scanner_task;
    TaskHandle_t owner_task;
    TaskHandle_t audio_task;
    bool scanner_started;
    bool owner_started;
    bool audio_started;
    lifecycle_state_t lifecycle_state;
    lifecycle_item_t lifecycle_item;
    uint32_t lifecycle_barrier;
    uint32_t last_quiescent_old_epoch;
    bool last_quiescent_valid;
    atomic_bool stopping;
    atomic_bool admission_open;
    atomic_bool local_failure;
    atomic_bool result_overflow;
    atomic_bool tainted;
    atomic_uint scanner_generation;
    atomic_uint active_epoch;
    atomic_uint barrier_generation;
    atomic_uint scanner_quiescent;
    atomic_uint owner_quiescent;
    atomic_uint protocol_callbacks_in_flight;
    vk_input_debounce_t debounce; /* scanner-owned */
    /* The remaining protocol/voice fields are input-owner-owned. */
    vk_usb_input_mode_t mode;
    vk_usb_key_t voice_key;
    uint32_t generation;
    uint32_t session;
    uint32_t runtime_generation;
    voice_state_t voice;
    bool voice_disabled;
    bool prepared_identity_only;
} input_context_t;

static input_context_t s_input;
static const vk_audio_control_api_t *s_audio;
#ifdef VK_INPUT_NATIVE_TEST
static atomic_bool s_test_force_join_timeout;
static vk_input_test_lifecycle_hook_t s_test_lifecycle_hook;
static void *s_test_lifecycle_hook_context;
static vk_input_test_command_hook_t s_test_command_hook;
static void *s_test_command_hook_context;
static void test_lifecycle_stage(vk_input_test_lifecycle_stage_t stage)
{
    if (s_test_lifecycle_hook != NULL) s_test_lifecycle_hook(s_test_lifecycle_hook_context, stage);
}
#else
#define test_lifecycle_stage(...) ((void)0)
#endif

static uint32_t next_nonzero(uint32_t value) { ++value; return value == 0U ? 1U : value; }
static uint32_t now_ms(void) { return (uint32_t)((uint64_t)esp_timer_get_time() / 1000U); }
static void control_lock(void) { xSemaphoreTake(s_input.control_mutex, portMAX_DELAY); }
static void control_unlock(void) { xSemaphoreGive(s_input.control_mutex); }
static uint32_t active_epoch(void) { return atomic_load_explicit(&s_input.active_epoch, memory_order_acquire); }

static void close_local_admission(void)
{
    atomic_store_explicit(&s_input.admission_open, false, memory_order_release);
    atomic_fetch_add_explicit(&s_input.scanner_generation, 1U, memory_order_acq_rel);
}

/* Input-owner only. */
static void fail_epoch_owned(vk_usb_input_error_t error)
{
    close_local_admission();
    atomic_store_explicit(&s_input.tainted, true, memory_order_release);
    s_input.voice = VOICE_TAINTED;
    uint32_t epoch = active_epoch();
    if (epoch != 0U) (void)vk_usb_fail_input_epoch(epoch, error);
}

/* Input-owner only. */
static bool submit_audio(audio_command_kind_t kind, uint32_t session)
{
    audio_command_t command = {.kind=kind,.epoch=active_epoch(),.generation=next_nonzero(s_input.generation),.session=session};
    s_input.generation = command.generation;
    if (!atomic_load_explicit(&s_input.admission_open, memory_order_acquire) ||
        xQueueSend(s_input.commands, &command, 0U) != pdTRUE) {
        fail_epoch_owned(VK_USB_INPUT_ERROR_QUEUE_OVERFLOW);
        return false;
    }
    return true;
}

static void scanner_task(void *argument)
{
    (void)argument;
    TickType_t wake = xTaskGetTickCount();
    const TickType_t scan_period_ticks = pdMS_TO_TICKS(VK_KEY_SCAN_PERIOD_MS);
    unsigned generation = atomic_load_explicit(&s_input.scanner_generation, memory_order_acquire);
    uint32_t barrier = 0U;
    vk_input_debounce_reset(&s_input.debounce);
    while (!atomic_load_explicit(&s_input.stopping, memory_order_acquire)) {
        uint32_t requested_barrier = atomic_load_explicit(&s_input.barrier_generation, memory_order_acquire);
        if (requested_barrier != barrier) {
            barrier = requested_barrier;
            vk_input_debounce_reset(&s_input.debounce);
            atomic_store_explicit(&s_input.scanner_quiescent, barrier, memory_order_release);
        }
        unsigned current = atomic_load_explicit(&s_input.scanner_generation, memory_order_acquire);
        if (current != generation) { generation = current; vk_input_debounce_reset(&s_input.debounce); }
        uint8_t mask = 0U;
        for (size_t index = 0; index < VK_KEY_COUNT; ++index) {
            if (gpio_get_level(vk_key_gpios[index]) == VK_KEY_ACTIVE_LEVEL) mask |= (uint8_t)(1U << index);
        }
        if (atomic_load_explicit(&s_input.admission_open, memory_order_acquire)) {
            vk_input_transition_t values[8];
            size_t count = vk_input_debounce_sample(&s_input.debounce, mask, now_ms(), values, 8U);
            if (count > 8U) atomic_store_explicit(&s_input.local_failure, true, memory_order_release);
            else for (size_t index = 0; index < count; ++index) {
                if (xQueueSend(s_input.transitions, &values[index], 0U) != pdTRUE) {
                    atomic_store_explicit(&s_input.local_failure, true, memory_order_release);
                    close_local_admission();
                    break;
                }
            }
        }
        vTaskDelayUntil(&wake, scan_period_ticks);
    }
    /* Completion is the join proof; task handles are controller-owned. */
    xSemaphoreGive(s_input.scanner_done);
    vTaskDelete(NULL);
}

static audio_result_t execute_audio(audio_command_t command)
{
    audio_result_t result = {.kind=AUDIO_FAILED,.command=command.kind,.epoch=command.epoch,.generation=command.generation,.session=command.session};
    esp_err_t error = ESP_ERR_INVALID_STATE;
    switch (command.kind) {
    case AUDIO_PREPARE: error=s_audio->prepare(&result.session);result.kind=error==ESP_OK?AUDIO_PREPARED:(vk_audio_is_tainted()?AUDIO_TAINTED:AUDIO_FAILED);break;
    case AUDIO_RELEASE: error=s_audio->release(command.session);result.kind=error==ESP_OK?AUDIO_RUNNING:(vk_audio_is_tainted()?AUDIO_TAINTED:AUDIO_FAILED);break;
    case AUDIO_CANCEL: error=s_audio->cancel_prepared(command.session);result.kind=error==ESP_OK?AUDIO_CANCELLED:AUDIO_TAINTED;break;
    case AUDIO_STOP: error=s_audio->stop();result.kind=error==ESP_OK?AUDIO_STOPPED:(vk_audio_is_tainted()?AUDIO_TAINTED:AUDIO_FAILED);break;
    case AUDIO_ABORT: error=s_audio->abort();result.kind=error==ESP_OK?AUDIO_CANCELLED:AUDIO_TAINTED;break;
    }
    return result;
}

static void complete_lifecycle(const lifecycle_item_t *item, vk_usb_input_lifecycle_ack_result_t result)
{
    if (item->sink.publish == NULL) return;
    vk_usb_input_lifecycle_ack_t ack={.token=item->request.token,.lifecycle_generation=item->request.lifecycle_generation,.result=result};
    (void)item->sink.publish(item->sink.context,&ack);
}

static bool owners_quiescent(uint32_t barrier)
{
    return atomic_load_explicit(&s_input.scanner_quiescent, memory_order_acquire) == barrier &&
           atomic_load_explicit(&s_input.owner_quiescent, memory_order_acquire) == barrier &&
           atomic_load_explicit(&s_input.protocol_callbacks_in_flight, memory_order_acquire) == 0U;
}

static void audio_task(void *argument)
{
    (void)argument;
    uint32_t runtime_epoch=0U,runtime_generation=0U,runtime_session=0U;
    while (!atomic_load_explicit(&s_input.stopping, memory_order_acquire)) {
        uint32_t wake;
        if (xQueueReceive(s_input.lifecycle_wake, &wake, 0U) == pdTRUE) {
            (void)wake;
            control_lock();
            if (s_input.lifecycle_state == LIFECYCLE_QUEUED) s_input.lifecycle_state = LIFECYCLE_EXECUTING;
            uint32_t barrier = s_input.lifecycle_barrier;
            control_unlock();
            while (!owners_quiescent(barrier) && !atomic_load_explicit(&s_input.stopping, memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(1U));
            }
            test_lifecycle_stage(VK_INPUT_TEST_LIFECYCLE_OWNERS_QUIESCENT);
            audio_result_t abort_result={.kind=AUDIO_CANCELLED,.command=AUDIO_ABORT,.epoch=0U,.generation=barrier,.session=0U};
            control_lock();
            uint32_t old_epoch = s_input.lifecycle_item.request.old_epoch;
            control_unlock();
            test_lifecycle_stage(VK_INPUT_TEST_LIFECYCLE_BEFORE_ABORT);
            if (old_epoch != 0U) abort_result=execute_audio((audio_command_t){.kind=AUDIO_ABORT,.epoch=old_epoch,.generation=barrier,.session=0U});
            runtime_epoch=runtime_generation=runtime_session=0U;
            xQueueReset(s_input.commands);
            xQueueReset(s_input.results);
            lifecycle_item_t completed;
            vk_usb_input_lifecycle_ack_result_t result=VK_USB_INPUT_LIFECYCLE_ACK_TAINTED;
            control_lock();
            completed=s_input.lifecycle_item; /* Includes any in-flight stopping retarget. */
            if (abort_result.kind==AUDIO_CANCELLED && owners_quiescent(barrier)) {
                atomic_store_explicit(&s_input.active_epoch,
                    completed.request.kind==VK_USB_INPUT_LIFECYCLE_NEW_EPOCH?completed.request.proposed_epoch:0U,
                    memory_order_release);
                atomic_store_explicit(&s_input.local_failure,false,memory_order_release);
                atomic_store_explicit(&s_input.result_overflow,false,memory_order_release);
                atomic_store_explicit(&s_input.admission_open,completed.request.kind==VK_USB_INPUT_LIFECYCLE_NEW_EPOCH,memory_order_release);
                s_input.last_quiescent_old_epoch=completed.request.old_epoch;
                s_input.last_quiescent_valid=true;
                result=VK_USB_INPUT_LIFECYCLE_QUIESCENT;
            } else {
                atomic_store_explicit(&s_input.tainted,true,memory_order_release);
            }
            s_input.lifecycle_state=LIFECYCLE_IDLE;
            control_unlock();
            test_lifecycle_stage(VK_INPUT_TEST_LIFECYCLE_BEFORE_PUBLISH);
            complete_lifecycle(&completed,result);
            continue;
        }
        uint32_t failed_session=0U;
        if (runtime_session!=0U && s_audio->take_runtime_failure!=NULL &&
            s_audio->take_runtime_failure(&failed_session) &&
            failed_session==runtime_session) {
            audio_result_t result={.kind=AUDIO_RUNTIME_FAILED,.command=AUDIO_RELEASE,.epoch=runtime_epoch,.generation=runtime_generation,.session=runtime_session};
            runtime_epoch=runtime_generation=runtime_session=0U;
            if (xQueueSend(s_input.results,&result,0U)!=pdTRUE) atomic_store_explicit(&s_input.result_overflow,true,memory_order_release);
            continue;
        }
        if (runtime_session!=0U && vk_audio_is_tainted()) {
            audio_result_t result={.kind=AUDIO_TAINTED,.command=AUDIO_RELEASE,.epoch=runtime_epoch,.generation=runtime_generation,.session=runtime_session};
            runtime_epoch=runtime_generation=runtime_session=0U;
            if (xQueueSend(s_input.results,&result,0U)!=pdTRUE) atomic_store_explicit(&s_input.result_overflow,true,memory_order_release);
            continue;
        }
        audio_command_t command;
        if (xQueueReceive(s_input.commands, &command, pdMS_TO_TICKS(10U)) != pdTRUE) continue;
        audio_result_t result = execute_audio(command);
        if(result.kind==AUDIO_RUNNING){runtime_epoch=result.epoch;runtime_generation=result.generation;runtime_session=result.session;}
        else if(command.kind==AUDIO_STOP||command.kind==AUDIO_ABORT){runtime_epoch=runtime_generation=runtime_session=0U;}
        if (xQueueSend(s_input.results, &result, 0U) != pdTRUE) atomic_store_explicit(&s_input.result_overflow,true,memory_order_release);
    }
    xSemaphoreGive(s_input.audio_done);
    vTaskDelete(NULL);
}

static vk_usb_button_event_t button_value(vk_input_transition_t value, bool use_session)
{
    return (vk_usb_button_event_t){.kind=value.kind==VK_INPUT_TRANSITION_DOWN?VK_USB_BUTTON_DOWN:(value.kind==VK_INPUT_TRANSITION_UP?VK_USB_BUTTON_UP:VK_USB_BUTTON_CLICK),.key=(vk_usb_key_t)value.key,.has_session_id=use_session,.session_id=use_session?s_input.session:0U,.has_duration_ms=value.kind!=VK_INPUT_TRANSITION_DOWN,.duration_ms=value.duration_ms};
}
static bool handoff(vk_input_transition_t value,bool use_session)
{
    vk_usb_button_event_t event=button_value(value,use_session);
    while(atomic_load_explicit(&s_input.admission_open,memory_order_acquire)){
        vk_usb_handoff_result_t result=vk_usb_send_button(active_epoch(),&event);
        if(result==VK_USB_HANDOFF_ACCEPTED)return true;
        if(result==VK_USB_HANDOFF_RETRY){vTaskDelay(pdMS_TO_TICKS(1U));continue;}
        if(result==VK_USB_HANDOFF_OVERFLOW)fail_epoch_owned(VK_USB_INPUT_ERROR_QUEUE_OVERFLOW);
        else close_local_admission();
        return false;
    }
    return false;
}

static void stable_filter_voice(void)
{
    vk_input_transition_t kept[VK_INPUT_FIFO_CAPACITY];size_t count=0;vk_input_transition_t value;
    while(xQueueReceive(s_input.transitions,&value,0U)==pdTRUE)if(value.key!=(vk_input_key_t)s_input.voice_key)kept[count++]=value;
    for(size_t i=0;i<count;++i)(void)xQueueSend(s_input.transitions,&kept[i],0U);
}

static void consume_result(audio_result_t result)
{
    uint32_t epoch=active_epoch();
    if(result.kind==AUDIO_RUNTIME_FAILED){
        if(result.epoch!=epoch||result.generation!=s_input.runtime_generation||result.session!=s_input.session)return;
        s_input.voice=VOICE_IDLE;s_input.session=0U;s_input.runtime_generation=0U;s_input.voice_disabled=true;stable_filter_voice();(void)vk_usb_send_input_error(epoch,VK_USB_INPUT_ERROR_AUDIO_RUNTIME_FAILED);return;
    }
    if(result.epoch!=epoch||result.generation!=s_input.generation)return;
    if(result.kind==AUDIO_TAINTED){fail_epoch_owned(VK_USB_INPUT_ERROR_TAINTED);return;}
    switch(result.command){
    case AUDIO_PREPARE:
        if(result.kind==AUDIO_PREPARED&&result.session!=0U){s_input.session=result.session;s_input.voice=VOICE_PREPARED;if(s_input.mode==VK_USB_INPUT_MODE_HOLD_TO_TALK){vk_input_transition_t down={.kind=VK_INPUT_TRANSITION_DOWN,.key=(vk_input_key_t)s_input.voice_key};if(handoff(down,true)&&submit_audio(AUDIO_RELEASE,s_input.session))s_input.voice=VOICE_RELEASING;}else if(submit_audio(AUDIO_RELEASE,s_input.session))s_input.voice=VOICE_RELEASING;}
        else{s_input.voice=VOICE_IDLE;s_input.session=0U;if(s_input.mode==VK_USB_INPUT_MODE_HOLD_TO_TALK){vk_input_transition_t down={.kind=VK_INPUT_TRANSITION_DOWN,.key=(vk_input_key_t)s_input.voice_key};(void)handoff(down,false);}(void)vk_usb_send_input_error(epoch,VK_USB_INPUT_ERROR_AUDIO_START_FAILED);}break;
    case AUDIO_RELEASE:
        if(result.kind==AUDIO_RUNNING){s_input.voice=VOICE_RUNNING;s_input.runtime_generation=result.generation;}
        else{(void)vk_usb_send_input_error(epoch,VK_USB_INPUT_ERROR_AUDIO_START_FAILED);s_input.voice=VOICE_CANCELLING;s_input.prepared_identity_only=true;(void)submit_audio(AUDIO_CANCEL,s_input.session);}break;
    case AUDIO_CANCEL:
        if(result.kind==AUDIO_CANCELLED){s_input.voice=VOICE_IDLE;if(s_input.mode==VK_USB_INPUT_MODE_CLICK_TO_TALK){s_input.session=0U;s_input.prepared_identity_only=false;}}
        else fail_epoch_owned(VK_USB_INPUT_ERROR_TAINTED);
        break;
    case AUDIO_STOP:
        s_input.runtime_generation=0U;
        if(result.kind==AUDIO_STOPPED){s_input.voice=VOICE_IDLE;s_input.session=0U;}
        else{s_input.voice=VOICE_IDLE;s_input.session=0U;s_input.voice_disabled=true;stable_filter_voice();(void)vk_usb_send_input_error(epoch,VK_USB_INPUT_ERROR_AUDIO_STOP_FAILED);}break;
    case AUDIO_ABORT:s_input.voice=VOICE_IDLE;s_input.session=0U;s_input.runtime_generation=0U;break;
    }
}

static void process_transition(vk_input_transition_t value)
{
#if defined(ESP_PLATFORM) || defined(VK_INPUT_STANDARD_MICROPHONE_TEST)
    /* UAC is the sole audio path in production. Physical keys remain ordinary
     * control events so the host can map them to push-to-talk shortcuts while
     * macOS reads the standard VibeBoard microphone independently. */
    (void)handoff(value, false);
    return;
#else
    bool voice=value.key==(vk_input_key_t)s_input.voice_key;
    if(!voice||s_input.voice_disabled){if(!voice)(void)handoff(value,false);return;}
    if(s_input.mode==VK_USB_INPUT_MODE_HOLD_TO_TALK){
        if(value.kind==VK_INPUT_TRANSITION_DOWN&&s_input.voice==VOICE_IDLE){s_input.voice=VOICE_PREPARING;(void)submit_audio(AUDIO_PREPARE,0U);return;}
        if((value.kind==VK_INPUT_TRANSITION_UP||value.kind==VK_INPUT_TRANSITION_CLICK)&&(s_input.voice==VOICE_RUNNING||s_input.prepared_identity_only||s_input.voice==VOICE_IDLE)){
            bool associated=s_input.session!=0U;if(handoff(value,associated)&&value.kind==VK_INPUT_TRANSITION_CLICK){if(s_input.voice==VOICE_RUNNING){s_input.voice=VOICE_STOPPING;(void)submit_audio(AUDIO_STOP,s_input.session);}else{s_input.session=0U;s_input.prepared_identity_only=false;}}return;
        }
    }else{
        bool active=s_input.voice==VOICE_RUNNING;if(!handoff(value,active))return;
        if(value.kind==VK_INPUT_TRANSITION_CLICK){if(active){s_input.voice=VOICE_STOPPING;(void)submit_audio(AUDIO_STOP,s_input.session);}else if(s_input.voice==VOICE_IDLE){s_input.voice=VOICE_PREPARING;(void)submit_audio(AUDIO_PREPARE,0U);}}
    }
#endif
}

static void reset_owner_for_barrier(uint32_t barrier)
{
    xQueueReset(s_input.transitions);
    xQueueReset(s_input.configurations);
    s_input.mode=VK_USB_INPUT_MODE_HOLD_TO_TALK;s_input.voice_key=VK_USB_KEY_K4;
    s_input.voice=VOICE_IDLE;s_input.session=0U;s_input.runtime_generation=0U;
    s_input.voice_disabled=false;s_input.prepared_identity_only=false;
    atomic_store_explicit(&s_input.owner_quiescent,barrier,memory_order_release);
}

static void owner_task(void *argument)
{
    (void)argument;
    uint32_t barrier=0U;
    while(!atomic_load_explicit(&s_input.stopping,memory_order_acquire)){
        uint32_t requested=atomic_load_explicit(&s_input.barrier_generation,memory_order_acquire);
        if(requested!=barrier){barrier=requested;reset_owner_for_barrier(barrier);}
        if(!atomic_load_explicit(&s_input.admission_open,memory_order_acquire)){vTaskDelay(pdMS_TO_TICKS(1U));continue;}
        if(atomic_exchange_explicit(&s_input.local_failure,false,memory_order_acq_rel)||atomic_exchange_explicit(&s_input.result_overflow,false,memory_order_acq_rel)){fail_epoch_owned(VK_USB_INPUT_ERROR_QUEUE_OVERFLOW);continue;}
        vk_usb_input_command_t configuration;
        while(xQueueReceive(s_input.configurations,&configuration,0U)==pdTRUE){
            uint32_t epoch=active_epoch();
            if(configuration.expected_epoch!=epoch||s_input.voice!=VOICE_IDLE||s_input.session!=0U)continue;
            if(configuration.kind==VK_USB_INPUT_COMMAND_MODE)s_input.mode=configuration.mode;else s_input.voice_key=configuration.key;
            (void)vk_usb_send_input_state(epoch,s_input.mode,s_input.voice_key);
        }
        audio_result_t result;while(xQueueReceive(s_input.results,&result,0U)==pdTRUE)consume_result(result);
        bool wait=s_input.voice==VOICE_PREPARING||s_input.voice==VOICE_PREPARED||s_input.voice==VOICE_RELEASING||s_input.voice==VOICE_CANCELLING||s_input.voice==VOICE_STOPPING;
        if(wait){vTaskDelay(pdMS_TO_TICKS(1U));continue;}
        vk_input_transition_t value;if(xQueueReceive(s_input.transitions,&value,pdMS_TO_TICKS(5U))==pdTRUE)process_transition(value);
    }
    xSemaphoreGive(s_input.owner_done);
    vTaskDelete(NULL);
}

static esp_err_t handle_input_command(void *context,const vk_usb_input_command_t *command)
{
    (void)context;
    atomic_fetch_add_explicit(&s_input.protocol_callbacks_in_flight,1U,memory_order_acq_rel);
#ifdef VK_INPUT_NATIVE_TEST
    if(s_test_command_hook!=NULL)s_test_command_hook(s_test_command_hook_context);
#endif
    esp_err_t result=ESP_ERR_INVALID_STATE;
    if(command!=NULL&&command->expected_epoch!=0U&&
       atomic_load_explicit(&s_input.admission_open,memory_order_acquire)&&
       command->expected_epoch==active_epoch()&&
       xQueueSend(s_input.configurations,command,0U)==pdTRUE)result=ESP_OK;
    atomic_fetch_sub_explicit(&s_input.protocol_callbacks_in_flight,1U,memory_order_acq_rel);
    return result;
}

static vk_usb_input_lifecycle_begin_result_t begin_lifecycle(void *context,const vk_usb_input_lifecycle_request_t *request,const vk_usb_input_lifecycle_sink_t *sink)
{
    (void)context;
    if(request==NULL||sink==NULL||sink->publish==NULL||request->token==0U||request->lifecycle_generation==0U)return VK_USB_INPUT_LIFECYCLE_TAINTED;
    close_local_admission();
    lifecycle_item_t direct={0};bool publish_direct=false;uint32_t wake=0U;
    control_lock();
    if(s_input.lifecycle_state!=LIFECYCLE_IDLE){
        if(request->kind!=VK_USB_INPUT_LIFECYCLE_STOPPING||s_input.lifecycle_item.request.old_epoch!=request->old_epoch){control_unlock();return VK_USB_INPUT_LIFECYCLE_TAINTED;}
        /* Retarget the one queued/executing cleanup proof; never enqueue another abort. */
        s_input.lifecycle_item=(lifecycle_item_t){.request=*request,.sink=*sink};
        control_unlock();return VK_USB_INPUT_LIFECYCLE_ACCEPTED;
    }
    if(request->kind==VK_USB_INPUT_LIFECYCLE_STOPPING&&s_input.last_quiescent_valid&&
       s_input.last_quiescent_old_epoch==request->old_epoch&&active_epoch()==0U){
        direct=(lifecycle_item_t){.request=*request,.sink=*sink};publish_direct=true;
    }else{
        s_input.lifecycle_item=(lifecycle_item_t){.request=*request,.sink=*sink};
        s_input.lifecycle_state=LIFECYCLE_QUEUED;
        s_input.lifecycle_barrier=next_nonzero(atomic_load_explicit(&s_input.barrier_generation,memory_order_relaxed));
        atomic_store_explicit(&s_input.barrier_generation,s_input.lifecycle_barrier,memory_order_release);
        wake=s_input.lifecycle_barrier;
    }
    control_unlock();
    if(publish_direct){complete_lifecycle(&direct,VK_USB_INPUT_LIFECYCLE_QUIESCENT);return VK_USB_INPUT_LIFECYCLE_ACCEPTED;}
    if(xQueueSend(s_input.lifecycle_wake,&wake,0U)!=pdTRUE){atomic_store_explicit(&s_input.tainted,true,memory_order_release);return VK_USB_INPUT_LIFECYCLE_TAINTED;}
    return VK_USB_INPUT_LIFECYCLE_ACCEPTED;
}

static void delete_resources(void)
{
    if(s_input.transitions) vQueueDelete(s_input.transitions);
    if(s_input.commands) vQueueDelete(s_input.commands);
    if(s_input.results) vQueueDelete(s_input.results);
    if(s_input.lifecycle_wake) vQueueDelete(s_input.lifecycle_wake);
    if(s_input.configurations) vQueueDelete(s_input.configurations);
    if(s_input.scanner_done) vSemaphoreDelete(s_input.scanner_done);
    if(s_input.owner_done) vSemaphoreDelete(s_input.owner_done);
    if(s_input.audio_done) vSemaphoreDelete(s_input.audio_done);
    if(s_input.control_mutex) vSemaphoreDelete(s_input.control_mutex);
    memset(&s_input,0,sizeof(s_input));
}

esp_err_t vk_input_init(void)
{
    if(s_input.transitions!=NULL)return ESP_ERR_INVALID_STATE;
    memset(&s_input,0,sizeof(s_input));atomic_init(&s_input.stopping,false);atomic_init(&s_input.admission_open,false);atomic_init(&s_input.local_failure,false);atomic_init(&s_input.result_overflow,false);atomic_init(&s_input.tainted,false);atomic_init(&s_input.scanner_generation,1U);atomic_init(&s_input.active_epoch,0U);atomic_init(&s_input.barrier_generation,0U);atomic_init(&s_input.scanner_quiescent,0U);atomic_init(&s_input.owner_quiescent,0U);atomic_init(&s_input.protocol_callbacks_in_flight,0U);
    s_input.mode=VK_USB_INPUT_MODE_HOLD_TO_TALK;s_input.voice_key=VK_USB_KEY_K4;s_audio=vk_audio_control_api();
    if(s_audio==NULL||s_audio->prepare==NULL||s_audio->release==NULL||s_audio->cancel_prepared==NULL||s_audio->stop==NULL||s_audio->abort==NULL)return ESP_ERR_INVALID_STATE;
    s_input.transitions=xQueueCreate(VK_INPUT_FIFO_CAPACITY,sizeof(vk_input_transition_t));s_input.commands=xQueueCreate(VK_INPUT_AUDIO_MAILBOX_CAPACITY,sizeof(audio_command_t));s_input.results=xQueueCreate(VK_INPUT_AUDIO_MAILBOX_CAPACITY,sizeof(audio_result_t));s_input.lifecycle_wake=xQueueCreate(1U,sizeof(uint32_t));s_input.configurations=xQueueCreate(4U,sizeof(vk_usb_input_command_t));
    s_input.control_mutex=xSemaphoreCreateMutex();s_input.scanner_done=xSemaphoreCreateBinary();s_input.owner_done=xSemaphoreCreateBinary();s_input.audio_done=xSemaphoreCreateBinary();
    if(!s_input.transitions||!s_input.commands||!s_input.results||!s_input.lifecycle_wake||!s_input.configurations||!s_input.control_mutex||!s_input.scanner_done||!s_input.owner_done||!s_input.audio_done){delete_resources();return ESP_ERR_NO_MEM;}
    vk_usb_input_handler_registration_t input={.handle_command=handle_input_command,.context=&s_input};
    vk_usb_input_lifecycle_registration_t lifecycle={.begin=begin_lifecycle,.context=&s_input};
    esp_err_t error=vk_usb_register_input_handler(&input);if(error==ESP_OK)error=vk_usb_register_input_lifecycle(&lifecycle);return error;
}

static bool join_task(bool *started,TaskHandle_t *handle,SemaphoreHandle_t done)
{
    if(!*started)return true;
#ifdef VK_INPUT_NATIVE_TEST
    if(atomic_load_explicit(&s_test_force_join_timeout,memory_order_acquire))return false;
#endif
    if(xSemaphoreTake(done,pdMS_TO_TICKS(VK_INPUT_JOIN_TIMEOUT_MS))!=pdTRUE)return false;
    control_lock();*started=false;*handle=NULL;control_unlock();return true;
}

esp_err_t vk_input_start(void)
{
    if(!s_input.transitions)return ESP_ERR_INVALID_STATE;
    control_lock();bool busy=s_input.scanner_started||s_input.owner_started||s_input.audio_started;control_unlock();if(busy)return ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&s_input.stopping,false,memory_order_release);
    if(xTaskCreatePinnedToCore(scanner_task,"vk_scan",VK_INPUT_SCANNER_STACK,&s_input,VK_INPUT_TASK_PRIORITY,&s_input.scanner_task,1)!=pdPASS)goto failed;
    control_lock();s_input.scanner_started=true;control_unlock();
    if(xTaskCreatePinnedToCore(owner_task,"vk_input",VK_INPUT_OWNER_STACK,&s_input,VK_INPUT_TASK_PRIORITY,&s_input.owner_task,1)!=pdPASS)goto failed;
    control_lock();s_input.owner_started=true;control_unlock();
    if(xTaskCreatePinnedToCore(audio_task,"vk_audio_ctl",VK_INPUT_AUDIO_STACK,&s_input,VK_INPUT_TASK_PRIORITY,&s_input.audio_task,1)!=pdPASS)goto failed;
    control_lock();s_input.audio_started=true;control_unlock();
    return ESP_OK;
failed:
    atomic_store_explicit(&s_input.stopping,true,memory_order_release);close_local_admission();
    bool scanner=join_task(&s_input.scanner_started,&s_input.scanner_task,s_input.scanner_done);
    bool owner=join_task(&s_input.owner_started,&s_input.owner_task,s_input.owner_done);
    bool audio=join_task(&s_input.audio_started,&s_input.audio_task,s_input.audio_done);
    if(!scanner||!owner||!audio)atomic_store_explicit(&s_input.tainted,true,memory_order_release);
    return ESP_ERR_NO_MEM;
}

esp_err_t vk_input_stop(void)
{
    if(!s_input.transitions)return ESP_OK;
    close_local_admission();
    /* USB must complete the registered stopping lifecycle while these tasks live. */
    if(active_epoch()!=0U){atomic_store_explicit(&s_input.tainted,true,memory_order_release);return ESP_ERR_INVALID_STATE;}
    atomic_store_explicit(&s_input.stopping,true,memory_order_release);
    bool scanner=join_task(&s_input.scanner_started,&s_input.scanner_task,s_input.scanner_done);
    bool owner=join_task(&s_input.owner_started,&s_input.owner_task,s_input.owner_done);
    bool audio=join_task(&s_input.audio_started,&s_input.audio_task,s_input.audio_done);
    if(!scanner||!owner||!audio){atomic_store_explicit(&s_input.tainted,true,memory_order_release);return ESP_ERR_TIMEOUT;}
    return ESP_OK;
}
esp_err_t vk_input_deinit(void){control_lock();bool active=s_input.scanner_started||s_input.owner_started||s_input.audio_started;control_unlock();if(active)return ESP_ERR_INVALID_STATE;delete_resources();return ESP_OK;}
bool vk_input_is_tainted(void){return atomic_load_explicit(&s_input.tainted,memory_order_acquire);}

#ifdef VK_INPUT_NATIVE_TEST
void vk_input_test_set_audio_api(const vk_audio_control_api_t *api){s_audio=api;}
void vk_input_test_set_lifecycle_hook(vk_input_test_lifecycle_hook_t hook,void *context){s_test_lifecycle_hook=hook;s_test_lifecycle_hook_context=context;}
void vk_input_test_set_command_hook(vk_input_test_command_hook_t hook,void *context){s_test_command_hook=hook;s_test_command_hook_context=context;}
vk_usb_input_lifecycle_begin_result_t vk_input_test_begin_lifecycle(const vk_usb_input_lifecycle_request_t *request,const vk_usb_input_lifecycle_sink_t *sink){return begin_lifecycle(&s_input,request,sink);}
bool vk_input_test_started(void){control_lock();bool started=s_input.scanner_started||s_input.owner_started||s_input.audio_started;control_unlock();return started;}
bool vk_input_test_enqueue_transition(vk_input_transition_t transition){return xQueueSend(s_input.transitions,&transition,0U)==pdTRUE;}
bool vk_input_test_fill_ordinary_mailboxes(void){audio_command_t command={.kind=AUDIO_PREPARE,.epoch=UINT32_MAX,.generation=UINT32_MAX,.session=0U};audio_result_t result={.kind=AUDIO_FAILED,.command=AUDIO_PREPARE,.epoch=UINT32_MAX,.generation=UINT32_MAX,.session=0U};bool ok=true;for(size_t index=0;index<VK_INPUT_AUDIO_MAILBOX_CAPACITY;++index){ok=ok&&xQueueSend(s_input.commands,&command,0U)==pdTRUE;ok=ok&&xQueueSend(s_input.results,&result,0U)==pdTRUE;}return ok;}
bool vk_input_test_fill_transition_fifo_and_overflow(void){vk_input_transition_t value={.kind=VK_INPUT_TRANSITION_DOWN,.key=VK_INPUT_KEY_K1};bool ok=true;for(size_t index=0;index<VK_INPUT_FIFO_CAPACITY;++index)ok=ok&&xQueueSend(s_input.transitions,&value,0U)==pdTRUE;if(xQueueSend(s_input.transitions,&value,0U)==pdTRUE)return false;atomic_store_explicit(&s_input.local_failure,true,memory_order_release);return ok;}
void vk_input_test_force_result_overflow(void){atomic_store_explicit(&s_input.result_overflow,true,memory_order_release);}
void vk_input_test_force_join_timeout(bool enabled){atomic_store_explicit(&s_test_force_join_timeout,enabled,memory_order_release);}
bool vk_input_test_inject_result(vk_input_test_result_kind_t kind,uint32_t epoch,uint32_t generation,uint32_t session){if(kind>VK_INPUT_TEST_RESULT_TAINTED)return false;audio_result_t result={.kind=(audio_result_kind_t)kind,.command=kind==VK_INPUT_TEST_RESULT_RUNTIME_FAILED?AUDIO_RELEASE:AUDIO_PREPARE,.epoch=epoch,.generation=generation,.session=session};return xQueueSend(s_input.results,&result,0U)==pdTRUE;}
uint32_t vk_input_test_active_epoch(void){return active_epoch();}
uint32_t vk_input_test_generation(void){return s_input.generation;}
uint32_t vk_input_test_session(void){return s_input.session;}
#endif
