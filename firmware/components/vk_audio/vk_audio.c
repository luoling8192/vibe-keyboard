#include "vk_audio.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef VK_AUDIO_NATIVE_TEST
#include "vk_audio_native_platform.h"
#else
#include "driver/i2s_common.h"
#include "driver/i2s_pdm.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opus.h"
#endif
#include "vk_audio_pipeline.h"
#include "vk_audio_backend.h"
#include "vk_audio_runtime.h"
#ifdef VK_AUDIO_NATIVE_TEST
#define VK_MIC_PDM_CLK 41
#define VK_MIC_PDM_DATA 40
#else
#include "vk_board.h"
#endif
#include "vk_usb.h"

#define VK_AUDIO_MIC_RING_BYTES 64000U
#define VK_AUDIO_WORKER_STACK_BYTES 32768U
#define VK_AUDIO_WORKER_PRIORITY 5U
#define VK_AUDIO_WORKER_CORE 0
#define VK_AUDIO_I2S_DMA_DESCRIPTORS 4U
#define VK_AUDIO_I2S_DMA_FRAMES 512U
#define VK_AUDIO_I2S_READ_BYTES 4096U
#define VK_AUDIO_I2S_READ_TIMEOUT_MS 20U

typedef struct {
    vk_audio_backend_t backend;
    uint32_t usb_epoch;
    uint32_t session_id;
    afe_config_t *afe_config;
    esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    OpusEncoder *encoder;
    vk_audio_pipeline_t pipeline;
    TaskHandle_t worker;
    atomic_bool stop_requested;
    atomic_bool capture_released;
    atomic_bool finalize_on_stop;
    atomic_bool stop_notification_complete;
    SemaphoreHandle_t started;
    SemaphoreHandle_t stopped;
    esp_err_t startup_error;
    atomic_int runtime_error;
    esp_err_t cleanup_error;
} production_session_t;

typedef struct {
    SemaphoreHandle_t mutex;
    bool initialized;
    bool api_initialized;
    uint32_t last_session_id;
    bool runtime_failure_pending;
    uint32_t runtime_failure_session_id;
    vk_audio_runtime_t runtime;
    production_session_t *session;
} production_audio_t;

static production_audio_t s_audio;

static void *backend_allocate(void *context, size_t bytes, bool external)
{
    (void)context;
    void *pointer = heap_caps_malloc(bytes, external ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#ifdef VK_AUDIO_NATIVE_TEST
    if (pointer != NULL) vk_audio_native_resource_allocated(
        bytes == VK_AUDIO_MIC_RING_BYTES ? VK_AUDIO_NATIVE_RESOURCE_RING :
                                           VK_AUDIO_NATIVE_RESOURCE_FEED);
#endif
    return pointer;
}
static void backend_deallocate(void *context, void *pointer)
{
#ifdef VK_AUDIO_NATIVE_TEST
    production_session_t *session = context;
    vk_audio_native_resource_freed(pointer == session->backend.ring ?
        VK_AUDIO_NATIVE_RESOURCE_RING : VK_AUDIO_NATIVE_RESOURCE_FEED);
#else
    (void)context;
#endif
    free(pointer);
}
static esp_err_t backend_i2s_create(void *context, void **handle)
{
    (void)context; i2s_chan_config_t cfg=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    cfg.dma_desc_num=VK_AUDIO_I2S_DMA_DESCRIPTORS; cfg.dma_frame_num=VK_AUDIO_I2S_DMA_FRAMES;
    i2s_chan_handle_t rx=NULL; esp_err_t e=i2s_new_channel(&cfg,NULL,&rx); if(e==ESP_OK)*handle=rx; return e;
}
static esp_err_t backend_i2s_initialize(void *context, void *handle)
{
    (void)context; i2s_pdm_rx_config_t cfg={.clk_cfg=I2S_PDM_RX_CLK_DEFAULT_CONFIG(VK_AUDIO_SAMPLE_RATE_HZ),.slot_cfg=I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),.gpio_cfg={.clk=VK_MIC_PDM_CLK,.din=VK_MIC_PDM_DATA,.invert_flags={.clk_inv=0}}};
    return i2s_channel_init_pdm_rx_mode((i2s_chan_handle_t)handle,&cfg);
}
static esp_err_t backend_i2s_enable(void*c,void*h){(void)c;return i2s_channel_enable((i2s_chan_handle_t)h);}
static esp_err_t backend_i2s_read(void*c,void*h,uint8_t*b,size_t z,size_t*n){(void)c;return i2s_channel_read((i2s_chan_handle_t)h,b,z,n,VK_AUDIO_I2S_READ_TIMEOUT_MS);}
static esp_err_t backend_i2s_disable(void*c,void*h){(void)c;return i2s_channel_disable((i2s_chan_handle_t)h);}
static esp_err_t backend_i2s_destroy(void*c,void*h){(void)c;return i2s_del_channel((i2s_chan_handle_t)h);}
static void backend_afe_destroy(void *context, void *handle);

