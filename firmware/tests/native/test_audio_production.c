#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vk_audio.h"
#include "vk_audio_native_platform.h"
#include "vk_usb.h"

typedef struct { pthread_mutex_t mutex; pthread_cond_t condition; unsigned count; } sem_t;
typedef struct { pthread_t thread; pthread_mutex_t mutex; pthread_cond_t condition; unsigned notifications; void (*entry)(void *); void *argument; } task_t;
static _Thread_local task_t *current_task;
static atomic_int allocation_fail_after, allocation_count, task_live_count;
static atomic_int resource_alloc[VK_AUDIO_NATIVE_RESOURCE_COUNT];
static atomic_int resource_free[VK_AUDIO_NATIVE_RESOURCE_COUNT];
static pthread_mutex_t stop_gate_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_gate_condition=PTHREAD_COND_INITIALIZER;
static bool stop_gate_enabled, stop_gate_entered, stop_gate_release;
static pthread_mutex_t started_gate_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t started_gate_condition=PTHREAD_COND_INITIALIZER;
static bool started_gate_enabled, started_gate_entered, started_gate_release;
static atomic_int afe_mode, afe_destroy_count, afe_config_free_count;
static atomic_int opus_ctl_fail_at, opus_ctl_count, opus_destroy_count;
static atomic_int usb_send_error, usb_send_count, usb_final_count, i2s_read_error;
static atomic_int i2s_disable_failures, i2s_destroy_failures;
static atomic_int i2s_create_count, i2s_initialize_count, i2s_enable_count;
static atomic_int i2s_disable_count, i2s_destroy_count, semaphore_live_count;
static atomic_bool i2s_block, i2s_read_entered, afe_create_block, afe_create_entered;
static vk_usb_uac_source_registration_t registered_uac_source;
static vk_usb_audio_diagnostics_provider_registration_t registered_diagnostics;

