#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vk_usb.h"
#include "vk_usb_native_platform.h"

typedef struct { pthread_mutex_t mutex; pthread_cond_t condition; unsigned count; } sem_t;
typedef struct { pthread_t thread; pthread_mutex_t mutex; pthread_cond_t condition; unsigned notifications; void(*entry)(void*); void*argument; } task_t;
static _Thread_local task_t*current;
static atomic_bool read_block;
static atomic_bool read_entered;
static atomic_bool write_block;
static atomic_int install_error, uninstall_error, writes, task_live_count;
static atomic_int transport_pending;
static pthread_mutex_t written_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint8_t written_frames[32][VK_USB_MAX_FRAME_BYTES];
static size_t written_lengths[32];
static const uint8_t transport_frame[]={1,0x10,34,0,'{','"','e','v','e','n','t','"',':','"','t','r','a','n','s','p','o','r','t','"',',','"','k','i','n','d','"',':','"','u','s','b','"','}'};

static void deadline_after(TickType_t ms,struct timespec*d){clock_gettime(CLOCK_REALTIME,d);d->tv_sec+=(time_t)(ms/1000U);d->tv_nsec+=(long)(ms%1000U)*1000000L;if(d->tv_nsec>=1000000000L){++d->tv_sec;d->tv_nsec-=1000000000L;}}
SemaphoreHandle_t xSemaphoreCreateBinary(void){sem_t*s=calloc(1,sizeof(*s));if(!s)return NULL;pthread_mutex_init(&s->mutex,NULL);pthread_cond_init(&s->condition,NULL);return s;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){sem_t*s=xSemaphoreCreateBinary();if(s)s->count=1;return s;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t h,TickType_t timeout){sem_t*s=h;pthread_mutex_lock(&s->mutex);if(timeout==0&&!s->count){pthread_mutex_unlock(&s->mutex);return 0;}if(timeout==portMAX_DELAY){while(!s->count)pthread_cond_wait(&s->condition,&s->mutex);}else{struct timespec d;deadline_after(timeout,&d);int e=0;while(!s->count&&e!=ETIMEDOUT)e=pthread_cond_timedwait(&s->condition,&s->mutex,&d);if(!s->count){pthread_mutex_unlock(&s->mutex);return 0;}}--s->count;pthread_mutex_unlock(&s->mutex);return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h){sem_t*s=h;pthread_mutex_lock(&s->mutex);++s->count;pthread_cond_broadcast(&s->condition);pthread_mutex_unlock(&s->mutex);return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t h){sem_t*s=h;pthread_mutex_destroy(&s->mutex);pthread_cond_destroy(&s->condition);free(s);}
static void*run(void*p){task_t*t=p;current=t;t->entry(t->argument);abort();}
BaseType_t xTaskCreatePinnedToCore(void(*e)(void*),const char*n,uint32_t z,void*a,uint32_t p,TaskHandle_t*out,int c){(void)n;(void)z;(void)p;(void)c;task_t*t=calloc(1,sizeof(*t));if(!t)return 0;t->entry=e;t->argument=a;pthread_mutex_init(&t->mutex,NULL);pthread_cond_init(&t->condition,NULL);*out=t;if(pthread_create(&t->thread,NULL,run,t)!=0){free(t);return 0;}pthread_detach(t->thread);atomic_fetch_add(&task_live_count,1);return pdPASS;}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t timeout){(void)clear;task_t*t=current;pthread_mutex_lock(&t->mutex);if(timeout==portMAX_DELAY){while(!t->notifications)pthread_cond_wait(&t->condition,&t->mutex);}unsigned n=t->notifications;t->notifications=0;pthread_mutex_unlock(&t->mutex);return n;}
void xTaskNotifyGive(TaskHandle_t h){task_t*t=h;pthread_mutex_lock(&t->mutex);++t->notifications;pthread_cond_broadcast(&t->condition);pthread_mutex_unlock(&t->mutex);}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
void vTaskDelete(TaskHandle_t h){assert(h==NULL);task_t*t=current;pthread_mutex_destroy(&t->mutex);pthread_cond_destroy(&t->condition);free(t);current=NULL;atomic_fetch_sub(&task_live_count,1);pthread_exit(NULL);}
void vTaskDelay(TickType_t t){(void)t;sched_yield();}

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t*c){(void)c;return atomic_load(&install_error);}
esp_err_t usb_serial_jtag_driver_uninstall(void){return atomic_load(&uninstall_error);}
int usb_serial_jtag_read_bytes(void*b,uint32_t z,TickType_t t){(void)t;if(atomic_load(&read_block)){atomic_store(&read_entered,true);while(atomic_load(&read_block)){}}int pending=atomic_load(&transport_pending);if(pending>0&&atomic_compare_exchange_strong(&transport_pending,&pending,pending-1)){assert(z>=sizeof(transport_frame));memcpy(b,transport_frame,sizeof(transport_frame));return (int)sizeof(transport_frame);}return 0;}
int usb_serial_jtag_write_bytes(const void*b,size_t z,TickType_t t){(void)t;while(atomic_load(&write_block)){}pthread_mutex_lock(&written_mutex);int at=atomic_load(&writes);assert(at<32&&z<=VK_USB_MAX_FRAME_BYTES);memcpy(written_frames[at],b,z);written_lengths[at]=z;atomic_store(&writes,at+1);pthread_mutex_unlock(&written_mutex);return (int)z;}
int64_t esp_timer_get_time(void){return 0;}
esp_err_t esp_read_mac(uint8_t*m,int kind){(void)kind;memset(m,1,6);return ESP_OK;}
const esp_app_desc_t*esp_app_get_description(void){static esp_app_desc_t d={"test"};return &d;}

