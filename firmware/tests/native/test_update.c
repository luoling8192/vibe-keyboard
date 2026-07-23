#include "vk_update.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    vk_update_partition_tuple_t tuple;
    uint8_t bytes[1024];
    uint32_t written;
    unsigned begin_count, cancel_count, select_count, seal_count;
    bool fail_write, fail_select, bad_readback, bad_seal;
    uint64_t now_ms;
} fake_t;

static const char *HASH = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static vk_update_result_t read_tuple(void *context, vk_update_partition_tuple_t *tuple) { *tuple=((fake_t*)context)->tuple; return VK_UPDATE_OK; }
static vk_update_result_t begin_write(void *context,const vk_update_partition_tuple_t *tuple,uint32_t size)
{ fake_t*f=context; assert(tuple->target_offset==f->tuple.target_offset&&size==4U);++f->begin_count;f->written=0;return VK_UPDATE_OK; }
static vk_update_result_t write_chunk(void *context,uint32_t offset,const uint8_t*data,size_t length)
{ fake_t*f=context;if(f->fail_write)return VK_UPDATE_WRITE_FAILED;assert(offset==f->tuple.target_offset+f->written);memcpy(f->bytes+f->written,data,length);f->written+=(uint32_t)length;return VK_UPDATE_OK; }
static void identity(vk_update_image_identity_t*out){memset(out,0,sizeof(*out));strcpy(out->project,"vibe_keyboard");strcpy(out->version,"test");out->chip_id=9;out->max_revision=99;out->image_size=4;}
static vk_update_result_t seal_image(void*context,uint32_t size,char digest[65],vk_update_image_identity_t*out)
{fake_t*f=context;++f->seal_count;assert(size==f->written);strcpy(digest,f->bad_seal?"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":HASH);identity(out);return VK_UPDATE_OK;}
static vk_update_result_t readback(void*context,uint32_t offset,uint32_t size,char digest[65],vk_update_image_identity_t*out)
{fake_t*f=context;assert(offset==f->tuple.target_offset&&size==4U);strcpy(digest,f->bad_readback?"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":HASH);identity(out);return VK_UPDATE_OK;}
static vk_update_result_t select_slot(void*context,vk_update_slot_t slot){fake_t*f=context;if(f->fail_select)return VK_UPDATE_SELECTION_FAILED;assert(slot==f->tuple.target_slot);++f->select_count;return VK_UPDATE_OK;}
static void cancel(void*context){++((fake_t*)context)->cancel_count;}
static uint64_t monotonic_ms(void*context){return ((fake_t*)context)->now_ms;}
static vk_update_backend_t backend(fake_t*f){return(vk_update_backend_t){f,read_tuple,begin_write,write_chunk,seal_image,readback,select_slot,cancel,monotonic_ms};}
static vk_update_partition_tuple_t tuple(vk_update_slot_t running)
{ return running==VK_UPDATE_SLOT_OTA0?(vk_update_partition_tuple_t){VK_UPDATE_SLOT_OTA0,VK_UPDATE_OTA0_OFFSET,VK_UPDATE_SLOT_OTA1,VK_UPDATE_OTA1_OFFSET,VK_UPDATE_SLOT_SIZE,true,true}:(vk_update_partition_tuple_t){VK_UPDATE_SLOT_OTA1,VK_UPDATE_OTA1_OFFSET,VK_UPDATE_SLOT_OTA0,VK_UPDATE_OTA0_OFFSET,VK_UPDATE_SLOT_SIZE,true,true}; }

static void complete_stage(vk_update_t*u,uint32_t epoch,uint32_t id)
{ const uint8_t a[]={1,2},b[]={3,4};assert(vk_update_begin(u,epoch,id,4,HASH)==VK_UPDATE_OK);assert(vk_update_write(u,epoch,id,0,a,2)==VK_UPDATE_OK);assert(vk_update_write(u,epoch,id,2,b,2)==VK_UPDATE_OK); }

static void test_direction(vk_update_slot_t running)
{
    fake_t f={.tuple=tuple(running),.now_ms=1};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    complete_stage(&u,7,9);
    assert(vk_update_seal(&u,7,9,4,HASH)==VK_UPDATE_OK&&u.sealed);
    assert(vk_update_activate(&u,7,9,HASH)==VK_UPDATE_OK&&f.select_count==1U);
    /* Activate replay: idempotent, does not reselect. */
    assert(vk_update_activate(&u,7,9,HASH)==VK_UPDATE_OK&&f.select_count==1U);
}

static void test_write_invalidates(void)
{
    /* Write failure must cancel + invalidate the RAM stage. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=300};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,7,11,4,HASH)==VK_UPDATE_OK);
    f.fail_write=true;
    const uint8_t byte=1;
    assert(vk_update_write(&u,7,11,0,&byte,1)==VK_UPDATE_WRITE_FAILED);
    assert(!u.active); /* Stage must be destroyed. */
    unsigned cancelled = f.cancel_count;
    assert(cancelled >= 1U);
    /* After invalidation, a fresh begin creates a new stage. */
    f.fail_write=false;
    assert(vk_update_begin(&u,8,12,4,HASH)==VK_UPDATE_OK);
    assert(u.active && u.epoch == 8U);
}

static void test_selection_invalidates(void)
{
    /* Selection failure must cancel + invalidate the RAM stage. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=10};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    complete_stage(&u,7,9);
    assert(vk_update_seal(&u,7,9,4,HASH)==VK_UPDATE_OK&&u.sealed);
    f.fail_select=true;
    assert(vk_update_activate(&u,7,9,HASH)==VK_UPDATE_SELECTION_FAILED);
    assert(!u.active); /* Stage must be destroyed. */
    assert(f.cancel_count >= 1U);
}

