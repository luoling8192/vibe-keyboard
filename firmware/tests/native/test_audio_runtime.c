#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "vk_audio_runtime.h"

typedef struct {
    esp_err_t prepare_error;
    esp_err_t create_error;
    esp_err_t startup_error;
    esp_err_t runtime_error;
    esp_err_t cleanup_error;
    bool start_wait;
    bool stop_wait;
    unsigned prepare_count;
    unsigned create_count;
    unsigned request_count;
    unsigned cleanup_retry_count;
    unsigned release_count;
    uint32_t start_timeout;
    uint32_t stop_timeout;
} fake_t;

static esp_err_t prepare(void *c) { fake_t *f=c; ++f->prepare_count; return f->prepare_error; }
static esp_err_t create(void *c) { fake_t *f=c; ++f->create_count; return f->create_error; }
static bool wait_started(void *c,uint32_t t){fake_t*f=c;f->start_timeout=t;return f->start_wait;}
static esp_err_t startup(void*c){return((fake_t*)c)->startup_error;}
static void request(void*c){++((fake_t*)c)->request_count;}
static bool wait_stopped(void*c,uint32_t t){fake_t*f=c;f->stop_timeout=t;return f->stop_wait;}
static esp_err_t runtime_error(void*c){return((fake_t*)c)->runtime_error;}
static esp_err_t cleanup_error(void*c){return((fake_t*)c)->cleanup_error;}
static esp_err_t retry_cleanup(void*c){fake_t*f=c;++f->cleanup_retry_count;return f->cleanup_error;}
static void release(void*c){++((fake_t*)c)->release_count;}

static vk_audio_runtime_t make(fake_t *fake)
{
    vk_audio_runtime_t runtime;
    vk_audio_runtime_ops_t ops={prepare,create,wait_started,startup,request,wait_stopped,runtime_error,cleanup_error,retry_cleanup,release,fake};
    vk_audio_runtime_init(&runtime,&ops);
    return runtime;
}

static void test_running_stop(void)
{
    fake_t fake={.start_wait=true,.stop_wait=true};
    vk_audio_runtime_t runtime=make(&fake);
    assert(vk_audio_runtime_start(&runtime)==ESP_OK);
    assert(vk_audio_runtime_is_active(&runtime));
    assert(vk_audio_runtime_start(&runtime)==ESP_ERR_INVALID_STATE);
    assert(vk_audio_runtime_stop(&runtime)==ESP_OK);
    assert(fake.request_count==1U&&fake.release_count==1U);
    assert(fake.start_timeout==VK_AUDIO_JOIN_TIMEOUT_MS&&fake.stop_timeout==VK_AUDIO_JOIN_TIMEOUT_MS);
    assert(vk_audio_runtime_stop(&runtime)==ESP_OK);
}

static void test_acquisition_failures(void)
{
    fake_t prepare_fail={.prepare_error=ESP_FAIL};
    vk_audio_runtime_t a=make(&prepare_fail);
    assert(vk_audio_runtime_start(&a)==ESP_FAIL);
    assert(prepare_fail.create_count==0U&&prepare_fail.release_count==0U);

    fake_t create_fail={.create_error=ESP_ERR_NO_MEM};
    vk_audio_runtime_t b=make(&create_fail);
    assert(vk_audio_runtime_start(&b)==ESP_ERR_NO_MEM);
    assert(create_fail.release_count==1U);

    fake_t startup_fail={.start_wait=true,.stop_wait=true,.startup_error=ESP_FAIL};
    vk_audio_runtime_t c=make(&startup_fail);
    assert(vk_audio_runtime_start(&c)==ESP_FAIL);
    assert(startup_fail.release_count==1U);
}

static void test_timeouts_retain_context(void)
{
    fake_t start_timeout={.start_wait=false,.stop_wait=false};
    vk_audio_runtime_t a=make(&start_timeout);
    assert(vk_audio_runtime_start(&a)==ESP_ERR_TIMEOUT);
    assert(vk_audio_runtime_is_tainted(&a));
    assert(start_timeout.request_count==1U&&start_timeout.release_count==0U);

    fake_t stop_timeout={.start_wait=true,.stop_wait=false};
    vk_audio_runtime_t b=make(&stop_timeout);
    assert(vk_audio_runtime_start(&b)==ESP_OK);
    assert(vk_audio_runtime_stop(&b)==ESP_ERR_TIMEOUT);
    assert(vk_audio_runtime_is_tainted(&b));
    assert(stop_timeout.release_count==0U);
    assert(vk_audio_runtime_stop(&b)==ESP_ERR_INVALID_STATE);
}

static void test_runtime_and_cleanup_errors(void)
{
    fake_t runtime_fail={.start_wait=true,.stop_wait=true,.runtime_error=ESP_FAIL};
    vk_audio_runtime_t a=make(&runtime_fail);
    assert(vk_audio_runtime_start(&a)==ESP_OK);
    assert(vk_audio_runtime_stop(&a)==ESP_FAIL);
    assert(runtime_fail.release_count==1U);

    fake_t cleanup_fail={.start_wait=true,.stop_wait=true,.cleanup_error=ESP_FAIL};
    vk_audio_runtime_t b=make(&cleanup_fail);
    assert(vk_audio_runtime_start(&b)==ESP_OK);
    assert(vk_audio_runtime_stop(&b)==ESP_FAIL);
    assert(vk_audio_runtime_is_tainted(&b));
    assert(cleanup_fail.release_count==0U);
    assert(vk_audio_runtime_collect(&b, 17U)==ESP_FAIL);
    assert(cleanup_fail.cleanup_retry_count==1U&&cleanup_fail.release_count==0U);
    cleanup_fail.cleanup_error=ESP_OK;
    assert(vk_audio_runtime_collect(&b, 17U)==ESP_OK);
    assert(cleanup_fail.cleanup_retry_count==1U&&cleanup_fail.release_count==1U);
    assert(vk_audio_runtime_collect(&b, 17U)==ESP_ERR_INVALID_STATE);
}

int main(void)
{
    test_running_stop();
    test_acquisition_failures();
    test_timeouts_retain_context();
    test_runtime_and_cleanup_errors();
    return 0;
}