void vk_audio_native_resource_allocated(vk_audio_native_resource_t r){assert(r<VK_AUDIO_NATIVE_RESOURCE_COUNT);atomic_fetch_add(&resource_alloc[r],1);}
void vk_audio_native_resource_freed(vk_audio_native_resource_t r){assert(r<VK_AUDIO_NATIVE_RESOURCE_COUNT);atomic_fetch_add(&resource_free[r],1);}
void vk_audio_native_started(void){pthread_mutex_lock(&started_gate_mutex);if(started_gate_enabled){started_gate_entered=true;pthread_cond_broadcast(&started_gate_condition);while(!started_gate_release)pthread_cond_wait(&started_gate_condition,&started_gate_mutex);}pthread_mutex_unlock(&started_gate_mutex);}
void vk_audio_native_stop_requested(void){pthread_mutex_lock(&stop_gate_mutex);if(stop_gate_enabled){stop_gate_entered=true;pthread_cond_broadcast(&stop_gate_condition);while(!stop_gate_release)pthread_cond_wait(&stop_gate_condition,&stop_gate_mutex);}pthread_mutex_unlock(&stop_gate_mutex);}
static bool fail_allocation(void){int n=atomic_fetch_add(&allocation_count,1)+1;int f=atomic_load(&allocation_fail_after);return f>0&&n==f;}
void *heap_caps_malloc(size_t n,uint32_t caps){(void)caps;return fail_allocation()?NULL:malloc(n);}
void *heap_caps_calloc(size_t a,size_t b,uint32_t caps){(void)caps;return fail_allocation()?NULL:calloc(a,b);}
SemaphoreHandle_t xSemaphoreCreateBinary(void){if(fail_allocation())return NULL;sem_t*s=calloc(1,sizeof(*s));if(!s)return NULL;pthread_mutex_init(&s->mutex,NULL);pthread_cond_init(&s->condition,NULL);atomic_fetch_add(&semaphore_live_count,1);return s;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){sem_t*s=xSemaphoreCreateBinary();if(s)s->count=1;return s;}
static void deadline_after(TickType_t milliseconds,struct timespec*deadline){clock_gettime(CLOCK_REALTIME,deadline);deadline->tv_sec+=(time_t)(milliseconds/1000U);deadline->tv_nsec+=(long)(milliseconds%1000U)*1000000L;if(deadline->tv_nsec>=1000000000L){++deadline->tv_sec;deadline->tv_nsec-=1000000000L;}}
BaseType_t xSemaphoreTake(SemaphoreHandle_t h,TickType_t timeout){sem_t*s=h;pthread_mutex_lock(&s->mutex);if(timeout==0&&s->count==0){pthread_mutex_unlock(&s->mutex);return 0;}int error=0;if(timeout==portMAX_DELAY){while(!s->count)pthread_cond_wait(&s->condition,&s->mutex);}else{struct timespec deadline;deadline_after(timeout,&deadline);while(!s->count&&error!=ETIMEDOUT)error=pthread_cond_timedwait(&s->condition,&s->mutex,&deadline);if(!s->count){pthread_mutex_unlock(&s->mutex);return 0;}}--s->count;pthread_mutex_unlock(&s->mutex);return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h){sem_t*s=h;pthread_mutex_lock(&s->mutex);++s->count;pthread_cond_broadcast(&s->condition);pthread_mutex_unlock(&s->mutex);return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t h){sem_t*s=h;pthread_mutex_destroy(&s->mutex);pthread_cond_destroy(&s->condition);free(s);atomic_fetch_sub(&semaphore_live_count,1);}
static void*task_main(void*p){task_t*t=p;current_task=t;t->entry(t->argument);abort();}
BaseType_t xTaskCreatePinnedToCore(void(*entry)(void*),const char*n,uint32_t z,void*a,uint32_t p,TaskHandle_t*out,int c){(void)n;(void)z;(void)p;(void)c;if(fail_allocation())return 0;task_t*t=calloc(1,sizeof(*t));if(!t)return 0;t->entry=entry;t->argument=a;pthread_mutex_init(&t->mutex,NULL);pthread_cond_init(&t->condition,NULL);*out=t;if(pthread_create(&t->thread,NULL,task_main,t)!=0){pthread_mutex_destroy(&t->mutex);pthread_cond_destroy(&t->condition);free(t);return 0;}pthread_detach(t->thread);atomic_fetch_add(&task_live_count,1);vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_WORKER);return pdPASS;}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t timeout){(void)clear;task_t*t=current_task;pthread_mutex_lock(&t->mutex);if(timeout==portMAX_DELAY){while(!t->notifications)pthread_cond_wait(&t->condition,&t->mutex);}else if(timeout!=0&&!t->notifications){struct timespec deadline;deadline_after(timeout,&deadline);(void)pthread_cond_timedwait(&t->condition,&t->mutex,&deadline);}unsigned n=t->notifications;t->notifications=0;pthread_mutex_unlock(&t->mutex);return n;}
void xTaskNotifyGive(TaskHandle_t h){task_t*t=h;pthread_mutex_lock(&t->mutex);++t->notifications;pthread_cond_broadcast(&t->condition);pthread_mutex_unlock(&t->mutex);}
void vTaskDelete(TaskHandle_t h){assert(h==NULL);current_task=NULL;atomic_fetch_sub(&task_live_count,1);vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_WORKER);pthread_exit(NULL);}

