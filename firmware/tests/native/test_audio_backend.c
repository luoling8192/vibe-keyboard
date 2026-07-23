#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_audio_backend.h"

typedef enum {
    READ_OK,
    READ_TIMEOUT,
    READ_ERROR,
    READ_OVERSIZE,
    READ_ODD,
} read_mode_t;

typedef struct {
    int fail_acquire_step;
    int acquire_step;
    int cleanup[16];
    int cleanups;
    size_t reads[32];
    size_t read_count;
    size_t read_index;
    read_mode_t read_mode;
    int fetch_mode;
    int fetch_repeats;
    int encode;
    esp_err_t send_error;
    int sends;
    bool feed_fail;
    bool disable_fail;
    bool destroy_fail;
    size_t feed_bytes;
    size_t fetch_bytes;
} fake_t;

static esp_err_t next(fake_t *fake, void **handle)
{
    if (++fake->acquire_step == fake->fail_acquire_step) return ESP_FAIL;
    *handle = fake;
    return ESP_OK;
}

static esp_err_t i2s_create(void *context, void **handle) { return next(context, handle); }
static esp_err_t i2s_initialize(void *context, void *handle) { (void)handle; fake_t *f=context; return ++f->acquire_step==f->fail_acquire_step?ESP_FAIL:ESP_OK; }
static esp_err_t i2s_enable(void *context, void *handle) { return i2s_initialize(context, handle); }
static esp_err_t i2s_read(void *context, void *handle, uint8_t *bytes, size_t capacity, size_t *count)
{
    (void)handle;
    fake_t *f = context;
    if (f->read_mode == READ_TIMEOUT) return ESP_ERR_TIMEOUT;
    if (f->read_mode == READ_ERROR) return ESP_FAIL;
    if (f->read_mode == READ_OVERSIZE) { *count = capacity + 2U; return ESP_OK; }
    if (f->read_mode == READ_ODD) { *count = 3U; return ESP_OK; }
    size_t length = f->read_index < f->read_count ? f->reads[f->read_index++] : 0U;
    assert(length <= capacity);
    memset(bytes, 0, length);
    *count = length;
    return ESP_OK;
}
static esp_err_t i2s_disable(void *context, void *handle)
{
    (void)handle;
    fake_t *f=context;
    f->cleanup[f->cleanups++]=1;
    return f->disable_fail ? ESP_FAIL : ESP_OK;
}
static esp_err_t i2s_destroy(void *context, void *handle)
{
    (void)handle;
    fake_t *f=context;
    f->cleanup[f->cleanups++]=4;
    return f->destroy_fail ? ESP_FAIL : ESP_OK;
}
static esp_err_t afe_create(void *context, void **handle, size_t *feed, size_t *fetch)
{
    fake_t *f=context;
    if (++f->acquire_step == f->fail_acquire_step) return ESP_FAIL;
    *handle=f;
    *feed=f->feed_bytes ? f->feed_bytes : 16U;
    *fetch=f->fetch_bytes ? f->fetch_bytes : 1920U;
    return ESP_OK;
}
static int afe_feed(void *context, void *handle, const int16_t *samples)
{
    (void)handle; (void)samples;
    return ((fake_t *)context)->feed_fail ? -1 : 0;
}
static bool afe_fetch(void *context, void *handle, vk_audio_backend_fetch_t *result)
{
    (void)handle;
    fake_t *f=context;
    static int16_t pcm[2048];
    if (f->fetch_repeats > 0) --f->fetch_repeats;
    else if (f->fetch_mode == 0) return false;
    result->result = f->fetch_mode == 1 ? 0 : -1;
    result->data = f->fetch_mode == 3 ? NULL : pcm;
    result->data_bytes = f->fetch_mode == 4 ? 3 :
                         f->fetch_mode == 5 ? (int)f->fetch_bytes - 2 :
                         (int)(f->fetch_bytes ? f->fetch_bytes : 1920U);
    if (f->fetch_repeats == 0) f->fetch_mode=0;
    return true;
}
static void afe_destroy(void *context, void *handle) { (void)handle; fake_t*f=context; f->cleanup[f->cleanups++]=3; }
static esp_err_t opus_create(void *context, void **handle) { return next(context, handle); }
static int opus_encode(void *context, void *handle, const int16_t *samples, size_t count, uint8_t *packet, size_t capacity)
{ (void)handle;(void)samples;(void)count;(void)packet;(void)capacity; return ((fake_t*)context)->encode; }
static void opus_destroy(void *context, void *handle) { (void)handle; fake_t*f=context; f->cleanup[f->cleanups++]=2; }
static void *allocate(void *context, size_t bytes, bool external)
{ (void)external; fake_t*f=context; if(++f->acquire_step==f->fail_acquire_step)return NULL; return malloc(bytes); }
static void deallocate(void *context, void *pointer) { fake_t*f=context; f->cleanup[f->cleanups++]=5; free(pointer); }
static esp_err_t send_frame(void *context, uint32_t session, uint32_t sequence, uint8_t flags, const uint8_t *payload, uint16_t length)
{ (void)session;(void)sequence;(void)flags;(void)payload;(void)length; fake_t*f=context; ++f->sends; return f->send_error; }

