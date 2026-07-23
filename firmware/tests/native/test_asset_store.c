#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vk_asset_store.h"
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

#define MAX_FILES 96
#define MAX_DATA 8192
typedef struct { char name[96]; uint8_t data[MAX_DATA]; size_t size; bool used; } file_t;
typedef enum {
    IO_OK = 0,
    IO_FAIL_BEFORE,
    IO_FAIL_AFTER_PARTIAL,
    IO_FAIL_AFTER_DURABLE,
    IO_ZERO_PROGRESS,
    IO_EINTR_THEN_OK,
    IO_NAME_CONFLICT,
    IO_NO_SPACE,
} io_mode_t;
typedef struct {
    file_t files[MAX_FILES];
    bool mounted, erased;
    int format_calls;
    uint32_t free_bytes;
    int fail_write_after;
    int writes;
    io_mode_t next_append;
    io_mode_t next_new;
    io_mode_t next_rewrite;
    bool fail_truncate;
    unsigned partial_writes;
    unsigned interrupted_writes;
    unsigned sync_failures;
    unsigned close_failures;
} fs_t;
static file_t *findf(fs_t*f,const char*n){for(int i=0;i<MAX_FILES;i++)if(f->files[i].used&&!strcmp(f->files[i].name,n))return &f->files[i];return NULL;}
static file_t *newf(fs_t*f,const char*n){if(findf(f,n))return NULL;for(int i=0;i<MAX_FILES;i++)if(!f->files[i].used){f->files[i].used=true;strcpy(f->files[i].name,n);return &f->files[i];}return NULL;}
static esp_err_t mountf(void*c,bool format){fs_t*f=c;assert(!format);if(!f->mounted)return ESP_FAIL;return ESP_OK;}
static esp_err_t unmountf(void*c){(void)c;return ESP_OK;}
static esp_err_t formatf(void*c){fs_t*f=c;f->format_calls++;for(int i=0;i<MAX_FILES;i++)f->files[i].used=false;f->erased=false;return ESP_OK;}
static esp_err_t erasedf(void*c,uint32_t o,uint32_t z,uint8_t h[32],bool*a){fs_t*f=c;(void)o;(void)z;uint8_t ff=0xff;vk_asset_sha256(&ff,1,h);*a=f->erased;return ESP_OK;}
static esp_err_t sizef(void*c,const char*n,size_t*z){file_t*x=findf(c,n);if(!x)return ESP_ERR_NOT_FOUND;*z=x->size;return ESP_OK;}
static esp_err_t readf(void*c,const char*n,size_t o,uint8_t*b,size_t cap,size_t*got){file_t*x=findf(c,n);if(!x)return ESP_ERR_NOT_FOUND;if(o>x->size)return ESP_ERR_INVALID_STATE;*got=x->size-o<cap?x->size-o:cap;memcpy(b,x->data+o,*got);return ESP_OK;}
static esp_err_t failpoint(fs_t*f){f->writes++;return f->fail_write_after&&f->writes==f->fail_write_after?ESP_FAIL:ESP_OK;}
static esp_err_t apply_before(io_mode_t mode){return mode==IO_NAME_CONFLICT?ESP_ERR_INVALID_STATE:mode==IO_NO_SPACE?ESP_ERR_NO_MEM:mode==IO_FAIL_BEFORE||mode==IO_ZERO_PROGRESS?ESP_FAIL:ESP_OK;}
static esp_err_t newfile(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)sync;fs_t*f=c;if(failpoint(f)!=ESP_OK)return ESP_FAIL;io_mode_t mode=f->next_new;f->next_new=IO_OK;esp_err_t injected=apply_before(mode);if(injected!=ESP_OK)return injected;if(z>MAX_DATA)return ESP_ERR_NO_MEM;file_t*x=newf(f,n);if(!x)return ESP_ERR_INVALID_STATE;if(z)memcpy(x->data,b,z);x->size=z;if(mode==IO_FAIL_AFTER_DURABLE){f->close_failures++;return ESP_FAIL;}return ESP_OK;}
static esp_err_t rewrite(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)sync;fs_t*f=c;if(failpoint(f)!=ESP_OK)return ESP_FAIL;io_mode_t mode=f->next_rewrite;f->next_rewrite=IO_OK;esp_err_t injected=apply_before(mode);if(injected!=ESP_OK)return injected;file_t*x=findf(f,n);if(!x)x=newf(f,n);if(!x||z>MAX_DATA)return ESP_ERR_NO_MEM;if(z)memcpy(x->data,b,z);x->size=z;if(mode==IO_FAIL_AFTER_DURABLE){f->sync_failures++;return ESP_FAIL;}return ESP_OK;}
static esp_err_t append(void*c,const char*n,size_t o,const uint8_t*b,size_t z,bool sync){(void)sync;fs_t*f=c;if(failpoint(f)!=ESP_OK)return ESP_FAIL;io_mode_t mode=f->next_append;f->next_append=IO_OK;file_t*x=findf(f,n);if(!x||x->size!=o||o+z>MAX_DATA)return ESP_ERR_INVALID_STATE;if(mode==IO_EINTR_THEN_OK){f->interrupted_writes++;mode=IO_OK;}esp_err_t injected=apply_before(mode);if(injected!=ESP_OK)return injected;if(mode==IO_FAIL_AFTER_PARTIAL){size_t part=z>1?z/2:1;memcpy(x->data+o,b,part);x->size+=part;f->partial_writes++;return ESP_FAIL;}memcpy(x->data+o,b,z);x->size+=z;if(mode==IO_FAIL_AFTER_DURABLE){f->sync_failures++;return ESP_FAIL;}return ESP_OK;}
static esp_err_t truncatef(void*c,const char*n,size_t z,bool sync){(void)sync;fs_t*f=c;if(f->fail_truncate)return ESP_FAIL;file_t*x=findf(f,n);if(!x||z>x->size)return ESP_ERR_INVALID_STATE;x->size=z;return ESP_OK;}
static esp_err_t removef(void*c,const char*n){file_t*x=findf(c,n);if(!x)return ESP_ERR_NOT_FOUND;x->used=false;return ESP_OK;}
static esp_err_t listf(void*c,const char*prefix,char names[][96],size_t cap,size_t*count){fs_t*f=c;(void)prefix;*count=0;for(int i=0;i<MAX_FILES;i++)if(f->files[i].used){if(*count==cap)return ESP_ERR_NO_MEM;strcpy(names[(*count)++],f->files[i].name);}return ESP_OK;}
static esp_err_t freef(void*c,uint32_t*out){*out=((fs_t*)c)->free_bytes;return ESP_OK;}
static const vk_asset_fs_ops_t ops={mountf,unmountf,formatf,erasedf,readf,sizef,newfile,rewrite,append,truncatef,removef,listf,freef};
static esp_err_t validate_vka1(void*c,const char*n,const uint8_t h[32],uint32_t z,vk_asset_kind_t k){(void)c;(void)n;(void)h;(void)z;(void)k;return ESP_OK;}
static esp_err_t validate_revision(void*c,uint32_t r,uint32_t p,const uint8_t*a,size_t az,const uint8_t*s,size_t sz){(void)c;(void)p;char needle[32];snprintf(needle,sizeof(needle),"\"revision\":%u",r);return memmem(a,az,needle,strlen(needle))&&memmem(s,sz,needle,strlen(needle))?ESP_OK:ESP_FAIL;}
static vk_asset_store_t make(fs_t*f){vk_asset_store_t s;vk_asset_store_config_t c={.fs=&ops,.fs_context=f,.validate_vka1=validate_vka1,.validate_revision=validate_revision,.partition_offset=0xa20000,.partition_size=0x5e0000,.reserve_bytes=1,.max_asset_bytes=4096,.max_assets=64};assert(vk_asset_store_init(&s,&c)==ESP_OK);return s;}
static void test_mount_no_format(void){fs_t f={.mounted=false,.erased=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_FAIL);assert(f.format_calls==0);assert(vk_asset_store_state(&s)==VK_ASSET_STORE_MOUNT_FAILED);}
static void test_format_token(void){fs_t f={.mounted=false,.erased=true,.free_bytes=10000};vk_asset_store_t s=make(&f);vk_asset_format_token_t t;assert(vk_asset_store_authorize_format(&s,7,9,&t)==ESP_OK);assert(vk_asset_store_format(&s,8,&t,VK_ASSET_FORMAT_CONFIRMATION)!=ESP_OK);assert(f.format_calls==0);assert(vk_asset_store_format(&s,7,&t,VK_ASSET_FORMAT_CONFIRMATION)==ESP_OK);assert(f.format_calls==1);assert(vk_asset_store_format(&s,7,&t,VK_ASSET_FORMAT_CONFIRMATION)!=ESP_OK);}
static void test_transfer(void){fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);uint8_t payload[]={1,2,3};vk_asset_transfer_t t={.transfer_id=1,.total_bytes=3,.kind=VK_ASSET_KIND_IMAGE};vk_asset_sha256(payload,3,t.sha256);assert(vk_asset_store_begin(&s,&t)==ESP_OK);uint32_t next;assert(vk_asset_store_append(&s,1,1,payload,1,&next)!=ESP_OK);assert(vk_asset_store_append(&s,1,0,payload,2,&next)==ESP_OK&&next==2);vk_asset_store_t resumed=make(&f);assert(vk_asset_store_mount(&resumed)==ESP_OK);vk_asset_transfer_t restored;assert(vk_asset_store_resume(&resumed,1,&restored)==ESP_OK&&restored.next_offset==2);s=resumed;assert(vk_asset_store_append(&s,1,2,payload+2,1,&next)==ESP_OK&&next==3);assert(vk_asset_store_seal(&s,1)==ESP_OK);char hex[65];static const char digits[]="0123456789abcdef";for(int i=0;i<32;i++){hex[i*2]=digits[t.sha256[i]>>4];hex[i*2+1]=digits[t.sha256[i]&15];}hex[64]=0;char path[96];snprintf(path,sizeof(path),"/assets/%s.vka",hex);assert(findf(&f,path)&&findf(&f,path)->size==3);}
static void test_seal_replaces_stale_destination(void){
    uint8_t payload[]={1,2,3};fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);
    vk_asset_transfer_t transfer={.transfer_id=2,.total_bytes=sizeof(payload),.kind=VK_ASSET_KIND_IMAGE};vk_asset_sha256(payload,sizeof(payload),transfer.sha256);assert(vk_asset_store_begin(&s,&transfer)==ESP_OK);uint32_t next;
    assert(vk_asset_store_append(&s,2,0,payload,sizeof(payload),&next)==ESP_OK&&next==sizeof(payload));
    static const char digits[]="0123456789abcdef";char digest[65],path[96];for(size_t i=0;i<32;i++){digest[i*2]=digits[transfer.sha256[i]>>4];digest[i*2+1]=digits[transfer.sha256[i]&15];}digest[64]=0;snprintf(path,sizeof(path),"/assets/%s.vka",digest);
    file_t*stale=newf(&f,path);assert(stale);stale->data[0]=9;stale->size=1;
    assert(vk_asset_store_seal(&s,2)==ESP_OK);
    file_t*stored=findf(&f,path);assert(stored&&stored->size==sizeof(payload)&&memcmp(stored->data,payload,sizeof(payload))==0);
}
static void start_transfer(fs_t*f,vk_asset_store_t*s,uint32_t id,const uint8_t*payload,size_t size){*f=(fs_t){.mounted=true,.free_bytes=10000};*s=make(f);assert(vk_asset_store_mount(s)==ESP_OK);vk_asset_transfer_t t={.transfer_id=id,.total_bytes=(uint32_t)size,.kind=VK_ASSET_KIND_IMAGE};vk_asset_sha256(payload,size,t.sha256);assert(vk_asset_store_begin(s,&t)==ESP_OK);}
static void assert_reboot_offset(fs_t*f,uint32_t id,uint32_t expected){vk_asset_store_t reboot=make(f);assert(vk_asset_store_mount(&reboot)==ESP_OK);vk_asset_transfer_t restored;assert(vk_asset_store_resume(&reboot,id,&restored)==ESP_OK);assert(restored.next_offset==expected);}
static void test_append_failure_matrix(void){
    static const io_mode_t append_modes[]={IO_FAIL_BEFORE,IO_FAIL_AFTER_PARTIAL,IO_FAIL_AFTER_DURABLE,IO_ZERO_PROGRESS};
    uint8_t payload[]={1,2,3};
    for(size_t i=0;i<sizeof(append_modes)/sizeof(append_modes[0]);i++){fs_t f;vk_asset_store_t s;start_transfer(&f,&s,7,payload,sizeof(payload));f.next_append=append_modes[i];uint32_t next=99;assert(vk_asset_store_append(&s,7,0,payload,2,&next)!=ESP_OK);assert(s.transfer_active&&s.transfer.next_offset==0);file_t*part=findf(&f,"/tmp/00000007.part");assert(part&&part->size==0);assert_reboot_offset(&f,7,0);}
    {fs_t f;vk_asset_store_t s;start_transfer(&f,&s,8,payload,sizeof(payload));f.next_append=IO_EINTR_THEN_OK;uint32_t next=0;assert(vk_asset_store_append(&s,8,0,payload,2,&next)==ESP_OK&&next==2);assert(f.interrupted_writes==1);assert_reboot_offset(&f,8,2);}
    {fs_t f;vk_asset_store_t s;start_transfer(&f,&s,9,payload,sizeof(payload));f.next_rewrite=IO_FAIL_AFTER_DURABLE;uint32_t next=99;assert(vk_asset_store_append(&s,9,0,payload,2,&next)==ESP_FAIL);assert(s.transfer_active&&s.transfer.next_offset==0);assert(findf(&f,"/tmp/00000009.part")->size==0);/* Durable new metadata is rejected because its part length no longer agrees. */vk_asset_store_t reboot=make(&f);assert(vk_asset_store_mount(&reboot)==ESP_OK);vk_asset_transfer_t restored;assert(vk_asset_store_resume(&reboot,9,&restored)==ESP_ERR_NOT_FOUND);}
    {fs_t f;vk_asset_store_t s;start_transfer(&f,&s,10,payload,sizeof(payload));f.next_append=IO_FAIL_AFTER_PARTIAL;f.fail_truncate=true;uint32_t next=99;assert(vk_asset_store_append(&s,10,0,payload,2,&next)==ESP_FAIL);assert(!s.transfer_active&&vk_asset_store_state(&s)==VK_ASSET_STORE_CORRUPT);}
}
static void test_commit_recovery(void){fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);const char*a1="{\"revision\":1}",*sc1="{\"revision\":1}";vk_asset_revision_t r1={1,0,(const uint8_t*)a1,strlen(a1),(const uint8_t*)sc1,strlen(sc1)};uint8_t x[32],y[32];assert(vk_asset_store_publish_revision(&s,&r1,x,y)==ESP_OK);const char*a2="{\"revision\":2}",*sc2="{\"revision\":2}";vk_asset_revision_t r2={2,1,(const uint8_t*)a2,strlen(a2),(const uint8_t*)sc2,strlen(sc2)};assert(vk_asset_store_publish_revision(&s,&r2,x,y)==ESP_OK);vk_asset_store_t reboot=make(&f);assert(vk_asset_store_mount(&reboot)==ESP_OK);vk_asset_recovery_t rec;assert(vk_asset_store_recover(&reboot,&rec)==ESP_OK);assert(rec.current_revision==2&&rec.previous_revision==1);file_t*c2=findf(&f,"/config/commit-r00000002.vkc");c2->data[0]='X';reboot=make(&f);assert(vk_asset_store_mount(&reboot)==ESP_OK);assert(vk_asset_store_recover(&reboot,&rec)==ESP_OK);assert(rec.current_revision==1);}
static void test_load_validated_revision(void){fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);const char*a="{\"revision\":1}",*sc="{\"revision\":1}";vk_asset_revision_t r={1,0,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};uint8_t x[32],y[32];assert(vk_asset_store_publish_revision(&s,&r,x,y)==ESP_OK);uint8_t ab[64],sb[64];size_t az=0,sz=0;uint32_t previous=99;assert(vk_asset_store_load_revision(&s,1,&previous,ab,sizeof(ab),&az,sb,sizeof(sb),&sz)==ESP_OK);assert(previous==0&&az==strlen(a)&&sz==strlen(sc)&&memcmp(ab,a,az)==0&&memcmp(sb,sc,sz)==0);assert(vk_asset_store_load_revision(&s,2,&previous,ab,sizeof(ab),&az,sb,sizeof(sb),&sz)==ESP_ERR_NOT_FOUND);}
static void publish_simple(vk_asset_store_t*s,uint32_t revision,uint32_t previous){char a[64],sc[64];snprintf(a,sizeof(a),"{\"revision\":%u}",revision);snprintf(sc,sizeof(sc),"{\"revision\":%u}",revision);vk_asset_revision_t r={revision,previous,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};uint8_t x[32],y[32];assert(vk_asset_store_publish_revision(s,&r,x,y)==ESP_OK);}
static void test_publish_failure_preserves(void){
    for(int fail=1;fail<=3;fail++){
        fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);publish_simple(&s,1,0);f.writes=0;f.fail_write_after=fail;
        const char*a="{\"revision\":2}",*sc="{\"revision\":2}";vk_asset_revision_t r={2,1,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};uint8_t x[32],y[32];assert(vk_asset_store_publish_revision(&s,&r,x,y)!=ESP_OK);assert(s.selected_revision==1);
        vk_asset_store_t reboot=make(&f);assert(vk_asset_store_mount(&reboot)==ESP_OK);vk_asset_recovery_t recovery;assert(vk_asset_store_recover(&reboot,&recovery)==ESP_OK);assert(recovery.current_revision==1);
        f.fail_write_after=0;f.writes=0;assert(vk_asset_store_publish_revision(&s,&r,x,y)==ESP_OK);assert(s.selected_revision==2);
    }
    {fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);publish_simple(&s,1,0);file_t*conflict=newf(&f,"/config/commit-r00000002.vkc");assert(conflict);conflict->size=1;const char*a="{\"revision\":2}",*sc="{\"revision\":2}";vk_asset_revision_t r={2,1,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};uint8_t x[32],y[32];assert(vk_asset_store_publish_revision(&s,&r,x,y)==ESP_OK);assert(s.selected_revision==2);}
}
static void test_no_space_and_destination_failures(void){
    uint8_t payload[]={1,2,3};fs_t f={.mounted=true,.free_bytes=3};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);vk_asset_transfer_t t={.transfer_id=12,.total_bytes=3,.kind=VK_ASSET_KIND_IMAGE};vk_asset_sha256(payload,3,t.sha256);assert(vk_asset_store_begin(&s,&t)==ESP_ERR_NO_MEM);assert(!s.transfer_active&&!findf(&f,"/tmp/0000000c.part"));
    f.free_bytes=10000;f.next_new=IO_NAME_CONFLICT;assert(vk_asset_store_begin(&s,&t)==ESP_ERR_INVALID_STATE);assert(!s.transfer_active);
}
static void test_gc_keeps_retained_and_active(void){fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);const char*a="{\"revision\":1}",*sc="{\"revision\":1}";uint8_t x[32],y[32];vk_asset_revision_t r1={1,0,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};assert(vk_asset_store_publish_revision(&s,&r1,x,y)==ESP_OK);a="{\"revision\":2}";sc="{\"revision\":2}";vk_asset_revision_t r2={2,1,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};assert(vk_asset_store_publish_revision(&s,&r2,x,y)==ESP_OK);a="{\"revision\":3}";sc="{\"revision\":3}";vk_asset_revision_t r3={3,2,(const uint8_t*)a,strlen(a),(const uint8_t*)sc,strlen(sc)};assert(vk_asset_store_publish_revision(&s,&r3,x,y)==ESP_OK);vk_asset_transfer_t t={.transfer_id=9,.total_bytes=1,.kind=VK_ASSET_KIND_IMAGE};uint8_t byte=7;vk_asset_sha256(&byte,1,t.sha256);assert(vk_asset_store_begin(&s,&t)==ESP_OK);assert(vk_asset_store_collect(&s)==ESP_OK);assert(!findf(&f,"/config/commit-r00000001.vkc"));assert(findf(&f,"/config/commit-r00000002.vkc"));assert(findf(&f,"/config/commit-r00000003.vkc"));assert(findf(&f,"/tmp/00000009.part"));assert(findf(&f,"/tmp/00000009.meta"));}
static void hash_hex(const uint8_t*d,char out[65]){static const char digits[]="0123456789abcdef";for(size_t i=0;i<32;i++){out[i*2]=digits[d[i]>>4];out[i*2+1]=digits[d[i]&15];}out[64]=0;}
static file_t*add_asset(fs_t*f,uint8_t value,uint8_t digest[32]){vk_asset_sha256(&value,1,digest);char h[65],path[96];hash_hex(digest,h);snprintf(path,sizeof(path),"/assets/%s.vka",h);file_t*x=newf(f,path);assert(x);x->data[0]=value;x->size=1;return x;}
static void test_gc_parses_canonical_assets_only(void){
    fs_t f={.mounted=true,.free_bytes=10000};vk_asset_store_t s=make(&f);assert(vk_asset_store_mount(&s)==ESP_OK);
    uint8_t kept[32],orphan[32];file_t*kept_file=add_asset(&f,11,kept);file_t*orphan_file=add_asset(&f,12,orphan);char kh[65],oh[65];hash_hex(kept,kh);hash_hex(orphan,oh);
    char assets[256],screen[256];snprintf(assets,sizeof(assets),"{\"assets\":[{\"bytes\":1,\"kind\":\"image\",\"sha256\":\"%s\"}],\"previous_revision\":0,\"revision\":1,\"schema\":1}",kh);snprintf(screen,sizeof(screen),"{\"diagnostic\":\"%s\",\"revision\":1}",oh);
    vk_asset_revision_t r={1,0,(const uint8_t*)assets,strlen(assets),(const uint8_t*)screen,strlen(screen)};uint8_t x[32],y[32];assert(vk_asset_store_publish_revision(&s,&r,x,y)==ESP_OK);assert(vk_asset_store_collect(&s)==ESP_OK);assert(kept_file->used);assert(!orphan_file->used);
}
int main(void){assert(strlen("/assets/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.vka")+1==77);assert(VK_ASSET_SPIFFS_NAME_MIN_BYTES>=78);assert(vk_asset_revision_is_newer(0,UINT32_MAX));assert(!vk_asset_revision_is_newer(0x80000000U,0));test_mount_no_format();test_format_token();test_transfer();test_seal_replaces_stale_destination();test_append_failure_matrix();test_commit_recovery();test_load_validated_revision();test_publish_failure_preserves();test_no_space_and_destination_failures();test_gc_keeps_retained_and_active();test_gc_parses_canonical_assets_only();puts("asset store tests passed (11 groups, 25 deterministic failure/recovery cells)");}