static esp_err_t backend_afe_create(void *context, void **handle,
                                    size_t *feed_bytes, size_t *fetch_bytes)
{
    production_session_t *session = context;
    *handle = NULL;
    session->afe_config = afe_config_init("MM", NULL, AFE_TYPE_SR,
                                          AFE_MODE_HIGH_PERF);
    if (session->afe_config == NULL) return ESP_ERR_NO_MEM;
#ifdef VK_AUDIO_NATIVE_TEST
    vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_AFE_CONFIG);
#endif
    session->afe_config->aec_init = false;
    session->afe_config->se_init = true;
    session->afe_config->vad_init = false;
    session->afe_config->wakenet_init = false;
    session->afe_config->agc_init = false;
    session->afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    session->afe_config->afe_perferred_core = 0;
    session->afe_config->afe_perferred_priority = 20;
    session->afe_config->afe_ringbuf_size = 64;
    session->afe_config->afe_linear_gain = 1.0f;
    session->afe_config->debug_init = false;
    session->afe = esp_afe_handle_from_config(session->afe_config);
    if (session->afe == NULL || session->afe->create_from_config == NULL ||
        session->afe->destroy == NULL || session->afe->feed == NULL ||
        session->afe->fetch_with_delay == NULL ||
        session->afe->get_feed_chunksize == NULL ||
        session->afe->get_fetch_chunksize == NULL ||
        session->afe->get_feed_channel_num == NULL ||
        session->afe->get_fetch_channel_num == NULL ||
        session->afe->get_samp_rate == NULL) {
        backend_afe_destroy(context, NULL);
        return ESP_ERR_NOT_SUPPORTED;
    }
    session->afe_data = session->afe->create_from_config(session->afe_config);
    if (session->afe_data == NULL) {
        backend_afe_destroy(context, NULL);
        return ESP_ERR_NO_MEM;
    }
#ifdef VK_AUDIO_NATIVE_TEST
    vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_AFE_DATA);