static vk_audio_backend_ops_t ops(fake_t *fake)
{
    return (vk_audio_backend_ops_t){i2s_create,i2s_initialize,i2s_enable,i2s_read,
        i2s_disable,i2s_destroy,afe_create,afe_feed,afe_fetch,afe_destroy,
        opus_create,opus_encode,opus_destroy,allocate,deallocate,send_frame,fake};
}

static void acquire_ok(fake_t *fake, vk_audio_backend_t *backend)
{
    fake->encode=10;
    vk_audio_backend_ops_t operations=ops(fake);
    assert(vk_audio_backend_acquire(backend,&operations,1)==ESP_OK);
}

int main(void)
{
    for(int fail=1;fail<=7;fail++){
        fake_t f={.fail_acquire_step=fail,.encode=10};
        vk_audio_backend_t b;
        vk_audio_backend_ops_t o=ops(&f);
        assert(vk_audio_backend_acquire(&b,&o,1)!=ESP_OK);
        assert(vk_audio_backend_release(&b)==ESP_OK);
    }

    fake_t f={0}; vk_audio_backend_t b;
    acquire_ok(&f,&b);
    f.read_mode=READ_TIMEOUT; f.fetch_mode=1; f.fetch_repeats=2;
    assert(vk_audio_backend_capture(&b)==ESP_OK);
    assert(f.sends==2);
    assert(vk_audio_backend_finish(&b)==ESP_OK&&f.sends==3);
    assert(vk_audio_backend_release(&b)==ESP_OK);
    assert(f.cleanups==6);
    assert(f.cleanup[0]==2&&f.cleanup[1]==1&&f.cleanup[2]==5&&f.cleanup[3]==3&&f.cleanup[4]==4&&f.cleanup[5]==5);

    for(int mode=2;mode<=5;mode++){
        f=(fake_t){.read_mode=READ_TIMEOUT,.fetch_mode=mode,.encode=10};
        acquire_ok(&f,&b);
        assert(vk_audio_backend_capture(&b)!=ESP_OK);
        assert(vk_audio_backend_release(&b)==ESP_OK);
    }
    for(read_mode_t mode=READ_ERROR;mode<=READ_ODD;mode++){
        f=(fake_t){.read_mode=mode}; acquire_ok(&f,&b);
        assert(vk_audio_backend_capture(&b)!=ESP_OK);
        assert(vk_audio_backend_release(&b)==ESP_OK);
    }
    f=(fake_t){.read_mode=READ_TIMEOUT,.fetch_mode=1,.fetch_repeats=1}; acquire_ok(&f,&b);
    assert(vk_audio_backend_capture(&b)==ESP_OK);
    assert(vk_audio_backend_release(&b)==ESP_OK);

    f=(fake_t){.feed_fail=true,.reads={16},.read_count=1}; acquire_ok(&f,&b);
    assert(vk_audio_backend_capture(&b)!=ESP_OK);
    assert(vk_audio_backend_release(&b)==ESP_OK);

    /* Partial accumulation, wrap, and a full-ring rejection use the production ring. */
    f=(fake_t){.feed_bytes=64000,.reads={4000,4000},.read_count=2}; acquire_ok(&f,&b);
    assert(vk_audio_backend_capture(&b)==ESP_OK); assert(b.ring_count==4000);
    b.ring_head=VK_AUDIO_BACKEND_RING_BYTES-2000; b.ring_count=4000;
    assert(vk_audio_backend_capture(&b)==ESP_OK); assert(b.ring_count==8000);
    b.ring_count=VK_AUDIO_BACKEND_RING_BYTES-1000; f.reads[2]=2000; f.read_count=3;
    assert(vk_audio_backend_capture(&b)==ESP_ERR_NO_MEM);
    assert(vk_audio_backend_release(&b)==ESP_OK);

    const esp_err_t send_errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL};
    for(size_t index=0;index<sizeof(send_errors)/sizeof(send_errors[0]);++index){
        esp_err_t send_error=send_errors[index];
        f=(fake_t){.read_mode=READ_TIMEOUT,.fetch_mode=1,.fetch_repeats=1,.send_error=send_error}; acquire_ok(&f,&b);
        assert(vk_audio_backend_capture(&b)==send_error); assert(f.sends==1);
        assert(vk_audio_backend_release(&b)==ESP_OK);
    }

    f=(fake_t){.disable_fail=true}; acquire_ok(&f,&b);
    assert(vk_audio_backend_release(&b)==ESP_FAIL);
    assert(b.i2s_enabled&&b.feed!=NULL&&b.afe!=NULL&&b.i2s!=NULL&&b.ring!=NULL);
    f.disable_fail=false; assert(vk_audio_backend_release(&b)==ESP_OK);

    f=(fake_t){.destroy_fail=true}; acquire_ok(&f,&b);
    assert(vk_audio_backend_release(&b)==ESP_FAIL);
    assert(!b.i2s_enabled&&b.feed==NULL&&b.afe==NULL&&b.i2s!=NULL&&b.ring!=NULL);
    f.destroy_fail=false; assert(vk_audio_backend_release(&b)==ESP_OK);

    puts("vk_audio backend native tests passed");
}