esp_err_t i2s_new_channel(const i2s_chan_config_t*c,void*t,i2s_chan_handle_t*h){(void)c;(void)t;atomic_fetch_add(&i2s_create_count,1);*h=(void*)1;vk_audio_native_resource_allocated(VK_AUDIO_NATIVE_RESOURCE_I2S);return ESP_OK;}
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t h,const i2s_pdm_rx_config_t*c){(void)h;(void)c;atomic_fetch_add(&i2s_initialize_count,1);return ESP_OK;}
esp_err_t i2s_channel_enable(i2s_chan_handle_t h){(void)h;atomic_fetch_add(&i2s_enable_count,1);return ESP_OK;}
esp_err_t i2s_channel_read(i2s_chan_handle_t h,void*b,size_t z,size_t*n,uint32_t t){(void)h;(void)t;atomic_store(&i2s_read_entered,true);while(atomic_load(&i2s_block)){}int e=atomic_load(&i2s_read_error);if(e)return e;memset(b,0,z<16?z:16);*n=z<16?z:16;return ESP_OK;}
esp_err_t i2s_channel_disable(i2s_chan_handle_t h){(void)h;atomic_fetch_add(&i2s_disable_count,1);int remaining=atomic_load(&i2s_disable_failures);if(remaining>0&&atomic_compare_exchange_strong(&i2s_disable_failures,&remaining,remaining-1))return ESP_FAIL;return ESP_OK;}
esp_err_t i2s_del_channel(i2s_chan_handle_t h){(void)h;atomic_fetch_add(&i2s_destroy_count,1);int remaining=atomic_load(&i2s_destroy_failures);if(remaining>0&&atomic_compare_exchange_strong(&i2s_destroy_failures,&remaining,remaining-1))return ESP_FAIL;vk_audio_native_resource_freed(VK_AUDIO_NATIVE_RESOURCE_I2S);return ESP_OK;}
static afe_config_t config;static int afe_data;
afe_config_t*afe_config_init(const char*f,void*m,int t,int p){(void)f;(void)m;(void)t;(void)p;if(atomic_load(&afe_mode)==1)return NULL;return &config;}
void afe_config_free(afe_config_t*c){(void)c;atomic_fetch_add(&afe_config_free_count,1);}
static esp_afe_sr_data_t*afe_create(afe_config_t*c){(void)c;atomic_store(&afe_create_entered,true);while(atomic_load(&afe_create_block)){}return atomic_load(&afe_mode)==12?NULL:(void*)&afe_data;}
static void afe_destroy(esp_afe_sr_data_t*d){(void)d;atomic_fetch_add(&afe_destroy_count,1);}
static int afe_feed(esp_afe_sr_data_t*d,const int16_t*s){(void)d;(void)s;return 0;}
static afe_fetch_result_t*afe_fetch(esp_afe_sr_data_t*d,uint32_t delay){(void)d;(void)delay;return NULL;}
static int feed_size(esp_afe_sr_data_t*d){(void)d;int m=atomic_load(&afe_mode);return m==13?0:m==18?20000:4;}
static int fetch_size(esp_afe_sr_data_t*d){(void)d;int m=atomic_load(&afe_mode);return m==14?0:m==19?40000:960;}
static int feed_channels(esp_afe_sr_data_t*d){(void)d;return atomic_load(&afe_mode)==15?1:2;}
static int fetch_channels(esp_afe_sr_data_t*d){(void)d;return atomic_load(&afe_mode)==16?2:1;}
static int sample_rate(esp_afe_sr_data_t*d){(void)d;return atomic_load(&afe_mode)==17?8000:16000;}
static const esp_afe_sr_iface_t good_iface={afe_create,afe_destroy,afe_feed,afe_fetch,feed_size,fetch_size,feed_channels,fetch_channels,sample_rate};
esp_afe_sr_iface_t*esp_afe_handle_from_config(afe_config_t*c){(void)c;static esp_afe_sr_iface_t selected;int mode=atomic_load(&afe_mode);if(mode==2)return NULL;selected=good_iface;if(mode>=3&&mode<=11){void **members=(void **)&selected;members[mode-3]=NULL;}return &selected;}
OpusEncoder*opus_encoder_create(int r,int c,int a,int*e){(void)r;(void)c;(void)a;*e=OPUS_OK;return malloc(1);}
int opus_encoder_ctl(OpusEncoder*e,int request){(void)e;(void)request;int n=atomic_fetch_add(&opus_ctl_count,1)+1;return n==atomic_load(&opus_ctl_fail_at)?-1:OPUS_OK;}
int opus_encode(OpusEncoder*e,const int16_t*s,int n,uint8_t*p,opus_int32 z){(void)e;(void)s;(void)n;(void)p;(void)z;return 10;}
void opus_encoder_destroy(OpusEncoder*e){atomic_fetch_add(&opus_destroy_count,1);free(e);}
esp_err_t vk_usb_current_epoch(uint32_t*e){*e=7;return ESP_OK;}
esp_err_t vk_usb_send_audio(uint32_t e,const vk_usb_audio_frame_t*f){(void)e;atomic_fetch_add(&usb_send_count,1);if((f->flags&0x02U)!=0U)atomic_fetch_add(&usb_final_count,1);return atomic_load(&usb_send_error);}
esp_err_t vk_usb_register_uac_source(const vk_usb_uac_source_registration_t *source){assert(source&&source->start&&source->read&&source->stop);registered_uac_source=*source;return ESP_OK;}
esp_err_t vk_usb_register_audio_diagnostics_provider(const vk_usb_audio_diagnostics_provider_registration_t *provider){assert(provider&&provider->get_snapshot);registered_diagnostics=*provider;return ESP_OK;}