static bool contains_bytes(const uint8_t*b,size_t n,const char*s){size_t z=strlen(s);for(size_t i=0;i+z<=n;i++)if(memcmp(b+i,s,z)==0)return true;return false;}
static void wait_epoch(uint32_t*epoch){struct timespec pause={0,1000000L};for(int i=0;i<5000;i++){if(vk_usb_current_epoch(epoch)==ESP_OK)return;nanosleep(&pause,NULL);}assert(false);}
static void wait_tasks(void){struct timespec pause={0,1000000L};for(int i=0;i<5000&&atomic_load(&task_live_count);i++)nanosleep(&pause,NULL);assert(!atomic_load(&task_live_count));}
static void wait_read_entered(void){struct timespec pause={0,1000000L};for(int i=0;i<5000&&!atomic_load(&read_entered);i++)nanosleep(&pause,NULL);assert(atomic_load(&read_entered));}
static void*send_thread(void*p){uint32_t e=*(uint32_t*)p;vk_usb_audio_frame_t f={.session_id=1};return(void*)(intptr_t)vk_usb_send_audio(e,&f);}
static void*stop_thread(void*p){(void)p;return(void*)(intptr_t)vk_usb_stop();}
static void*epoch_thread(void*p){uint32_t e=0;(void)p;return(void*)(intptr_t)vk_usb_current_epoch(&e);}
static void*replace_epoch_thread(void*p){(void)p;return(void*)(intptr_t)vk_usb_consume_for_test(transport_frame,sizeof(transport_frame));}
typedef struct{pthread_mutex_t mutex;pthread_cond_t condition;bool entered,release;}commit_gate_t;
static void commit_gate(void*p){commit_gate_t*g=p;pthread_mutex_lock(&g->mutex);g->entered=true;pthread_cond_broadcast(&g->condition);while(!g->release)pthread_cond_wait(&g->condition,&g->mutex);pthread_mutex_unlock(&g->mutex);}
static void observe_gate(void*p){commit_gate_t*g=p;pthread_mutex_lock(&g->mutex);g->entered=true;pthread_cond_broadcast(&g->condition);pthread_mutex_unlock(&g->mutex);}
static void wait_gate_entered(commit_gate_t*g){pthread_mutex_lock(&g->mutex);while(!g->entered)pthread_cond_wait(&g->condition,&g->mutex);pthread_mutex_unlock(&g->mutex);}
static uint32_t read_le32(const uint8_t*p){return(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}

int main(int argc,char**argv)
{
    atomic_store(&transport_pending,1);
    assert(vk_usb_start()==ESP_OK);uint32_t epoch=0;wait_epoch(&epoch);assert(epoch!=0);
    atomic_store(&transport_pending,1);uint32_t replaced=epoch;for(int i=0;i<5000&&replaced==epoch;i++){(void)vk_usb_current_epoch(&replaced);sched_yield();}assert(replaced!=0&&replaced!=epoch);epoch=replaced;
    vk_usb_audio_frame_t frame={.session_id=1};assert(vk_usb_send_audio(epoch+1,&frame)==ESP_ERR_INVALID_STATE);

    if(argc==2&&strcmp(argv[1],"timeout")==0){atomic_store(&read_block,true);wait_read_entered();assert(vk_usb_stop()==ESP_ERR_TIMEOUT);assert(vk_usb_start()==ESP_ERR_INVALID_STATE);atomic_store(&read_block,false);wait_tasks();puts("vk_usb production timeout retention passed");return 0;}
    if(argc==2&&strcmp(argv[1],"cleanup-failure")==0){atomic_store(&uninstall_error,ESP_FAIL);assert(vk_usb_stop()==ESP_FAIL);wait_tasks();assert(vk_usb_start()==ESP_ERR_INVALID_STATE);puts("vk_usb production cleanup taint passed");return 0;}

    if(argc==2&&strcmp(argv[1],"concurrent-stop")==0){pthread_t sender,stopper,query;pthread_create(&sender,NULL,send_thread,&epoch);pthread_create(&query,NULL,epoch_thread,NULL);pthread_create(&stopper,NULL,stop_thread,NULL);void*r;pthread_join(sender,&r);esp_err_t se=(esp_err_t)(intptr_t)r;assert(se==ESP_OK||se==ESP_ERR_INVALID_STATE);pthread_join(query,&r);esp_err_t qe=(esp_err_t)(intptr_t)r;assert(qe==ESP_OK||qe==ESP_ERR_INVALID_STATE);pthread_join(stopper,&r);assert((esp_err_t)(intptr_t)r==ESP_OK);wait_tasks();assert(vk_usb_send_audio(epoch,&frame)==ESP_ERR_INVALID_STATE);puts("vk_usb production concurrent stop passed");return 0;}

    if(argc==2&&strcmp(argv[1],"precommit-stop")==0){
        commit_gate_t gate={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
        vk_usb_set_before_tx_commit_hook_for_test(commit_gate,&gate);assert(vk_usb_send_audio(epoch,&frame)==ESP_OK);
        wait_gate_entered(&gate);
        commit_gate_t stopped={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
        vk_usb_set_stop_requested_hook_for_test(observe_gate,&stopped);
        pthread_t stopper;pthread_create(&stopper,NULL,stop_thread,NULL);
        wait_gate_entered(&stopped);assert(vk_usb_send_audio(epoch,&frame)==ESP_ERR_INVALID_STATE);
        pthread_mutex_lock(&gate.mutex);gate.release=true;pthread_cond_broadcast(&gate.condition);pthread_mutex_unlock(&gate.mutex);
        void*r=NULL;pthread_join(stopper,&r);assert((esp_err_t)(intptr_t)r==ESP_OK);wait_tasks();
        assert(atomic_load(&writes)==0);assert(vk_usb_current_epoch(&epoch)==ESP_ERR_INVALID_STATE);
        puts("vk_usb production precommit stop cancellation passed");return 0;
    }
    if(argc==2&&strcmp(argv[1],"precommit-replace")==0){
        commit_gate_t gate={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
        uint32_t old_epoch=epoch;vk_usb_set_before_tx_commit_hook_for_test(commit_gate,&gate);assert(vk_usb_send_audio(old_epoch,&frame)==ESP_OK);
        pthread_mutex_lock(&gate.mutex);while(!gate.entered)pthread_cond_wait(&gate.condition,&gate.mutex);pthread_mutex_unlock(&gate.mutex);
        pthread_t replacer;pthread_create(&replacer,NULL,replace_epoch_thread,NULL);void*r=NULL;pthread_join(replacer,&r);assert((esp_err_t)(intptr_t)r==ESP_OK);
        assert(vk_usb_current_epoch(&epoch)==ESP_OK&&epoch!=old_epoch);assert(vk_usb_send_audio(old_epoch,&frame)==ESP_ERR_INVALID_STATE);
        pthread_mutex_lock(&gate.mutex);gate.release=true;pthread_cond_broadcast(&gate.condition);pthread_mutex_unlock(&gate.mutex);
        for(int i=0;i<100000;i++)sched_yield();assert(atomic_load(&writes)==0);
        commit_gate_t poll_returned={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
        vk_usb_set_poll_returned_hook_for_test(observe_gate,&poll_returned);
        vk_usb_set_before_tx_commit_hook_for_test(NULL,NULL);
        wait_gate_entered(&poll_returned);
        pthread_mutex_lock(&poll_returned.mutex);poll_returned.entered=false;pthread_mutex_unlock(&poll_returned.mutex);
        const uint8_t payload[]={0xA5,0x5A,0x01};
        vk_usb_audio_frame_t expected={.session_id=0x11223344U,.sequence=0x55667788U,.flags=0x03U,.payload=payload,.payload_length=sizeof(payload)};
        assert(vk_usb_send_audio(epoch,&expected)==ESP_OK);
        for(int i=0;i<1000000&&atomic_load(&writes)==0;i++)sched_yield();assert(atomic_load(&writes)==1);
        pthread_mutex_lock(&written_mutex);
        assert(written_lengths[0]==16U+sizeof(payload));assert(written_frames[0][0]==1&&written_frames[0][1]==VK_USB_FRAME_TYPE_AUDIO);
        assert(read_le32(written_frames[0]+4)==expected.session_id);assert(read_le32(written_frames[0]+8)==expected.sequence);
        assert(written_frames[0][12]==expected.flags&&written_frames[0][13]==0);
        assert(((uint16_t)written_frames[0][14]|((uint16_t)written_frames[0][15]<<8))==sizeof(payload));
        assert(memcmp(written_frames[0]+16,payload,sizeof(payload))==0);pthread_mutex_unlock(&written_mutex);
        wait_gate_entered(&poll_returned);
        assert(vk_usb_stop()==ESP_OK);wait_tasks();puts("vk_usb production precommit epoch replacement passed");return 0;
    }

    /* Deterministically dequeue one audio value, then close it with the real
     * overflow terminal before the service can commit the value. */
    commit_gate_t gate={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    vk_usb_set_before_tx_commit_hook_for_test(commit_gate,&gate);
    assert(vk_usb_send_audio(epoch,&frame)==ESP_OK);
    pthread_mutex_lock(&gate.mutex);while(!gate.entered)pthread_cond_wait(&gate.condition,&gate.mutex);pthread_mutex_unlock(&gate.mutex);
    for(size_t i=0;i<VK_USB_TYPED_TX_QUEUE_CAPACITY;i++)assert(vk_usb_send_audio(epoch,&frame)==ESP_OK);
    assert(vk_usb_send_audio(epoch,&frame)==ESP_ERR_NO_MEM);
    assert(vk_usb_send_audio(epoch,&frame)==ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&gate.mutex);gate.release=true;pthread_cond_broadcast(&gate.condition);pthread_mutex_unlock(&gate.mutex);
    for(int i=0;i<1000000&&atomic_load(&writes)==0;i++)sched_yield();assert(atomic_load(&writes)==1);
    pthread_mutex_lock(&written_mutex);assert(written_lengths[0]>4);assert(written_frames[0][1]==0x10);assert(contains_bytes(written_frames[0]+4,written_lengths[0]-4,"audio_queue_overflow"));pthread_mutex_unlock(&written_mutex);
    vk_usb_set_before_tx_commit_hook_for_test(NULL,NULL);

    assert(vk_usb_stop()==ESP_OK);wait_tasks();assert(vk_usb_current_epoch(&epoch)==ESP_ERR_INVALID_STATE);
    puts("vk_usb production service native tests passed");
}