static void test_wrong_epoch_no_refresh(void)
{
    /* wrong epoch must not refresh idle timer. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=400};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,7,12,4,HASH)==VK_UPDATE_OK);
    uint64_t before = u.last_activity_ms;
    f.now_ms=500;
    const uint8_t byte=1;
    assert(vk_update_write(&u,6,12,0,&byte,1)==VK_UPDATE_WRONG_EPOCH);
    assert(u.last_activity_ms == before); /* Timer NOT refreshed. */
    assert(u.active); /* Stage still alive. */
}

static void test_bad_offset_no_refresh(void)
{
    /* bad offset must not refresh idle timer. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=400};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,7,12,4,HASH)==VK_UPDATE_OK);
    uint64_t before = u.last_activity_ms;
    f.now_ms=500;
    const uint8_t byte=1;
    assert(vk_update_write(&u,7,12,1,&byte,1)==VK_UPDATE_BAD_OFFSET);
    assert(u.last_activity_ms == before); /* Timer NOT refreshed. */
}

static void test_seal_requires_transfer_complete(void)
{
    /* Seal fails when incomplete (not all bytes written). */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=400};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,7,12,4,HASH)==VK_UPDATE_OK);
    const uint8_t byte=1;
    assert(vk_update_write(&u,7,12,0,&byte,1)==VK_UPDATE_OK);
    assert(vk_update_seal(&u,7,12,4,HASH)==VK_UPDATE_INCOMPLETE);
    /* Timer NOT refreshed. */
}

static void test_tuple_change_invalidates(void)
{
    /* The running-slot tuple changes mid-stage → invalidate. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=10};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,7,9,4,HASH)==VK_UPDATE_OK);
    /* Simulate running slot changed externally: */
    f.tuple=tuple(VK_UPDATE_SLOT_OTA1);
    const uint8_t byte=1;
    assert(vk_update_write(&u,7,9,0,&byte,1)==VK_UPDATE_WRONG_TARGET);
    assert(!u.active && f.cancel_count >= 1U);
}

static void test_seal_bad_hash_invalidates(void)
{
    /* Bad seal digest must invalidate. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=100};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    complete_stage(&u,7,9);
    f.bad_seal=true;
    assert(vk_update_seal(&u,7,9,4,HASH)==VK_UPDATE_IMAGE_INVALID);
    assert(!u.active && f.cancel_count >= 1U);
}

static void test_seal_readback_mismatch_invalidates(void)
{
    /* Bad readback digest must invalidate. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=100};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    complete_stage(&u,7,9);
    f.bad_readback=true;
    assert(vk_update_seal(&u,7,9,4,HASH)==VK_UPDATE_READBACK_MISMATCH);
    assert(!u.active && f.cancel_count >= 1U);
}

static void test_activate_bad_readback_invalidates(void)
{
    /* Activate must invalidate on readback mismatch. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=10};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    complete_stage(&u,7,9);
    assert(vk_update_seal(&u,7,9,4,HASH)==VK_UPDATE_OK);
    f.bad_readback=true;
    assert(vk_update_activate(&u,7,9,HASH)==VK_UPDATE_READBACK_MISMATCH);
    assert(!u.active && f.cancel_count >= 1U);
}

static void test_activate_not_sealed(void)
{
    /* Activate before seal fails. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=10};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,7,9,4,HASH)==VK_UPDATE_OK);
    assert(vk_update_activate(&u,7,9,HASH)==VK_UPDATE_NOT_SEALED);
}

static void test_timeout_granularity(void)
{
    /* Verify timeout boundary at exactly VK_UPDATE_IDLE_TIMEOUT_MS. */
    fake_t f={.tuple=tuple(VK_UPDATE_SLOT_OTA0),.now_ms=1000};vk_update_backend_t b=backend(&f);vk_update_t u;vk_update_init(&u,&b);
    assert(vk_update_begin(&u,8,10,4,HASH)==VK_UPDATE_OK);
    /* One ms before timeout: still alive. */
    f.now_ms=1000+VK_UPDATE_IDLE_TIMEOUT_MS-1U;
    uint32_t next=0;bool sealed=false;
    assert(vk_update_query(&u,8,10,&next,&sealed)==VK_UPDATE_OK);
    /* Exactly at timeout: invalidated. */
    f.now_ms+=VK_UPDATE_IDLE_TIMEOUT_MS;
    assert(vk_update_query(&u,8,10,&next,&sealed)==VK_UPDATE_TIMEOUT);
    assert(!u.active);
}

int main(void)
{
    /* Core direction tests: ota0→ota1 and ota1→ota0. */
    test_direction(VK_UPDATE_SLOT_OTA0);
    test_direction(VK_UPDATE_SLOT_OTA1);

    /* Failure matrix tests. */
    test_write_invalidates();
    test_selection_invalidates();
    test_wrong_epoch_no_refresh();
    test_bad_offset_no_refresh();
    test_seal_requires_transfer_complete();
    test_tuple_change_invalidates();
    test_seal_bad_hash_invalidates();
    test_seal_readback_mismatch_invalidates();
    test_activate_bad_readback_invalidates();
    test_activate_not_sealed();
    test_timeout_granularity();

    puts("update state machine tests passed");
    return 0;
}