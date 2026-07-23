#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vk_usb_facade.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool acquired;
    bool release;
} fixture_t;

static void lock(void *context) { pthread_mutex_lock(&((fixture_t *)context)->mutex); }
static void unlock(void *context) { pthread_mutex_unlock(&((fixture_t *)context)->mutex); }
static void tick(void *context) { (void)context; usleep(1000); }

typedef struct { vk_usb_facade_t *facade; fixture_t *fixture; } thread_args_t;
static void *borrower(void *argument)
{
    thread_args_t *args = argument;
    void *service = NULL;
    assert(vk_usb_facade_acquire(args->facade, &service) == ESP_OK);
    assert(service == (void *)0x1234);
    pthread_mutex_lock(&args->fixture->mutex);
    args->fixture->acquired = true;
    pthread_cond_broadcast(&args->fixture->condition);
    while (!args->fixture->release) pthread_cond_wait(&args->fixture->condition, &args->fixture->mutex);
    pthread_mutex_unlock(&args->fixture->mutex);
    vk_usb_facade_release(args->facade);
    return NULL;
}

int main(void)
{
    fixture_t fixture = {.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    vk_usb_facade_ops_t ops = {.lock=lock,.unlock=unlock,.wait_tick=tick,.context=&fixture};
    vk_usb_facade_t facade;
    vk_usb_facade_init(&facade,&ops);
    assert(vk_usb_facade_publish(&facade,(void *)0x1234)==ESP_OK);
    thread_args_t args={&facade,&fixture}; pthread_t thread;
    assert(pthread_create(&thread,NULL,borrower,&args)==0);
    pthread_mutex_lock(&fixture.mutex);
    while(!fixture.acquired)pthread_cond_wait(&fixture.condition,&fixture.mutex);
    pthread_mutex_unlock(&fixture.mutex);
    assert(vk_usb_facade_close(&facade,0)==ESP_ERR_TIMEOUT);
    void *service=NULL;
    assert(vk_usb_facade_acquire(&facade,&service)==ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&fixture.mutex); fixture.release=true; pthread_cond_broadcast(&fixture.condition); pthread_mutex_unlock(&fixture.mutex);
    pthread_join(thread,NULL);
    assert(vk_usb_facade_close(&facade,1)==ESP_ERR_INVALID_STATE);

    vk_usb_facade_init(&facade,&ops);
    assert(vk_usb_facade_publish(&facade,(void *)0x1234)==ESP_OK);
    assert(vk_usb_facade_close(&facade,1)==ESP_OK);
    assert(vk_usb_facade_acquire(&facade,&service)==ESP_ERR_INVALID_STATE);
    puts("vk_usb facade native tests passed");
}