static void reset_faults(void){atomic_store(&allocation_fail_after,0);atomic_store(&allocation_count,0);atomic_store(&afe_mode,0);atomic_store(&opus_ctl_fail_at,0);atomic_store(&opus_ctl_count,0);atomic_store(&usb_send_error,0);atomic_store(&usb_send_count,0);atomic_store(&usb_final_count,0);atomic_store(&i2s_read_error,ESP_ERR_TIMEOUT);atomic_store(&i2s_disable_failures,0);atomic_store(&i2s_destroy_failures,0);atomic_store(&i2s_create_count,0);atomic_store(&i2s_initialize_count,0);atomic_store(&i2s_enable_count,0);atomic_store(&i2s_disable_count,0);atomic_store(&i2s_destroy_count,0);atomic_store(&i2s_block,false);atomic_store(&i2s_read_entered,false);atomic_store(&afe_create_block,false);atomic_store(&afe_create_entered,false);}
static void*getter_loop(void*p){(void)p;for(int i=0;i<1000;i++){(void)vk_audio_is_active();(void)vk_audio_session_id();(void)vk_audio_is_tainted();}return NULL;}
typedef struct { uint32_t session_id; esp_err_t result; } start_result_t;
static void*start_call(void*p){start_result_t*r=p;r->session_id=0;r->result=vk_audio_start(&r->session_id);return NULL;}
static void*stop_call(void*p){(void)p;return(void*)(intptr_t)vk_audio_stop();}
static void*deinit_call(void*p){(void)p;return(void*)(intptr_t)vk_audio_deinit();}
static void wait_for_read(void){struct timespec pause={0,1000000L};for(int i=0;i<5000&&!atomic_load(&i2s_read_entered);++i)nanosleep(&pause,NULL);assert(atomic_load(&i2s_read_entered));}
static void wait_for_afe_create(void){struct timespec pause={0,1000000L};for(int i=0;i<5000&&!atomic_load(&afe_create_entered);++i)nanosleep(&pause,NULL);assert(atomic_load(&afe_create_entered));}
static void wait_for_no_tasks(void){struct timespec pause={0,1000000L};for(int i=0;i<5000&&atomic_load(&task_live_count)!=0;++i)nanosleep(&pause,NULL);assert(atomic_load(&task_live_count)==0);}
static void assert_all_resources_released(void){for(int r=0;r<VK_AUDIO_NATIVE_RESOURCE_COUNT;r++)assert(atomic_load(&resource_alloc[r])==atomic_load(&resource_free[r]));}
static void assert_only_persistent_i2s(void){for(int r=0;r<VK_AUDIO_NATIVE_RESOURCE_COUNT;r++){int outstanding=atomic_load(&resource_alloc[r])-atomic_load(&resource_free[r]);assert(outstanding==(r==VK_AUDIO_NATIVE_RESOURCE_I2S?1:0));}}
static void assert_retained_resources(bool disable_failed){
    for(int r=0;r<VK_AUDIO_NATIVE_RESOURCE_COUNT;++r){
        bool retained=r==VK_AUDIO_NATIVE_RESOURCE_SESSION||r==VK_AUDIO_NATIVE_RESOURCE_RING||
            r==VK_AUDIO_NATIVE_RESOURCE_I2S||r==VK_AUDIO_NATIVE_RESOURCE_STARTED_SEMAPHORE||
            r==VK_AUDIO_NATIVE_RESOURCE_STOPPED_SEMAPHORE||
            (disable_failed&&(r==VK_AUDIO_NATIVE_RESOURCE_FEED||r==VK_AUDIO_NATIVE_RESOURCE_AFE_CONFIG||r==VK_AUDIO_NATIVE_RESOURCE_AFE_DATA));
        int outstanding=atomic_load(&resource_alloc[r])-atomic_load(&resource_free[r]);
        assert(outstanding==(retained?1:0));
    }
}

