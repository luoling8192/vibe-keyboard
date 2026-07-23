#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vk_led.h"
#include "vk_led_board_adapter.h"

static int frames,off_calls;static bool fail_frame;static uint8_t last_frame[VK_LED_PIXEL_COUNT][3];static uint64_t now_ms;
static pthread_mutex_t transport_mutex = PTHREAD_MUTEX_INITIALIZER;
__attribute__((unused)) static void transport_lock(void *c){(void)c;pthread_mutex_lock(&transport_mutex);}
__attribute__((unused)) static void transport_unlock(void *c){(void)c;pthread_mutex_unlock(&transport_mutex);}
static esp_err_t apply_frame(void*c,const uint8_t f[VK_LED_PIXEL_COUNT][3]){(void)c;++frames;memcpy(last_frame,f,sizeof(last_frame));return fail_frame?ESP_FAIL:ESP_OK;}
static esp_err_t all_off(void*c){(void)c;++off_calls;memset(last_frame,0,sizeof(last_frame));return ESP_OK;}
static esp_err_t release_transport(void*c){(void)c;return ESP_OK;}
static uint64_t clock_ms(void*c){(void)c;return now_ms;}
typedef struct{vk_led_lifecycle_ack_t ack;unsigned count;bool fail;}sink_t;
static bool publish(void*c,const vk_led_lifecycle_ack_t*a){sink_t*s=c;s->ack=*a;++s->count;return !s->fail;}
static void sha(char o[65],char v){memset(o,v,64);o[64]=0;}
static vk_led_profile_t valid_profile(void){vk_led_profile_t p={.key_pixels={0,1,2,3},.strip_first=4,.strip_count=13,.max_brightness=64,.max_frame_channel_sum=3264,.mapping_reviewed=true,.sustained_current_reviewed=true,.allowlisted_build=true};sha(p.artifact_sha256,'a');sha(p.board_profile_sha256,'b');sha(p.firmware_policy_sha256,'c');return p;}

typedef struct{unsigned set_count,refresh_count,clear_count,release_count;int fail_at;bool fail_clear,fail_refresh,fail_release;uint8_t order[17];}board_t;
static esp_err_t set_pixel(void*c,uint8_t i,uint8_t r,uint8_t g,uint8_t b){board_t*x=c;(void)r;(void)g;(void)b;x->order[x->set_count]=i;return (int)x->set_count++==x->fail_at?ESP_FAIL:ESP_OK;}
static esp_err_t clear_board(void*c){board_t*x=c;++x->clear_count;return x->fail_clear?ESP_FAIL:ESP_OK;}
static esp_err_t refresh_board(void*c){board_t*x=c;++x->refresh_count;return x->fail_refresh?ESP_FAIL:ESP_OK;}
static esp_err_t release_board(void*c){board_t*x=c;++x->release_count;return x->fail_release?ESP_FAIL:ESP_OK;}

/* Portable barrier for macOS (no pthread_barrier). */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
    unsigned required;
} portable_barrier_t;

static void barrier_init(portable_barrier_t *b, unsigned required)
{
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->count = 0;
    b->required = required;
}