#endif
    int feed_samples = session->afe->get_feed_chunksize(session->afe_data);
    int fetch_samples = session->afe->get_fetch_chunksize(session->afe_data);
    if (feed_samples <= 0 || fetch_samples <= 0 ||
        session->afe->get_feed_channel_num(session->afe_data) != 2 ||
        session->afe->get_fetch_channel_num(session->afe_data) != 1 ||
        session->afe->get_samp_rate(session->afe_data) != VK_AUDIO_SAMPLE_RATE_HZ ||
        (size_t)feed_samples > VK_AUDIO_MIC_RING_BYTES / (2U * sizeof(int16_t)) ||
        (size_t)fetch_samples > VK_AUDIO_MIC_RING_BYTES / sizeof(int16_t)) {
        backend_afe_destroy(context, NULL);
        return ESP_ERR_INVALID_SIZE;
    }
    *feed_bytes = (size_t)feed_samples * 2U * sizeof(int16_t);
    *fetch_bytes = (size_t)fetch_samples * sizeof(int16_t);
    *handle = session;
    return ESP_OK;
}
static int backend_afe_feed(void*c,void*h,const int16_t*p){(void)h;production_session_t*s=c;return s->afe->feed(s->afe_data,p);}
static bool backend_afe_fetch(void*c,void*h,vk_audio_backend_fetch_t*r){(void)h;production_session_t*s=c;afe_fetch_result_t*x=s->afe->fetch_with_delay(s->afe_data,0U);if(!x)return false;r->data=x->data;r->data_bytes=x->data_size;r->result=x->ret_value;return true;}
static void backend_afe_destroy(void*c,void*h){(void)h;production_session_t*s=c;if(s->afe_data&&s->afe){s->afe->destroy(s->afe_data);
#ifdef VK_AUDIO_NATIVE_TEST
vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_AFE_DATA);
#endif
}s->afe_data=NULL;s->afe=NULL;if(s->afe_config){afe_config_free(s->afe_config);
#ifdef VK_AUDIO_NATIVE_TEST
vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_AFE_CONFIG);
#endif
}s->afe_config=NULL;}
static esp_err_t backend_opus_create(void *context, void **handle)
{
    production_session_t *session = context;
    int opus_error = OPUS_OK;
    *handle = NULL;
    session->encoder = opus_encoder_create(VK_AUDIO_SAMPLE_RATE_HZ, 1,
                                            OPUS_APPLICATION_VOIP, &opus_error);
    if (session->encoder != NULL) {
#ifdef VK_AUDIO_NATIVE_TEST
        vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_OPUS);
#endif
    }
    if (session->encoder == NULL || opus_error != OPUS_OK) {
        if (session->encoder != NULL) { opus_encoder_destroy(session->encoder);
#ifdef VK_AUDIO_NATIVE_TEST
            vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_OPUS);
#endif
        }
        session->encoder = NULL;
        return ESP_FAIL;
    }
    if (opus_encoder_ctl(session->encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(session->encoder, OPUS_SET_BITRATE(16000)) != OPUS_OK ||
        opus_encoder_ctl(session->encoder, OPUS_SET_DTX(0)) != OPUS_OK ||
        opus_encoder_ctl(session->encoder, OPUS_SET_COMPLEXITY(4)) != OPUS_OK ||
        opus_encoder_ctl(session->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK) {
        opus_encoder_destroy(session->encoder);
#ifdef VK_AUDIO_NATIVE_TEST
        vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_OPUS);
#endif
        session->encoder = NULL;
        return ESP_FAIL;
    }
    *handle = session->encoder;
    return ESP_OK;
}
static int backend_opus_encode(void*c,void*h,const int16_t*p,size_t n,uint8_t*out,size_t z){(void)c;if(n!=VK_AUDIO_OPUS_FRAME_SAMPLES||z>INT_MAX)return OPUS_BAD_ARG;return opus_encode(h,p,(int)n,out,(opus_int32)z);}
static void backend_opus_destroy(void*c,void*h){production_session_t*s=c;opus_encoder_destroy(h);
#ifdef VK_AUDIO_NATIVE_TEST
vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_OPUS);
#endif
s->encoder=NULL;}
static esp_err_t backend_send(void*c,uint32_t sid,uint32_t seq,uint8_t flags,const uint8_t*p,uint16_t n){production_session_t*s=c;vk_usb_audio_frame_t f={.session_id=sid,.sequence=seq,.flags=flags,.payload=p,.payload_length=n};return vk_usb_send_audio(s->usb_epoch,&f);}
static vk_audio_backend_ops_t backend_ops(production_session_t*s){return(vk_audio_backend_ops_t){backend_i2s_create,backend_i2s_initialize,backend_i2s_enable,backend_i2s_read,backend_i2s_disable,backend_i2s_destroy,backend_afe_create,backend_afe_feed,backend_afe_fetch,backend_afe_destroy,backend_opus_create,backend_opus_encode,backend_opus_destroy,backend_allocate,backend_deallocate,backend_send,s};}
static void production_worker(void *argument)
{
    production_session_t*s=argument;vk_audio_backend_ops_t ops=backend_ops(s);s->startup_error=vk_audio_backend_acquire(&s->backend,&ops,s->session_id);xSemaphoreGive(s->started);
    if (s->startup_error == ESP_OK) {
        while (!atomic_load_explicit(&s->capture_released, memory_order_acquire) &&
               !atomic_load_explicit(&s->stop_requested, memory_order_acquire)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        while (atomic_load_explicit(&s->capture_released, memory_order_acquire) &&
               !atomic_load_explicit(&s->stop_requested, memory_order_acquire) &&
               ulTaskNotifyTake(pdTRUE, 0U) == 0U) {
            esp_err_t capture_error = vk_audio_backend_capture(&s->backend);
            atomic_store_explicit(&s->runtime_error, capture_error,
                                  memory_order_release);
            if (capture_error != ESP_OK) break;
        }
        if (atomic_load_explicit(&s->capture_released, memory_order_acquire) &&
            atomic_load_explicit(&s->finalize_on_stop, memory_order_acquire) &&
            atomic_load_explicit(&s->runtime_error, memory_order_acquire) == ESP_OK) {
            atomic_store_explicit(&s->runtime_error,
                                  vk_audio_backend_finish(&s->backend),
                                  memory_order_release);
        }
    }
    if (!atomic_load_explicit(&s->stop_requested, memory_order_acquire)) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    while (!atomic_load_explicit(&s->stop_notification_complete,
                                 memory_order_acquire)) {}
    s->cleanup_error = vk_audio_backend_release(&s->backend);
    xSemaphoreGive(s->stopped);
    vTaskDelete(NULL);
}

static void delete_started(production_session_t *session) { if (session->started != NULL) { vSemaphoreDelete(session->started); session->started=NULL;
#ifdef VK_AUDIO_NATIVE_TEST
vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_STARTED_SEMAPHORE);
#endif
} }
static void delete_stopped(production_session_t *session) { if (session->stopped != NULL) { vSemaphoreDelete(session->stopped); session->stopped=NULL;
#ifdef VK_AUDIO_NATIVE_TEST
vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_STOPPED_SEMAPHORE);
#endif
} }
static void free_session(production_session_t *session) { if (session != NULL) { free(session);
#ifdef VK_AUDIO_NATIVE_TEST
vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_SESSION);
#endif
} }