int main(int argc, char **argv)
{
    reset_faults();assert(vk_audio_init()==ESP_OK);uint32_t sid=0;
    if(argc==2&&strcmp(argv[1],"startup-timeout")==0){
        atomic_store(&afe_create_block,true);assert(vk_audio_start(&sid)==ESP_ERR_TIMEOUT);assert(vk_audio_is_tainted());assert(vk_audio_start(&sid)==ESP_ERR_INVALID_STATE);assert(vk_audio_deinit()==ESP_ERR_TIMEOUT);atomic_store(&afe_create_block,false);wait_for_no_tasks();assert(vk_audio_is_tainted());assert(vk_audio_deinit()==ESP_OK);assert(vk_audio_is_tainted());puts("vk_audio startup timeout late collection passed");return 0;
    }
    if(argc==2&&strcmp(argv[1],"stop-timeout")==0){
        atomic_store(&i2s_block,true);assert(vk_audio_start(&sid)==ESP_OK);wait_for_read();assert(vk_audio_stop()==ESP_ERR_TIMEOUT);assert(vk_audio_is_tainted());assert(vk_audio_start(&sid)==ESP_ERR_INVALID_STATE);atomic_store(&i2s_block,false);wait_for_no_tasks();assert(vk_audio_deinit()==ESP_OK);assert(vk_audio_is_tainted());puts("vk_audio stop timeout late collection passed");return 0;
    }
    if(argc==2&&strcmp(argv[1],"runtime-failure")==0){
        assert(vk_audio_start(&sid)==ESP_OK);atomic_store(&i2s_read_error,ESP_FAIL);
        wait_for_read();
        const vk_audio_control_api_t *api=vk_audio_control_api();uint32_t failed=0U;
        struct timespec pause={0,1000000L};
        for(int i=0;i<5000&&!api->take_runtime_failure(&failed);++i)nanosleep(&pause,NULL);
        assert(failed==sid);wait_for_no_tasks();assert(!vk_audio_is_active());
        assert(atomic_load(&usb_final_count)==0);
        puts("vk_audio independently collected async runtime failure without EOS");return 0;
    }
    if(argc==2&&strcmp(argv[1],"abort-no-eos")==0){
        atomic_store(&i2s_read_error,ESP_OK);assert(vk_audio_start(&sid)==ESP_OK);wait_for_read();
        assert(vk_audio_abort()==ESP_OK);wait_for_no_tasks();assert(atomic_load(&usb_final_count)==0);
        reset_faults();atomic_store(&i2s_read_error,ESP_OK);assert(vk_audio_start(&sid)==ESP_OK);wait_for_read();
        assert(vk_audio_stop()==ESP_OK);wait_for_no_tasks();assert(atomic_load(&usb_final_count)==1);
        puts("vk_audio abort no-EOS and normal stop EOS passed");return 0;
    }
    if(argc==2&&strcmp(argv[1],"system-mic")==0){
        pthread_mutex_lock(&started_gate_mutex);started_gate_enabled=true;started_gate_entered=false;started_gate_release=false;pthread_mutex_unlock(&started_gate_mutex);
        assert(registered_uac_source.start(registered_uac_source.context)==ESP_OK);
        pthread_mutex_lock(&started_gate_mutex);while(!started_gate_entered)pthread_cond_wait(&started_gate_condition,&started_gate_mutex);started_gate_release=true;pthread_cond_broadcast(&started_gate_condition);pthread_mutex_unlock(&started_gate_mutex);
        wait_for_read();
        uint8_t pcm[320];size_t read_bytes=0U;
        memset(pcm,0xa5,sizeof(pcm));
        assert(registered_uac_source.read(registered_uac_source.context,pcm,sizeof(pcm),&read_bytes)==ESP_OK);
        assert(read_bytes==sizeof(pcm));
        for(size_t index=0;index<sizeof(pcm);++index)assert(pcm[index]==0U);
        registered_uac_source.stop(registered_uac_source.context);wait_for_no_tasks();
        vk_usb_audio_diagnostics_t diagnostics={0};
        assert(registered_diagnostics.get_snapshot(registered_diagnostics.context,7U,&diagnostics)==ESP_OK);
        assert(diagnostics.source_start_attempts==1U&&diagnostics.source_starts==1U&&diagnostics.source_start_failures==0U&&diagnostics.source_stops==1U);
        assert(diagnostics.last_source_start_error==ESP_OK);
        assert(diagnostics.i2s_reads>0U&&diagnostics.i2s_read_bytes==0U);
        assert(diagnostics.i2s_timeouts>0U&&diagnostics.published_samples==0U);
        assert(diagnostics.uac_reads==1U&&diagnostics.uac_underflow_bytes==sizeof(pcm));
        assert(!diagnostics.source_active&&diagnostics.i2s_initialized&&!diagnostics.i2s_enabled);
        atomic_store(&i2s_read_entered,false);
        assert(registered_uac_source.start(registered_uac_source.context)==ESP_OK);wait_for_read();
        registered_uac_source.stop(registered_uac_source.context);wait_for_no_tasks();
        pthread_mutex_lock(&started_gate_mutex);started_gate_enabled=false;pthread_mutex_unlock(&started_gate_mutex);
        assert(atomic_load(&i2s_create_count)==1&&atomic_load(&i2s_initialize_count)==1);
        assert(atomic_load(&i2s_enable_count)==2&&atomic_load(&i2s_disable_count)==2);
        assert(atomic_load(&i2s_destroy_count)==0);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_OK);assert(atomic_load(&i2s_destroy_count)==1);assert_all_resources_released();
        assert(atomic_load(&usb_send_count)==0&&atomic_load(&usb_final_count)==0);
        puts("vk_audio UAC source reuses one persistent PDM channel across opens");return 0;
    }
    if(argc==2&&strcmp(argv[1],"cleanup-disable")==0){
        int baseline=atomic_load(&semaphore_live_count);assert(vk_audio_start(&sid)==ESP_OK&&sid!=0);
        atomic_store(&i2s_disable_failures,2);assert(vk_audio_stop()==ESP_FAIL);wait_for_no_tasks();
        assert(vk_audio_is_tainted());assert(atomic_load(&semaphore_live_count)==baseline+2);assert_retained_resources(true);
        assert(vk_audio_deinit()==ESP_FAIL);assert(atomic_load(&semaphore_live_count)==baseline+2);assert_retained_resources(true);
        assert(vk_audio_deinit()==ESP_OK);assert(atomic_load(&semaphore_live_count)==baseline);assert_all_resources_released();
        assert(vk_audio_deinit()==ESP_ERR_INVALID_STATE);assert_all_resources_released();
        assert(atomic_load(&i2s_disable_count)==3);assert(atomic_load(&i2s_destroy_count)==1);
        puts("vk_audio retained disable cleanup retry passed");return 0;
    }
    if(argc==2&&strcmp(argv[1],"cleanup-persistent")==0){
        assert(vk_audio_start(&sid)==ESP_OK&&sid!=0);atomic_store(&i2s_destroy_failures,4);
        assert(vk_audio_stop()==ESP_OK);wait_for_no_tasks();assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_FAIL);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_FAIL);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_FAIL);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_FAIL);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_OK);assert_all_resources_released();
        assert(atomic_load(&i2s_disable_count)==1);assert(atomic_load(&i2s_destroy_count)==5);
        puts("vk_audio persistent PDM teardown retries until eventual release");return 0;
    }
    if(argc==2&&strcmp(argv[1],"cleanup-destroy")==0){
        int baseline=atomic_load(&semaphore_live_count);assert(vk_audio_start(&sid)==ESP_OK&&sid!=0);
        atomic_store(&i2s_destroy_failures,2);assert(vk_audio_stop()==ESP_OK);wait_for_no_tasks();
        assert(!vk_audio_is_tainted());assert(atomic_load(&semaphore_live_count)==baseline);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_FAIL);assert(atomic_load(&semaphore_live_count)==baseline);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_FAIL);assert(atomic_load(&semaphore_live_count)==baseline);assert_only_persistent_i2s();
        assert(vk_audio_deinit()==ESP_OK);assert(atomic_load(&semaphore_live_count)==baseline);assert_all_resources_released();
        assert(atomic_load(&i2s_disable_count)==1);assert(atomic_load(&i2s_destroy_count)==3);
        puts("vk_audio persistent PDM destroy failure stays retryable");return 0;
    }
    if(argc==2&&strcmp(argv[1],"start-stop")==0){
        /* Stop linearizes first: the contending start is admitted only after the
         * old session is fully collected and receives a distinct identity. */
        uint32_t old_id=0;assert(vk_audio_start(&old_id)==ESP_OK&&old_id!=0);
        pthread_mutex_lock(&stop_gate_mutex);stop_gate_enabled=true;stop_gate_entered=false;stop_gate_release=false;pthread_mutex_unlock(&stop_gate_mutex);
        pthread_t stopper,starter;start_result_t after_stop={0};pthread_create(&stopper,NULL,stop_call,NULL);
        pthread_mutex_lock(&stop_gate_mutex);while(!stop_gate_entered)pthread_cond_wait(&stop_gate_condition,&stop_gate_mutex);pthread_mutex_unlock(&stop_gate_mutex);
        pthread_create(&starter,NULL,start_call,&after_stop);pthread_mutex_lock(&stop_gate_mutex);stop_gate_release=true;pthread_cond_broadcast(&stop_gate_condition);pthread_mutex_unlock(&stop_gate_mutex);
        void*r=NULL;pthread_join(stopper,&r);assert((esp_err_t)(intptr_t)r==ESP_OK);pthread_join(starter,NULL);
        assert(after_stop.result==ESP_OK&&after_stop.session_id!=0&&after_stop.session_id!=old_id);assert(vk_audio_session_id()==after_stop.session_id);assert(vk_audio_stop()==ESP_OK);wait_for_no_tasks();
        pthread_mutex_lock(&stop_gate_mutex);stop_gate_enabled=false;pthread_mutex_unlock(&stop_gate_mutex);
        /* Start linearizes first: stop waits at the exact production mutex while
         * AFE construction holds the admitted start. */
        start_result_t before_stop={0};atomic_store(&afe_create_block,true);atomic_store(&afe_create_entered,false);pthread_create(&starter,NULL,start_call,&before_stop);wait_for_afe_create();pthread_create(&stopper,NULL,stop_call,NULL);atomic_store(&afe_create_block,false);pthread_join(starter,NULL);pthread_join(stopper,&r);
        assert(before_stop.result==ESP_OK&&before_stop.session_id!=0&&before_stop.session_id!=after_stop.session_id);assert((esp_err_t)(intptr_t)r==ESP_OK);wait_for_no_tasks();assert(!vk_audio_is_active());assert_only_persistent_i2s();
        puts("vk_audio deterministic start versus stop passed");return 0;
    }
    if(argc==2&&strcmp(argv[1],"concurrency")==0){
        pthread_t a,b,c,g;start_result_t first={0},second={0};atomic_store(&afe_create_block,true);
        pthread_create(&a,NULL,start_call,&first);wait_for_afe_create();
        pthread_create(&b,NULL,start_call,&second);atomic_store(&afe_create_block,false);pthread_join(a,NULL);pthread_join(b,NULL);
        assert(first.result==ESP_OK&&first.session_id!=0);assert(second.result==ESP_ERR_INVALID_STATE&&second.session_id==0);
        uint32_t first_id=first.session_id;assert(vk_audio_session_id()==first_id);assert(vk_audio_stop()==ESP_OK);wait_for_no_tasks();
        reset_faults();start_result_t third={0};atomic_store(&afe_create_block,true);pthread_create(&a,NULL,start_call,&third);
        wait_for_afe_create();
        pthread_create(&c,NULL,deinit_call,NULL);atomic_store(&afe_create_block,false);pthread_join(a,NULL);void*r=NULL;pthread_join(c,&r);
        assert(third.result==ESP_OK&&third.session_id!=0&&third.session_id!=first_id);assert((esp_err_t)(intptr_t)r==ESP_OK);wait_for_no_tasks();
        pthread_create(&g,NULL,getter_loop,NULL);pthread_create(&c,NULL,deinit_call,NULL);pthread_join(c,&r);assert((esp_err_t)(intptr_t)r==ESP_OK);pthread_join(g,NULL);
        puts("vk_audio deterministic public concurrency passed");return 0;
    }
    assert(vk_audio_start(&sid)==ESP_OK&&sid!=0);pthread_t getters;pthread_create(&getters,NULL,getter_loop,NULL);assert(vk_audio_stop()==ESP_OK);pthread_join(getters,NULL);wait_for_no_tasks();assert(!vk_audio_is_active());assert(vk_audio_stop()==ESP_OK);

    for(int fail=1;fail<=6;fail++){reset_faults();atomic_store(&allocation_fail_after,fail);esp_err_t error=vk_audio_start(&sid);assert(error!=ESP_OK);assert(!vk_audio_is_active());wait_for_no_tasks();}
    for(int mode=1;mode<=19;mode++){reset_faults();atomic_store(&afe_mode,mode);int before_free=atomic_load(&afe_config_free_count),before_destroy=atomic_load(&afe_destroy_count);assert(vk_audio_start(&sid)!=ESP_OK);wait_for_no_tasks();if(mode!=1)assert(atomic_load(&afe_config_free_count)==before_free+1);if(mode>=13)assert(atomic_load(&afe_destroy_count)==before_destroy+1);}
    for(int ctl=1;ctl<=5;ctl++){reset_faults();atomic_store(&opus_ctl_fail_at,ctl);int before=atomic_load(&opus_destroy_count);assert(vk_audio_start(&sid)!=ESP_OK);wait_for_no_tasks();assert(atomic_load(&opus_destroy_count)==before+1);}

    reset_faults();atomic_store(&usb_send_error,ESP_ERR_NO_MEM);assert(vk_audio_start(&sid)==ESP_OK);assert(vk_audio_stop()!=ESP_OK);wait_for_no_tasks();assert(!vk_audio_is_tainted());

    puts("vk_audio production owner native tests passed");
}