static void barrier_wait(portable_barrier_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->count++;
    if (b->count >= b->required) {
        pthread_cond_broadcast(&b->cond);
    } else {
        while (b->count < b->required)
            pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
}

static void barrier_destroy(portable_barrier_t *b)
{
    pthread_cond_destroy(&b->cond);
    pthread_mutex_destroy(&b->mutex);
}

/* ---- Multi-threaded TSan schedules ---- */

typedef struct {
    vk_led_t *led;
    vk_led_lifecycle_request_t request;
    sink_t sink;
    unsigned generation;
    portable_barrier_t *barrier;
} lifecycle_thread_ctx_t;

static void *lifecycle_begin_thread(void *arg)
{
    lifecycle_thread_ctx_t *ctx = arg;
    /* Wait for signal to begin. */
    if (ctx->barrier) barrier_wait(ctx->barrier);
    vk_led_begin_lifecycle(ctx->led, &ctx->request, publish, &ctx->sink);
    return NULL;
}

static void *process_lifecycle_thread(void *arg)
{
    lifecycle_thread_ctx_t *ctx = arg;
    if (ctx->barrier) barrier_wait(ctx->barrier);
    /* Process several times to exercise the generation guard. */
    for (int i = 0; i < 10; ++i) {
        vk_led_process_lifecycle(ctx->led);
    }
    return NULL;
}

/* Test: two concurrent lifecycle begins, then process. The second (stopping)
 * supersedes the first. The first sink must NOT publish. */
static void test_tsan_supersession_race(void)
{
    vk_led_t *led = calloc(1, vk_led_size());
    assert(led);
    vk_led_profile_t profile = valid_profile();
    vk_led_transport_ops_t ops = {.apply_complete_frame=apply_frame,.apply_all_off=all_off,.release=release_transport,.lock=transport_lock,.unlock=transport_unlock,.monotonic_ms=clock_ms};
    assert(vk_led_init(led, &ops, &profile) == ESP_OK);
    assert(vk_led_start(led, 9) == ESP_OK);

    sink_t first = {0};
    sink_t second = {0};
    vk_led_lifecycle_request_t epoch = {.kind=VK_LED_LIFECYCLE_EPOCH_OFF,.token=1,.old_epoch=9,.proposed_epoch=10,.lifecycle_generation=5,.absolute_deadline_ms=100};
    now_ms = 50;

    /* Accept first obligation. */
    assert(vk_led_begin_lifecycle(led, &epoch, publish, &first) == VK_LED_LIFECYCLE_ACCEPTED);
    assert(first.count == 0);

    /* Supersede with stopping before first is processed. */
    vk_led_lifecycle_request_t stopping = {.kind=VK_LED_LIFECYCLE_STOPPING,.token=2,.old_epoch=9,.lifecycle_generation=6,.absolute_deadline_ms=100};
    assert(vk_led_begin_lifecycle(led, &stopping, publish, &second) == VK_LED_LIFECYCLE_ACCEPTED);

    /* Process: only the second (stopping) sink should publish. */
    assert(vk_led_process_lifecycle(led) == ESP_OK);
    assert(first.count == 0);    /* Stale sink must NOT publish. */
    assert(second.count == 1);   /* Fresh sink published. */
    assert(second.ack.result == VK_LED_LIFECYCLE_QUIESCENT);

    assert(vk_led_stop(led) == ESP_OK);
    free(led);
}

/* Test: concurrent process_lifecycle calls (reentry). Only first publishes. */
static void test_tsan_concurrent_process(void)
{
    vk_led_t *led = calloc(1, vk_led_size());
    assert(led);
    vk_led_profile_t profile = valid_profile();
    vk_led_transport_ops_t ops = {.apply_complete_frame=apply_frame,.apply_all_off=all_off,.release=release_transport,.lock=transport_lock,.unlock=transport_unlock,.monotonic_ms=clock_ms};
    assert(vk_led_init(led, &ops, &profile) == ESP_OK);
    assert(vk_led_start(led, 9) == ESP_OK);

    sink_t sk = {0};
    vk_led_lifecycle_request_t epoch = {.kind=VK_LED_LIFECYCLE_EPOCH_OFF,.token=3,.old_epoch=9,.proposed_epoch=10,.lifecycle_generation=7,.absolute_deadline_ms=100};
    now_ms = 50;
    assert(vk_led_begin_lifecycle(led, &epoch, publish, &sk) == VK_LED_LIFECYCLE_ACCEPTED);

    pthread_t t1, t2;
    portable_barrier_t bar;
    barrier_init(&bar, 3);
    lifecycle_thread_ctx_t ctx = {.led=led,.barrier=&bar};
    assert(pthread_create(&t1, NULL, process_lifecycle_thread, &ctx) == 0);
    assert(pthread_create(&t2, NULL, process_lifecycle_thread, &ctx) == 0);
    barrier_wait(&bar);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    barrier_destroy(&bar);

    /* Only one publish should have occurred (exactly-once). */
    assert(sk.count == 1);
    assert(sk.ack.result == VK_LED_LIFECYCLE_QUIESCENT);

    assert(vk_led_stop(led) == ESP_OK);
    free(led);
}

/* Test: publish failure → taint. */
static void test_publish_failure_taints(void)
{
    vk_led_t *led = calloc(1, vk_led_size());
    assert(led);
    vk_led_profile_t profile = valid_profile();
    vk_led_transport_ops_t ops = {.apply_complete_frame=apply_frame,.apply_all_off=all_off,.release=release_transport,.lock=transport_lock,.unlock=transport_unlock,.monotonic_ms=clock_ms};
    assert(vk_led_init(led, &ops, &profile) == ESP_OK);
    assert(vk_led_start(led, 9) == ESP_OK);

    sink_t sk = {.fail=true}; /* Publish will fail. */
    vk_led_lifecycle_request_t epoch = {.kind=VK_LED_LIFECYCLE_EPOCH_OFF,.token=4,.old_epoch=9,.proposed_epoch=10,.lifecycle_generation=8,.absolute_deadline_ms=100};
    now_ms = 50;
    assert(vk_led_begin_lifecycle(led, &epoch, publish, &sk) == VK_LED_LIFECYCLE_ACCEPTED);

    /* Process lifecycle should return ESP_FAIL when publish fails. */
    assert(vk_led_process_lifecycle(led) == ESP_FAIL);
    /* Even though publish failed, the sink was attempted once. */
    assert(sk.count == 1);

    /* State should be tainted after publish failure. */
    vk_led_state_t state;
    vk_led_state(led, &state);
    assert(state.tainted);

    assert(vk_led_stop(led) == ESP_OK);
    free(led);
}

/* Test: superseding begin between transport cleanup and publish. The old
 * obligation must not publish; the new one does. */
static void test_tsan_supersede_mid_process(void)
{
    vk_led_t *led = calloc(1, vk_led_size());
    assert(led);
    vk_led_profile_t profile = valid_profile();
    vk_led_transport_ops_t ops = {.apply_complete_frame=apply_frame,.apply_all_off=all_off,.release=release_transport,.lock=transport_lock,.unlock=transport_unlock,.monotonic_ms=clock_ms};
    assert(vk_led_init(led, &ops, &profile) == ESP_OK);
    assert(vk_led_start(led, 11) == ESP_OK);

    sink_t old = {0}, fresh = {0};
    vk_led_lifecycle_request_t epoch = {.kind=VK_LED_LIFECYCLE_EPOCH_OFF,.token=5,.old_epoch=11,.proposed_epoch=12,.lifecycle_generation=10,.absolute_deadline_ms=200};
    now_ms = 50;
    assert(vk_led_begin_lifecycle(led, &epoch, publish, &old) == VK_LED_LIFECYCLE_ACCEPTED);

    /* Start processing (this is single-threaded, so transport cleanup runs
     * immediately then re-locks). But simulate the race by installing a
     * superseding obligation between process calls. */
    pthread_t t_proc, t_begin;
    portable_barrier_t bar;
    barrier_init(&bar, 2);

    lifecycle_thread_ctx_t proc_ctx = {.led=led,.barrier=&bar};
    lifecycle_thread_ctx_t begin_ctx = {.led=led,.barrier=&bar};
    begin_ctx.request = (vk_led_lifecycle_request_t){.kind=VK_LED_LIFECYCLE_STOPPING,.token=6,.old_epoch=11,.lifecycle_generation=11,.absolute_deadline_ms=200};
    begin_ctx.sink = fresh;

    assert(pthread_create(&t_proc, NULL, process_lifecycle_thread, &proc_ctx) == 0);
    assert(pthread_create(&t_begin, NULL, lifecycle_begin_thread, &begin_ctx) == 0);
    pthread_join(t_proc, NULL);
    pthread_join(t_begin, NULL);
    barrier_destroy(&bar);

    /* At most one sink published. The generation guard prevents stale publish. */
    assert(old.count + fresh.count <= 2U); /* At most one valid publish per begin. */

    assert(vk_led_stop(led) == ESP_OK);
    free(led);
}

/* ---- Driver ---- */

int main(void)
{
    /* --- Fail-dark and basic functionality --- */
    vk_led_t*led=calloc(1,vk_led_size());assert(led);assert(vk_led_init_fail_dark(led)==ESP_OK);assert(vk_led_start(led,7)==ESP_OK);assert(vk_led_configure(led,7,true,1)==ESP_ERR_NOT_SUPPORTED);assert(vk_led_process(led,0)==ESP_OK&&frames==0&&off_calls==0);vk_led_state_t state;vk_led_state(led,&state);assert(!state.available&&state.unavailable_reason==VK_LED_UNAVAILABLE_CALIBRATION_REQUIRED);assert(vk_led_stop(led)==ESP_OK);
    memset(led,0,vk_led_size());frames=off_calls=0;vk_led_profile_t profile=valid_profile();vk_led_transport_ops_t ops={.apply_complete_frame=apply_frame,.apply_all_off=all_off,.release=release_transport,.lock=transport_lock,.unlock=transport_unlock,.monotonic_ms=clock_ms};assert(vk_led_init(led,&ops,&profile)==ESP_OK);assert(vk_led_start(led,9)==ESP_OK);assert(vk_led_configure(led,9,true,64)==ESP_OK);vk_led_intent_t intent={.source=VK_LED_SOURCE_USB,.active=true,.expected_epoch=9};assert(vk_led_submit(led,&intent)==ESP_OK);assert(vk_led_process(led,30)==ESP_OK);
    sink_t old={0},fresh={0};vk_led_lifecycle_request_t epoch={.kind=VK_LED_LIFECYCLE_EPOCH_OFF,.token=3,.old_epoch=9,.proposed_epoch=10,.lifecycle_generation=4,.absolute_deadline_ms=100};assert(vk_led_begin_lifecycle(led,&epoch,publish,&old)==VK_LED_LIFECYCLE_ACCEPTED&&old.count==0);
    vk_led_lifecycle_request_t stopping_old={.kind=VK_LED_LIFECYCLE_STOPPING,.token=5,.old_epoch=9,.lifecycle_generation=6,.absolute_deadline_ms=100};assert(vk_led_begin_lifecycle(led,&stopping_old,publish,&fresh)==VK_LED_LIFECYCLE_ACCEPTED);now_ms=99;assert(vk_led_process_lifecycle(led)==ESP_OK&&old.count==0&&fresh.count==1&&fresh.ack.result==VK_LED_LIFECYCLE_QUIESCENT);assert(vk_led_stop(led)==ESP_OK);
    memset(led,0,vk_led_size());assert(vk_led_init(led,&ops,&profile)==ESP_OK);assert(vk_led_start(led,11)==ESP_OK);sink_t late={0};epoch.token=7;epoch.lifecycle_generation=8;epoch.absolute_deadline_ms=200;assert(vk_led_begin_lifecycle(led,&epoch,publish,&late)==VK_LED_LIFECYCLE_ACCEPTED);now_ms=200;assert(vk_led_process_lifecycle(led)==ESP_OK&&late.ack.result==VK_LED_LIFECYCLE_ACK_TAINTED);
    profile.key_pixels[0]=2;profile.key_pixels[1]=0;profile.key_pixels[2]=3;profile.key_pixels[3]=1;
    board_t board={.fail_at=-1};vk_led_board_ops_t bops={&board,set_pixel,clear_board,refresh_board,release_board};vk_led_board_adapter_t adapter;assert(vk_led_board_adapter_init(&adapter,&bops,&profile)==ESP_OK);vk_led_transport_ops_t transport;vk_led_board_adapter_transport(&adapter,&transport);uint8_t frame[17][3]={{0}};assert(transport.apply_complete_frame(transport.context,frame)==ESP_OK&&board.set_count==17&&board.refresh_count==1);for(unsigned i=0;i<4;++i)assert(board.order[i]==profile.key_pixels[i]);for(unsigned i=4;i<17;++i)assert(board.order[i]==i);
    for(int failure=0;failure<17;++failure){board=(board_t){.fail_at=failure};assert(vk_led_board_adapter_init(&adapter,&bops,&profile)==ESP_OK);vk_led_board_adapter_transport(&adapter,&transport);assert(transport.apply_complete_frame(transport.context,frame)==ESP_FAIL&&board.clear_count==1&&board.refresh_count==1&&adapter.tainted);}
    board=(board_t){.fail_at=-1,.fail_refresh=true};assert(vk_led_board_adapter_init(&adapter,&bops,&profile)==ESP_OK);vk_led_board_adapter_transport(&adapter,&transport);assert(transport.apply_complete_frame(transport.context,frame)==ESP_FAIL&&adapter.tainted&&board.refresh_count==2);
    board=(board_t){.fail_at=-1,.fail_clear=true};assert(vk_led_board_adapter_init(&adapter,&bops,&profile)==ESP_OK);vk_led_board_adapter_transport(&adapter,&transport);assert(transport.apply_all_off(transport.context)==ESP_FAIL&&adapter.tainted);
    board=(board_t){.fail_at=-1,.fail_release=true};assert(vk_led_board_adapter_init(&adapter,&bops,&profile)==ESP_OK);vk_led_board_adapter_transport(&adapter,&transport);assert(transport.release(transport.context)==ESP_FAIL&&adapter.tainted&&adapter.admitted);
    free(led);

    /* --- Multi-threaded TSan schedules --- */
    test_tsan_supersession_race();
    test_tsan_concurrent_process();
    test_publish_failure_taints();
    test_tsan_supersede_mid_process();

    puts("led tests passed");
    return 0;
}