static esp_err_t runtime_prepare(void *context)
{
    production_audio_t *audio = context;
    production_session_t *session = heap_caps_calloc(
        1U, sizeof(*session), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (session == NULL) return ESP_ERR_NO_MEM;
#ifdef VK_AUDIO_NATIVE_TEST
    vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_SESSION);
#endif
    atomic_init(&session->stop_requested, false);
    atomic_init(&session->capture_released, false);
    atomic_init(&session->finalize_on_stop, false);
    atomic_init(&session->stop_notification_complete, false);
    atomic_init(&session->runtime_error, ESP_OK);
    session->started = xSemaphoreCreateBinary();
#ifdef VK_AUDIO_NATIVE_TEST
    if (session->started != NULL) vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_STARTED_SEMAPHORE);
#endif
    session->stopped = xSemaphoreCreateBinary();
#ifdef VK_AUDIO_NATIVE_TEST
    if (session->stopped != NULL) vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_STOPPED_SEMAPHORE);
#endif
    if (session->started == NULL || session->stopped == NULL) {
        delete_started(session);
        delete_stopped(session);
        free_session(session);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = vk_usb_current_epoch(&session->usb_epoch);
    if (error != ESP_OK) {
        delete_started(session);
        delete_stopped(session);
        free_session(session);
        return error;
    }
    audio->last_session_id = vk_audio_next_session(audio->last_session_id);
    session->session_id = audio->last_session_id;
    audio->session = session;
    return ESP_OK;
}

static esp_err_t runtime_create_worker(void *context)
{
    production_audio_t *audio = context;
    BaseType_t created = xTaskCreatePinnedToCore(
        production_worker, "vk_audio", VK_AUDIO_WORKER_STACK_BYTES,
        audio->session, VK_AUDIO_WORKER_PRIORITY, &audio->session->worker,
        VK_AUDIO_WORKER_CORE);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool runtime_wait_started(void *context, uint32_t timeout_ms)
{
    production_audio_t *audio = context;
    return xSemaphoreTake(audio->session->started, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static esp_err_t runtime_startup_error(void *context) { return ((production_audio_t *)context)->session->startup_error; }
static void runtime_request_stop(void *context)
{
    production_session_t *session = ((production_audio_t *)context)->session;
    if (session != NULL) {
        atomic_store_explicit(&session->stop_requested, true, memory_order_release);
        if (session->worker != NULL) xTaskNotifyGive(session->worker);
        atomic_store_explicit(&session->stop_notification_complete, true,
                              memory_order_release);
#ifdef VK_AUDIO_NATIVE_TEST
        vk_audio_native_stop_requested();
#endif
    }
}
static bool runtime_wait_stopped(void *context, uint32_t timeout_ms)
{
    production_audio_t *audio = context;
    return xSemaphoreTake(audio->session->stopped, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static esp_err_t runtime_runtime_error(void *context)
{
    return atomic_load_explicit(
        &((production_audio_t *)context)->session->runtime_error,
        memory_order_acquire);
}
static esp_err_t runtime_cleanup_error(void *context) { return ((production_audio_t *)context)->session->cleanup_error; }
static esp_err_t runtime_retry_cleanup(void *context)
{
    production_session_t *session = ((production_audio_t *)context)->session;
    if (session == NULL) return ESP_ERR_INVALID_STATE;
    session->cleanup_error = vk_audio_backend_release(&session->backend);
    return session->cleanup_error;
}
static void runtime_release(void *context)
{
    production_audio_t *audio = context;
    production_session_t *session = audio->session;
    if (session == NULL) return;
    delete_started(session);
    delete_stopped(session);
    free_session(session);
    audio->session = NULL;
}

static void initialize_runtime(void)
{
    if (s_audio.initialized) return;
    vk_audio_runtime_ops_t ops = {
        .prepare = runtime_prepare,
        .create_worker = runtime_create_worker,
        .wait_started = runtime_wait_started,
        .startup_error = runtime_startup_error,
        .request_stop = runtime_request_stop,
        .wait_stopped = runtime_wait_stopped,
        .runtime_error = runtime_runtime_error,
        .cleanup_error = runtime_cleanup_error,
        .retry_cleanup = runtime_retry_cleanup,
        .release = runtime_release,
        .context = &s_audio,
    };
    vk_audio_runtime_init(&s_audio.runtime, &ops);
    s_audio.initialized = true;
}

esp_err_t vk_audio_init(void)
{
    /* Startup/shutdown composition is the sole creator; the mutex is retained so
     * concurrent getters can never race semaphore destruction. */
    if (s_audio.mutex == NULL) {
        s_audio.mutex = xSemaphoreCreateMutex();
        if (s_audio.mutex == NULL) return ESP_ERR_NO_MEM;
        initialize_runtime();
    }
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    if (vk_audio_runtime_is_tainted(&s_audio.runtime)) {
        xSemaphoreGive(s_audio.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_audio.api_initialized = true;
    xSemaphoreGive(s_audio.mutex);
    return ESP_OK;
}

esp_err_t vk_audio_deinit(void)
{
    if (s_audio.mutex == NULL) return ESP_OK;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    esp_err_t error;
    if (vk_audio_runtime_is_tainted(&s_audio.runtime)) {
        error = vk_audio_runtime_collect(&s_audio.runtime,
                                         VK_AUDIO_JOIN_TIMEOUT_MS);
    } else {
        error = vk_audio_runtime_stop(&s_audio.runtime);
    }
    if (error == ESP_OK) s_audio.api_initialized = false;
    xSemaphoreGive(s_audio.mutex);
    return error;
}

esp_err_t vk_audio_prepare(uint32_t *session_id)
{
    if (session_id == NULL) return ESP_ERR_INVALID_ARG;
    if (s_audio.mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    if (!s_audio.api_initialized) {
        xSemaphoreGive(s_audio.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = vk_audio_runtime_start(&s_audio.runtime);
    if (error == ESP_OK) *session_id = s_audio.last_session_id;
    xSemaphoreGive(s_audio.mutex);
    return error;
}

esp_err_t vk_audio_release(uint32_t session_id)
{
    if (session_id == 0U || s_audio.mutex == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    production_session_t *session = s_audio.session;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_audio.api_initialized && session != NULL &&
        session->session_id == session_id &&
        vk_audio_runtime_is_active(&s_audio.runtime) &&
        !atomic_load_explicit(&session->capture_released, memory_order_acquire)) {
        atomic_store_explicit(&session->capture_released, true, memory_order_release);
        if (session->worker != NULL) xTaskNotifyGive(session->worker);
        error = ESP_OK;
    }
    xSemaphoreGive(s_audio.mutex);
    return error;
}

esp_err_t vk_audio_cancel_prepared(uint32_t session_id)
{
    if (session_id == 0U || s_audio.mutex == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    production_session_t *session = s_audio.session;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_audio.api_initialized && session != NULL &&
        session->session_id == session_id &&
        vk_audio_runtime_is_active(&s_audio.runtime) &&
        !atomic_load_explicit(&session->capture_released, memory_order_acquire)) {
        error = vk_audio_runtime_stop(&s_audio.runtime);
    }
    xSemaphoreGive(s_audio.mutex);
    return error;
}

esp_err_t vk_audio_abort(void)
{
    if (s_audio.mutex == NULL) return ESP_OK;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    if (!s_audio.api_initialized) {
        xSemaphoreGive(s_audio.mutex);
        return ESP_OK;
    }
    production_session_t *session = s_audio.session;
    if (session != NULL) {
        atomic_store_explicit(&session->finalize_on_stop, false,
                              memory_order_release);
    }
    esp_err_t error = vk_audio_runtime_stop(&s_audio.runtime);
    xSemaphoreGive(s_audio.mutex);
    return error;
}

esp_err_t vk_audio_start(uint32_t *session_id)
{
    esp_err_t error = vk_audio_prepare(session_id);
    if (error != ESP_OK) return error;
    error = vk_audio_release(*session_id);
    if (error != ESP_OK) (void)vk_audio_cancel_prepared(*session_id);
    return error;
}

esp_err_t vk_audio_stop(void)
{
    if (s_audio.mutex == NULL) return ESP_OK;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    if (!s_audio.api_initialized) {
        xSemaphoreGive(s_audio.mutex);
        return ESP_OK;
    }
    production_session_t *session = s_audio.session;
    if (session != NULL) {
        atomic_store_explicit(&session->finalize_on_stop, true,
                              memory_order_release);
    }
    esp_err_t error = vk_audio_runtime_stop(&s_audio.runtime);
    xSemaphoreGive(s_audio.mutex);
    return error;
}

bool vk_audio_is_active(void)
{
    if (s_audio.mutex == NULL) return false;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    bool result = s_audio.api_initialized && vk_audio_runtime_is_active(&s_audio.runtime);
    xSemaphoreGive(s_audio.mutex);
    return result;
}

uint32_t vk_audio_session_id(void)
{
    if (s_audio.mutex == NULL) return 0U;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    uint32_t result = s_audio.api_initialized ? s_audio.last_session_id : 0U;
    xSemaphoreGive(s_audio.mutex);
    return result;
}

static bool vk_audio_take_runtime_failure(uint32_t *session_id)
{
    if (session_id == NULL || s_audio.mutex == NULL) return false;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    production_session_t *session = s_audio.session;
    if (!s_audio.runtime_failure_pending && session != NULL &&
        vk_audio_runtime_is_active(&s_audio.runtime)) {
        esp_err_t runtime_error = atomic_load_explicit(
            &session->runtime_error, memory_order_acquire);
        if (runtime_error != ESP_OK) {
            uint32_t failed_session_id = session->session_id;
            atomic_store_explicit(&session->finalize_on_stop, false,
                                  memory_order_release);
            esp_err_t stop_error = vk_audio_runtime_stop(&s_audio.runtime);
            if (stop_error == runtime_error &&
                !vk_audio_runtime_is_tainted(&s_audio.runtime)) {
                s_audio.runtime_failure_pending = true;
                s_audio.runtime_failure_session_id = failed_session_id;
            }
        }
    }
    bool result = s_audio.runtime_failure_pending;
    if (result) {
        *session_id = s_audio.runtime_failure_session_id;
        s_audio.runtime_failure_pending = false;
        s_audio.runtime_failure_session_id = 0U;
    }
    xSemaphoreGive(s_audio.mutex);
    return result;
}

bool vk_audio_is_tainted(void)
{
    if (s_audio.mutex == NULL) return false;
    xSemaphoreTake(s_audio.mutex, portMAX_DELAY);
    bool result = vk_audio_runtime_is_tainted(&s_audio.runtime);
    xSemaphoreGive(s_audio.mutex);
    return result;
}

const vk_audio_control_api_t *vk_audio_control_api(void)
{
    static const vk_audio_control_api_t api = {
        .prepare = vk_audio_prepare,
        .release = vk_audio_release,
        .cancel_prepared = vk_audio_cancel_prepared,
        .stop = vk_audio_stop,
        .abort = vk_audio_abort,
        .is_active = vk_audio_is_active,
        .session_id = vk_audio_session_id,
        .take_runtime_failure = vk_audio_take_runtime_failure,
    };
    return &api;
}
