#include "vk_asset_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define SHA_BLOCK 64U
#define COPY_CHUNK 8192U
#define META_MAX 256U
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif
#ifndef ESP_ERR_INVALID_CRC
#define ESP_ERR_INVALID_CRC 0x109
#endif
#ifndef ESP_ERR_NOT_ALLOWED
#define ESP_ERR_NOT_ALLOWED 0x10b
#endif

typedef struct { uint32_t h[8]; uint64_t bits; uint8_t block[SHA_BLOCK]; size_t used; } sha_ctx_t;

static void *large_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

static void yield_storage_owner(void)
{
#ifdef ESP_PLATFORM
    vTaskDelay(1U);
#endif
}

static const uint32_t k256[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static uint32_t rr(uint32_t x,unsigned n){return (x>>n)|(x<<(32U-n));}
static void sha_transform(sha_ctx_t*c,const uint8_t*b){uint32_t w[64];for(size_t i=0;i<16;i++)w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];for(size_t i=16;i<64;i++){uint32_t s0=rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3),s1=rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}uint32_t a=c->h[0],d=c->h[3],b0=c->h[1],cc=c->h[2],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];for(size_t i=0;i<64;i++){uint32_t s1=rr(e,6)^rr(e,11)^rr(e,25),ch=(e&f)^((~e)&g),t1=h+s1+ch+k256[i]+w[i],s0=rr(a,2)^rr(a,13)^rr(a,22),maj=(a&b0)^(a&cc)^(b0&cc),t2=s0+maj;h=g;g=f;f=e;e=d+t1;d=cc;cc=b0;b0=a;a=t1+t2;}c->h[0]+=a;c->h[1]+=b0;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;}
static void sha_init(sha_ctx_t*c){static const uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memset(c,0,sizeof(*c));memcpy(c->h,h,sizeof(h));}
static void sha_update(sha_ctx_t*c,const uint8_t*p,size_t n){while(n){size_t take=SHA_BLOCK-c->used;if(take>n)take=n;memcpy(c->block+c->used,p,take);c->used+=take;p+=take;n-=take;c->bits+=(uint64_t)take*8U;if(c->used==SHA_BLOCK){sha_transform(c,c->block);c->used=0;}}}
static void sha_final(sha_ctx_t*c,uint8_t out[32]){uint64_t bits=c->bits;c->block[c->used++]=0x80;if(c->used>56){while(c->used<64)c->block[c->used++]=0;sha_transform(c,c->block);c->used=0;}while(c->used<56)c->block[c->used++]=0;for(int i=7;i>=0;i--)c->block[c->used++]=(uint8_t)(bits>>(i*8));sha_transform(c,c->block);for(size_t i=0;i<8;i++){out[i*4]=(uint8_t)(c->h[i]>>24);out[i*4+1]=(uint8_t)(c->h[i]>>16);out[i*4+2]=(uint8_t)(c->h[i]>>8);out[i*4+3]=(uint8_t)c->h[i];}}
void vk_asset_sha256(const uint8_t*b,size_t n,uint8_t d[32]){sha_ctx_t c;sha_init(&c);if(n)sha_update(&c,b,n);sha_final(&c,d);}

static uint32_t le32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static bool lock_store(vk_asset_store_t*s){return s&&!atomic_flag_test_and_set_explicit(&s->admission,memory_order_acquire);}
static void unlock_store(vk_asset_store_t*s){atomic_flag_clear_explicit(&s->admission,memory_order_release);}
static bool valid_config(const vk_asset_store_config_t*c){return c&&c->fs&&c->fs->mount&&c->fs->unmount&&c->fs->format&&c->fs->partition_is_all_ff&&c->fs->read_file&&c->fs->file_size&&c->fs->write_new_file&&c->fs->rewrite_file&&c->fs->append_file&&c->fs->truncate_file&&c->fs->remove_file&&c->fs->list_files&&c->fs->free_bytes&&c->validate_vka1&&c->validate_revision&&c->partition_size&&c->reserve_bytes&&c->max_asset_bytes&&c->max_assets;}
static void hex(const uint8_t*d,char*out){static const char x[]="0123456789abcdef";for(size_t i=0;i<32;i++){out[i*2]=x[d[i]>>4];out[i*2+1]=x[d[i]&15];}out[64]=0;}
static const char*kind_name(vk_asset_kind_t k){return k==VK_ASSET_KIND_IMAGE?"image":k==VK_ASSET_KIND_ANIMATION?"animation":k==VK_ASSET_KIND_GLYPH_BITMAP?"glyph_bitmap":NULL;}
static bool pathf(char*out,size_t cap,const char*fmt,uint32_t v){int n=snprintf(out,cap,fmt,(unsigned)v);return n>0&&(size_t)n<cap;}
static bool asset_path(char*out,size_t cap,const uint8_t hash[32]){char h[65];hex(hash,h);int n=snprintf(out,cap,"/assets/%s.vka",h);return n>0&&(size_t)n<cap;}
static esp_err_t read_exact(const vk_asset_store_t*s,const char*n,uint8_t*b,size_t z){size_t got=0,total=0;while(total<z){esp_err_t e=s->config.fs->read_file(s->config.fs_context,n,total,b+total,z-total,&got);if(e!=ESP_OK)return e;if(got==0)return ESP_ERR_INVALID_SIZE;total+=got;}return ESP_OK;}
esp_err_t vk_asset_sha256_file(const vk_asset_store_t *store, const char *name,
                               uint8_t digest[32])
{
    if (store == NULL || name == NULL || digest == NULL) return ESP_ERR_INVALID_ARG;
    size_t size = 0U;
    if (store->config.fs->file_size(store->config.fs_context, name, &size) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t magic[4] = {0};
    bool vka1 = size >= 56U;
    if (vka1) {
        esp_err_t result = read_exact(store, name, magic, sizeof(magic));
        if (result != ESP_OK) return result;
        vka1 = memcmp(magic, "VKA1", sizeof(magic)) == 0;
    }
    uint8_t *buffer = large_alloc(COPY_CHUNK);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    sha_ctx_t sha;
    sha_init(&sha);
    esp_err_t result = ESP_OK;
    for (size_t offset = 0U; offset < size;) {
        size_t read_bytes = 0U;
        size_t wanted = size - offset < COPY_CHUNK ? size - offset : COPY_CHUNK;
        result = store->config.fs->read_file(
            store->config.fs_context, name, offset, buffer, wanted, &read_bytes);
        if (result != ESP_OK || read_bytes == 0U) {
            if (result == ESP_OK) result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (vka1 && offset < 56U && offset + read_bytes > 24U) {
            size_t start = offset < 24U ? 24U : offset;
            size_t end = offset + read_bytes < 56U ? offset + read_bytes : 56U;
            memset(buffer + start - offset, 0, end - start);
        }
        sha_update(&sha, buffer, read_bytes);
        offset += read_bytes;
        yield_storage_owner();
    }
    if (result == ESP_OK) sha_final(&sha, digest);
    free(buffer);
    return result;
}
bool vk_asset_revision_is_newer(uint32_t a,uint32_t b){uint32_t d=a-b;return d!=0&&d<0x80000000U;}

esp_err_t vk_asset_store_init(vk_asset_store_t*s,const vk_asset_store_config_t*c){if(!s||!valid_config(c))return ESP_ERR_INVALID_ARG;memset(s,0,sizeof(*s));s->config=*c;atomic_flag_clear(&s->admission);return ESP_OK;}
esp_err_t vk_asset_store_mount(vk_asset_store_t*s){if(!lock_store(s))return ESP_ERR_INVALID_STATE;esp_err_t e=s->config.fs->mount(s->config.fs_context,false);if(e!=ESP_OK){s->state=VK_ASSET_STORE_MOUNT_FAILED;unlock_store(s);return e;}s->mounted=true;s->state=VK_ASSET_STORE_READY;unlock_store(s);return ESP_OK;}
esp_err_t vk_asset_store_unmount(vk_asset_store_t*s){if(!lock_store(s))return ESP_ERR_INVALID_STATE;esp_err_t e=ESP_OK;if(s->mounted)e=s->config.fs->unmount(s->config.fs_context);if(e==ESP_OK){s->mounted=false;s->state=VK_ASSET_STORE_UNMOUNTED;s->transfer_active=false;s->format_token_valid=false;}unlock_store(s);return e;}
vk_asset_store_state_t vk_asset_store_state(const vk_asset_store_t*s){return s?s->state:VK_ASSET_STORE_UNMOUNTED;}

esp_err_t vk_asset_store_authorize_format(vk_asset_store_t*s,uint32_t epoch,uint64_t nonce,vk_asset_format_token_t*out){if(!s||!out||epoch==0||nonce==0)return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;bool erased=false;uint8_t h[32];esp_err_t e=s->config.fs->partition_is_all_ff(s->config.fs_context,s->config.partition_offset,s->config.partition_size,h,&erased);if(e==ESP_OK&&!erased)e=ESP_ERR_INVALID_STATE;if(e==ESP_OK){memset(out,0,sizeof(*out));out->epoch=epoch;out->nonce=nonce;out->partition_offset=s->config.partition_offset;out->partition_size=s->config.partition_size;memcpy(out->erased_sha256,h,32);s->format_token=*out;s->format_token_valid=true;s->state=VK_ASSET_STORE_UNFORMATTED;}unlock_store(s);return e;}
esp_err_t vk_asset_store_format(vk_asset_store_t*s,uint32_t epoch,const vk_asset_format_token_t*t,const char*confirm){if(!s||!t||!confirm||epoch==0)return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;esp_err_t e=ESP_ERR_INVALID_STATE;if(s->format_token_valid&&s->state==VK_ASSET_STORE_UNFORMATTED&&t->epoch==epoch&&memcmp(t,&s->format_token,sizeof(*t))==0&&strcmp(confirm,VK_ASSET_FORMAT_CONFIRMATION)==0){bool erased=false;uint8_t h[32];e=s->config.fs->partition_is_all_ff(s->config.fs_context,s->config.partition_offset,s->config.partition_size,h,&erased);if(e==ESP_OK&&(!erased||memcmp(h,t->erased_sha256,32)!=0))e=ESP_ERR_INVALID_STATE;s->format_token_valid=false;if(e==ESP_OK)e=s->config.fs->format(s->config.fs_context);if(e==ESP_OK){s->state=VK_ASSET_STORE_UNMOUNTED;s->selected_revision=0;s->previous_revision=0;}}unlock_store(s);return e;}

static esp_err_t write_meta(vk_asset_store_t*s,const vk_asset_transfer_t*t){char p[VK_ASSET_PATH_BYTES],h[65],body[META_MAX];if(!pathf(p,sizeof(p),"/tmp/%08x.meta",t->transfer_id))return ESP_ERR_INVALID_SIZE;hex(t->sha256,h);const char*k=kind_name(t->kind);if(!k)return ESP_ERR_INVALID_ARG;int n=snprintf(body,sizeof(body),"{\"kind\":\"%s\",\"next_offset\":%u,\"schema\":1,\"sha256\":\"%s\",\"total_bytes\":%u,\"transfer_id\":%u}",k,(unsigned)t->next_offset,h,(unsigned)t->total_bytes,(unsigned)t->transfer_id);if(n<=0||(size_t)n>=sizeof(body))return ESP_ERR_INVALID_SIZE;return s->config.fs->rewrite_file(s->config.fs_context,p,(const uint8_t*)body,(size_t)n,true);}
esp_err_t vk_asset_store_begin(vk_asset_store_t*s,const vk_asset_transfer_t*t){if(!s||!t||!t->transfer_id||!t->total_bytes||t->next_offset||t->total_bytes>s->config.max_asset_bytes||!kind_name(t->kind))return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;if(!s->mounted||s->state!=VK_ASSET_STORE_READY||s->transfer_active){unlock_store(s);return ESP_ERR_INVALID_STATE;}uint32_t freeb=0;esp_err_t e=s->config.fs->free_bytes(s->config.fs_context,&freeb);if(e==ESP_OK&&(freeb<=s->config.reserve_bytes||t->total_bytes>freeb-s->config.reserve_bytes))e=ESP_ERR_NO_MEM;char part[VK_ASSET_PATH_BYTES];if(e==ESP_OK&&!pathf(part,sizeof(part),"/tmp/%08x.part",t->transfer_id))e=ESP_ERR_INVALID_SIZE;if(e==ESP_OK)e=s->config.fs->write_new_file(s->config.fs_context,part,NULL,0,true);if(e==ESP_OK){s->transfer=*t;s->transfer_active=true;e=write_meta(s,t);}if(e!=ESP_OK){(void)s->config.fs->remove_file(s->config.fs_context,part);s->transfer_active=false;}unlock_store(s);return e;}
static bool parse_hash_hex(const char*h,uint8_t d[32]){for(size_t i=0;i<32;i++){unsigned hi,lo;char a=h[i*2],b=h[i*2+1];hi=(a>='0'&&a<='9')?(unsigned)(a-'0'):(a>='a'&&a<='f')?(unsigned)(a-'a'+10):16U;lo=(b>='0'&&b<='9')?(unsigned)(b-'0'):(b>='a'&&b<='f')?(unsigned)(b-'a'+10):16U;if(hi>15||lo>15)return false;d[i]=(uint8_t)((hi<<4)|lo);}return h[64]==0;}
static bool restore_meta(vk_asset_store_t*s,uint32_t id,vk_asset_transfer_t*t){char p[96],body[META_MAX],kind[20],hash[65],canonical[META_MAX];pathf(p,sizeof(p),"/tmp/%08x.meta",id);size_t z;if(s->config.fs->file_size(s->config.fs_context,p,&z)!=ESP_OK||z==0||z>=sizeof(body)||read_exact(s,p,(uint8_t*)body,z)!=ESP_OK)return false;body[z]=0;unsigned next,total,transfer;char tail;if(sscanf(body,"{\"kind\":\"%19[^\"]\",\"next_offset\":%u,\"schema\":1,\"sha256\":\"%64[0-9a-f]\",\"total_bytes\":%u,\"transfer_id\":%u}%c",kind,&next,hash,&total,&transfer,&tail)!=5||transfer!=id||total==0||next>total||total>s->config.max_asset_bytes)return false;vk_asset_kind_t k=!strcmp(kind,"image")?VK_ASSET_KIND_IMAGE:!strcmp(kind,"animation")?VK_ASSET_KIND_ANIMATION:!strcmp(kind,"glyph_bitmap")?VK_ASSET_KIND_GLYPH_BITMAP:(vk_asset_kind_t)99;uint8_t d[32];if(!kind_name(k)||!parse_hash_hex(hash,d))return false;int n=snprintf(canonical,sizeof(canonical),"{\"kind\":\"%s\",\"next_offset\":%u,\"schema\":1,\"sha256\":\"%s\",\"total_bytes\":%u,\"transfer_id\":%u}",kind,next,hash,total,transfer);if(n<=0||(size_t)n!=z||memcmp(canonical,body,z)!=0)return false;pathf(p,sizeof(p),"/tmp/%08x.part",id);size_t part;if(s->config.fs->file_size(s->config.fs_context,p,&part)!=ESP_OK||part!=next)return false;*t=(vk_asset_transfer_t){.transfer_id=id,.total_bytes=total,.next_offset=next,.kind=k};memcpy(t->sha256,d,32);return true;}
esp_err_t vk_asset_store_resume(vk_asset_store_t*s,uint32_t id,vk_asset_transfer_t*out){if(!s||!out||!id)return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;esp_err_t e=ESP_OK;if(s->transfer_active){if(s->transfer.transfer_id!=id)e=ESP_ERR_NOT_FOUND;}else if(restore_meta(s,id,&s->transfer)){s->transfer_active=true;}else{char path[VK_ASSET_PATH_BYTES];if(pathf(path,sizeof(path),"/tmp/%08x.part",id))(void)s->config.fs->remove_file(s->config.fs_context,path);if(pathf(path,sizeof(path),"/tmp/%08x.meta",id))(void)s->config.fs->remove_file(s->config.fs_context,path);e=ESP_ERR_NOT_FOUND;}if(e==ESP_OK)*out=s->transfer;unlock_store(s);return e;}
esp_err_t vk_asset_store_append(vk_asset_store_t*s,uint32_t id,uint32_t off,const uint8_t*b,size_t n,uint32_t*next){if(!s||!b||!n||!next)return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;if(!s->transfer_active||s->transfer.transfer_id!=id||s->transfer.next_offset!=off||n>s->transfer.total_bytes-off){unlock_store(s);return ESP_ERR_INVALID_STATE;}char p[VK_ASSET_PATH_BYTES];pathf(p,sizeof(p),"/tmp/%08x.part",id);esp_err_t e=s->config.fs->append_file(s->config.fs_context,p,off,b,n,true);if(e==ESP_OK){vk_asset_transfer_t candidate=s->transfer;candidate.next_offset+=(uint32_t)n;e=write_meta(s,&candidate);if(e==ESP_OK){s->transfer=candidate;*next=candidate.next_offset;}}if(e!=ESP_OK){/* A failed durable append may still have made partial progress before an I/O,
                   fsync, or close error. Restore the sidecar checkpoint before returning. */esp_err_t rollback=s->config.fs->truncate_file(s->config.fs_context,p,off,true);if(rollback!=ESP_OK){s->transfer_active=false;s->state=VK_ASSET_STORE_CORRUPT;e=rollback;}}unlock_store(s);return e;}
static esp_err_t copy_new(vk_asset_store_t *store, const char *source,
                          const char *destination, size_t size)
{
    uint8_t *buffer = large_alloc(COPY_CHUNK);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    esp_err_t result = store->config.fs->write_new_file(
        store->config.fs_context, destination, NULL, 0U, false);
    for (size_t offset = 0U; result == ESP_OK && offset < size;) {
        size_t read_bytes = 0U;
        size_t wanted = size - offset < COPY_CHUNK ? size - offset : COPY_CHUNK;
        result = store->config.fs->read_file(
            store->config.fs_context, source, offset, buffer, wanted, &read_bytes);
        if (result == ESP_OK && read_bytes == 0U) result = ESP_ERR_INVALID_SIZE;
        if (result == ESP_OK) {
            bool final_chunk = offset + read_bytes == size;
            result = store->config.fs->append_file(
                store->config.fs_context, destination, offset, buffer, read_bytes,
                final_chunk);
        }
        offset += read_bytes;
        yield_storage_owner();
    }
    free(buffer);
    if (result != ESP_OK) {
        (void)store->config.fs->remove_file(
            store->config.fs_context, destination);
    }
    return result;
}
esp_err_t vk_asset_store_seal(vk_asset_store_t *store, uint32_t transfer_id)
{
    if (store == NULL || transfer_id == 0U) return ESP_ERR_INVALID_ARG;
    if (!lock_store(store)) return ESP_ERR_INVALID_STATE;
    if (!store->transfer_active ||
        store->transfer.transfer_id != transfer_id ||
        store->transfer.next_offset != store->transfer.total_bytes) {
        unlock_store(store);
        return ESP_ERR_INVALID_STATE;
    }

    char part[VK_ASSET_PATH_BYTES];
    char meta[VK_ASSET_PATH_BYTES];
    char destination[VK_ASSET_PATH_BYTES];
    pathf(part, sizeof(part), "/tmp/%08x.part", transfer_id);
    pathf(meta, sizeof(meta), "/tmp/%08x.meta", transfer_id);
    asset_path(destination, sizeof(destination), store->transfer.sha256);

    uint8_t digest[VK_ASSET_SHA256_BYTES];
    esp_err_t result = vk_asset_sha256_file(store, part, digest);
    if (result == ESP_OK &&
        memcmp(digest, store->transfer.sha256, sizeof(digest)) != 0) {
        result = ESP_ERR_INVALID_CRC;
    }
    if (result == ESP_OK && store->config.validate_vka1 != NULL) {
        result = store->config.validate_vka1(
            store->config.vka1_context, part, store->transfer.sha256,
            store->transfer.total_bytes, store->transfer.kind);
    }

    size_t existing_bytes = 0U;
    if (result == ESP_OK) {
        esp_err_t existing = store->config.fs->file_size(
            store->config.fs_context, destination, &existing_bytes);
        if (existing == ESP_OK) {
            uint8_t existing_digest[VK_ASSET_SHA256_BYTES];
            bool matches = existing_bytes == store->transfer.total_bytes &&
                vk_asset_sha256_file(store, destination, existing_digest) == ESP_OK &&
                memcmp(existing_digest, digest, sizeof(digest)) == 0;
            if (!matches) {
                result = store->config.fs->remove_file(
                    store->config.fs_context, destination);
                if (result == ESP_OK) {
                    result = copy_new(store, part, destination,
                                      store->transfer.total_bytes);
                }
            }
        } else if (existing == ESP_ERR_NOT_FOUND) {
            result = copy_new(store, part, destination,
                              store->transfer.total_bytes);
        } else {
            result = existing;
        }
    }

    if (result == ESP_OK) {
        (void)store->config.fs->remove_file(store->config.fs_context, part);
        (void)store->config.fs->remove_file(store->config.fs_context, meta);
        store->transfer_active = false;
    }
    unlock_store(store);
    return result;
}
esp_err_t vk_asset_store_abort(vk_asset_store_t*s,uint32_t id){if(!s||!id)return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;if(!s->transfer_active||s->transfer.transfer_id!=id){unlock_store(s);return ESP_ERR_NOT_FOUND;}char p[96];pathf(p,sizeof(p),"/tmp/%08x.part",id);(void)s->config.fs->remove_file(s->config.fs_context,p);pathf(p,sizeof(p),"/tmp/%08x.meta",id);(void)s->config.fs->remove_file(s->config.fs_context,p);s->transfer_active=false;unlock_store(s);return ESP_OK;}

static esp_err_t write_manifest(vk_asset_store_t*s,const char*name,const uint8_t*b,size_t n,uint8_t d[32]){vk_asset_sha256(b,n,d);return s->config.fs->write_new_file(s->config.fs_context,name,b,n,true);}
esp_err_t vk_asset_store_publish_revision(vk_asset_store_t*s,const vk_asset_revision_t*r,uint8_t sd[32],uint8_t ad[32]){if(!s||!r||!sd||!ad||!r->revision||!r->assets_manifest||!r->screen_manifest||!r->assets_manifest_bytes||!r->screen_manifest_bytes)return ESP_ERR_INVALID_ARG;if(!lock_store(s))return ESP_ERR_INVALID_STATE;if(!s->mounted||s->state!=VK_ASSET_STORE_READY||r->previous_revision!=s->selected_revision||!vk_asset_revision_is_newer(r->revision,s->selected_revision)){unlock_store(s);return ESP_ERR_INVALID_STATE;}if(s->config.validate_revision){esp_err_t v=s->config.validate_revision(s->config.revision_context,r->revision,r->previous_revision,r->assets_manifest,r->assets_manifest_bytes,r->screen_manifest,r->screen_manifest_bytes);if(v!=ESP_OK){unlock_store(s);return v;}}char ap[96],sp[96],cp[96];pathf(ap,sizeof(ap),"/config/assets-r%08x.json",r->revision);pathf(sp,sizeof(sp),"/config/screen-r%08x.json",r->revision);pathf(cp,sizeof(cp),"/config/commit-r%08x.vkc",r->revision);esp_err_t e=write_manifest(s,ap,r->assets_manifest,r->assets_manifest_bytes,ad);if(e==ESP_OK)e=write_manifest(s,sp,r->screen_manifest,r->screen_manifest_bytes,sd);uint8_t c[VK_ASSET_COMMIT_BYTES]={0};memcpy(c,"VKC1",4);c[4]=1;put32(c+8,r->revision);put32(c+12,r->previous_revision);memcpy(c+16,sd,32);memcpy(c+48,ad,32);vk_asset_sha256(c,80,c+80);if(e==ESP_OK)e=s->config.fs->write_new_file(s->config.fs_context,cp,c,sizeof(c),true);if(e==ESP_OK){s->previous_revision=s->selected_revision;s->selected_revision=r->revision;}unlock_store(s);return e;}
static bool parse_revision_name(const char*n,const char*prefix,const char*suffix,uint32_t*r){unsigned v;char tail;char pattern[80];int pn=snprintf(pattern,sizeof(pattern),"%s%%8x%s%%c",prefix,suffix);if(pn<=0||(size_t)pn>=sizeof(pattern)||sscanf(n,pattern,&v,&tail)!=1)return false;*r=(uint32_t)v;char exact[96];int en=snprintf(exact,sizeof(exact),"%s%08x%s",prefix,v,suffix);return en>0&&(size_t)en<sizeof(exact)&&strcmp(exact,n)==0;}
static bool parse_commit_name(const char*n,uint32_t*r){return parse_revision_name(n,"/config/commit-r",".vkc",r);}
static esp_err_t validate_commit(vk_asset_store_t *store, uint32_t revision,
                                 uint32_t *previous_revision)
{
    char commit_path[VK_ASSET_PATH_BYTES];
    char assets_path[VK_ASSET_PATH_BYTES];
    char screen_path[VK_ASSET_PATH_BYTES];
    if (!pathf(commit_path, sizeof(commit_path), "/config/commit-r%08x.vkc", revision) ||
        !pathf(assets_path, sizeof(assets_path), "/config/assets-r%08x.json", revision) ||
        !pathf(screen_path, sizeof(screen_path), "/config/screen-r%08x.json", revision)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t commit[VK_ASSET_COMMIT_BYTES];
    uint8_t digest[VK_ASSET_SHA256_BYTES];
    size_t commit_bytes = 0U;
    if (store->config.fs->file_size(store->config.fs_context, commit_path, &commit_bytes) != ESP_OK ||
        commit_bytes != sizeof(commit) ||
        read_exact(store, commit_path, commit, sizeof(commit)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(commit, "VKC1", 4U) != 0 || commit[4] != 1U || commit[5] != 0U ||
        commit[6] != 0U || commit[7] != 0U || le32(commit + 8U) != revision) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    vk_asset_sha256(commit, 80U, digest);
    if (memcmp(digest, commit + 80U, sizeof(digest)) != 0) return ESP_ERR_INVALID_CRC;

    size_t assets_bytes = 0U;
    size_t screen_bytes = 0U;
    if (store->config.fs->file_size(store->config.fs_context, assets_path, &assets_bytes) != ESP_OK ||
        store->config.fs->file_size(store->config.fs_context, screen_path, &screen_bytes) != ESP_OK ||
        assets_bytes == 0U || screen_bytes == 0U ||
        assets_bytes > 4092U || screen_bytes > 4092U) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *manifests = large_alloc(assets_bytes + screen_bytes);
    if (manifests == NULL) return ESP_ERR_NO_MEM;
    uint8_t *assets = manifests;
    uint8_t *screen = manifests + assets_bytes;
    esp_err_t result = read_exact(store, assets_path, assets, assets_bytes);
    if (result == ESP_OK) result = read_exact(store, screen_path, screen, screen_bytes);
    if (result == ESP_OK) {
        vk_asset_sha256(assets, assets_bytes, digest);
        if (memcmp(digest, commit + 48U, sizeof(digest)) != 0) result = ESP_ERR_INVALID_CRC;
    }
    if (result == ESP_OK) {
        vk_asset_sha256(screen, screen_bytes, digest);
        if (memcmp(digest, commit + 16U, sizeof(digest)) != 0) result = ESP_ERR_INVALID_CRC;
    }
    uint32_t previous = le32(commit + 12U);
    if (result == ESP_OK && store->config.validate_revision != NULL) {
        result = store->config.validate_revision(store->config.revision_context,
            revision, previous, assets, assets_bytes, screen, screen_bytes);
    }
    free(manifests);
    if (result == ESP_OK) *previous_revision = previous;
    return result;
}

esp_err_t vk_asset_store_recover(vk_asset_store_t *store, vk_asset_recovery_t *recovery)
{
    if (store == NULL || recovery == NULL) return ESP_ERR_INVALID_ARG;
    if (!lock_store(store)) return ESP_ERR_INVALID_STATE;
    if (!store->mounted) {
        unlock_store(store);
        return ESP_ERR_INVALID_STATE;
    }
    char (*names)[VK_ASSET_PATH_BYTES] =
        large_alloc(VK_ASSET_MAX_COMMITS * sizeof(*names));
    if (names == NULL) {
        unlock_store(store);
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0U;
    esp_err_t result = store->config.fs->list_files(
        store->config.fs_context, "/", names, VK_ASSET_MAX_COMMITS, &count);
    uint32_t best = 0U;
    uint32_t best_previous = 0U;
    bool found = false;
    if (result == ESP_OK) {
        for (size_t index = 0U; index < count; ++index) {
            uint32_t revision = 0U;
            uint32_t previous = 0U;
            if (!parse_commit_name(names[index], &revision) ||
                validate_commit(store, revision, &previous) != ESP_OK) continue;
            if (!found || vk_asset_revision_is_newer(revision, best)) {
                best = revision;
                best_previous = previous;
                found = true;
            }
        }
    }
    free(names);
    memset(recovery, 0, sizeof(*recovery));
    if (found) {
        recovery->has_current = true;
        recovery->current_revision = best;
        store->selected_revision = best;
        if (best_previous != 0U &&
            validate_commit(store, best_previous, &recovery->previous_revision) == ESP_OK) {
            recovery->has_previous = true;
            recovery->previous_revision = best_previous;
            store->previous_revision = best_previous;
        } else {
            store->previous_revision = 0U;
        }
    } else {
        store->selected_revision = 0U;
        store->previous_revision = 0U;
        if (result == ESP_OK) result = ESP_ERR_NOT_FOUND;
    }
    unlock_store(store);
    return result;
}

esp_err_t vk_asset_store_load_revision(vk_asset_store_t *s, uint32_t revision,
                                        uint32_t *previous_revision,
                                        uint8_t *assets_manifest, size_t assets_capacity,
                                        size_t *assets_bytes,
                                        uint8_t *screen_manifest, size_t screen_capacity,
                                        size_t *screen_bytes)
{
    if (!s || !revision || !previous_revision || !assets_manifest || !assets_bytes ||
        !screen_manifest || !screen_bytes) return ESP_ERR_INVALID_ARG;
    if (!lock_store(s)) return ESP_ERR_INVALID_STATE;
    if (!s->mounted || (revision != s->selected_revision && revision != s->previous_revision)) {
        unlock_store(s); return ESP_ERR_NOT_FOUND;
    }
    uint32_t previous = 0U;
    esp_err_t result = validate_commit(s, revision, &previous);
    char assets_path[VK_ASSET_PATH_BYTES], screen_path[VK_ASSET_PATH_BYTES];
    size_t assets_size = 0U, screen_size = 0U;
    if (result == ESP_OK && (!pathf(assets_path, sizeof(assets_path), "/config/assets-r%08x.json", revision) ||
                             !pathf(screen_path, sizeof(screen_path), "/config/screen-r%08x.json", revision))) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK) result = s->config.fs->file_size(s->config.fs_context, assets_path, &assets_size);
    if (result == ESP_OK) result = s->config.fs->file_size(s->config.fs_context, screen_path, &screen_size);
    if (result == ESP_OK && (assets_size == 0U || screen_size == 0U || assets_size > assets_capacity || screen_size > screen_capacity)) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK) result = read_exact(s, assets_path, assets_manifest, assets_size);
    if (result == ESP_OK) result = read_exact(s, screen_path, screen_manifest, screen_size);
    if (result == ESP_OK) {
        *previous_revision = previous;
        *assets_bytes = assets_size;
        *screen_bytes = screen_size;
    }
    unlock_store(s);
    return result;
}

static bool parse_asset_name(const char *name, uint8_t digest[32]);
static bool manifest_references(vk_asset_store_t *store, uint32_t revision, const uint8_t digest[32]);

esp_err_t vk_asset_store_collect(vk_asset_store_t *store)
{
    if (store == NULL) return ESP_ERR_INVALID_ARG;
    if (!lock_store(store)) return ESP_ERR_INVALID_STATE;
    if (!store->mounted) {
        unlock_store(store);
        return ESP_ERR_INVALID_STATE;
    }
    char (*names)[VK_ASSET_PATH_BYTES] =
        large_alloc(VK_ASSET_MAX_COMMITS * sizeof(*names));
    if (names == NULL) {
        unlock_store(store);
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0U;
    esp_err_t result = store->config.fs->list_files(
        store->config.fs_context, "/", names, VK_ASSET_MAX_COMMITS, &count);
    if (result == ESP_OK) {
        for (size_t index = 0U; index < count; ++index) {
            uint32_t revision = 0U;
            bool old_commit = parse_commit_name(names[index], &revision) &&
                revision != store->selected_revision && revision != store->previous_revision;
            bool old_assets = parse_revision_name(names[index], "/config/assets-r", ".json", &revision) &&
                revision != store->selected_revision && revision != store->previous_revision;
            bool old_screen = parse_revision_name(names[index], "/config/screen-r", ".json", &revision) &&
                revision != store->selected_revision && revision != store->previous_revision;
            bool remove = old_commit || old_assets || old_screen;
            if (strncmp(names[index], "/tmp/", 5U) == 0) {
                char active_part[VK_ASSET_PATH_BYTES] = {0};
                char active_meta[VK_ASSET_PATH_BYTES] = {0};
                if (store->transfer_active) {
                    pathf(active_part, sizeof(active_part), "/tmp/%08x.part", store->transfer.transfer_id);
                    pathf(active_meta, sizeof(active_meta), "/tmp/%08x.meta", store->transfer.transfer_id);
                }
                remove = strcmp(names[index], active_part) != 0 &&
                    strcmp(names[index], active_meta) != 0;
            }
            uint8_t digest[VK_ASSET_SHA256_BYTES];
            if (parse_asset_name(names[index], digest)) {
                uint8_t actual[VK_ASSET_SHA256_BYTES];
                remove = !manifest_references(store, store->selected_revision, digest) &&
                    !manifest_references(store, store->previous_revision, digest);
                if (!remove && (vk_asset_sha256_file(store, names[index], actual) != ESP_OK ||
                                memcmp(actual, digest, sizeof(actual)) != 0)) {
                    remove = false;
                }
            }
            if (remove) {
                esp_err_t removed = store->config.fs->remove_file(
                    store->config.fs_context, names[index]);
                if (removed != ESP_OK && removed != ESP_ERR_NOT_FOUND) {
                    result = removed;
                    break;
                }
            }
        }
    }
    free(names);
    unlock_store(store);
    return result;
}

static bool parse_asset_name(const char *name, uint8_t digest[32])
{
    static const char prefix[] = "/assets/";
    static const char suffix[] = ".vka";
    size_t prefix_bytes = sizeof(prefix) - 1U;
    if (strncmp(name, prefix, prefix_bytes) != 0 || strlen(name) != prefix_bytes + 64U + sizeof(suffix) - 1U ||
        strcmp(name + prefix_bytes + 64U, suffix) != 0) return false;
    char hash[65];memcpy(hash, name + prefix_bytes, 64U);hash[64] = 0;
    return parse_hash_hex(hash, digest);
}

static bool consume_literal(const uint8_t **cursor, const uint8_t *end, const char *literal)
{
    size_t count = strlen(literal);
    if ((size_t)(end - *cursor) < count || memcmp(*cursor, literal, count) != 0) return false;
    *cursor += count;
    return true;
}

static bool consume_u32(const uint8_t **cursor, const uint8_t *end)
{
    if (*cursor == end || **cursor < '0' || **cursor > '9') return false;
    if (**cursor == '0') {++*cursor;return *cursor == end || **cursor < '0' || **cursor > '9';}
    uint32_t value = 0U;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        uint32_t digit = (uint32_t)(**cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;++*cursor;
    }
    return true;
}

/* Parse the complete canonical assets manifest. Malformed retained manifests fail safe:
 * collection treats every asset as referenced instead of guessing from arbitrary text. */
static bool parse_manifest_reference(const uint8_t *body, size_t size,
                                     const char expected_hash[65], bool *referenced)
{
    const uint8_t *cursor = body, *end = body + size;
    *referenced = false;
    if (!consume_literal(&cursor,end,"{\"assets\":[")) return false;
    bool first = true;
    while (cursor < end && *cursor != ']') {
        if (!first && !consume_literal(&cursor,end,",")) return false;
        first = false;
        if (!consume_literal(&cursor,end,"{\"bytes\":")) return false;
        if (!consume_u32(&cursor,end) || !consume_literal(&cursor,end,",\"kind\":\"")) return false;
        if (consume_literal(&cursor,end,"image\"")) {}
        else if (consume_literal(&cursor,end,"animation\"")) {}
        else if (consume_literal(&cursor,end,"glyph_bitmap\"")) {}
        else return false;
        if (!consume_literal(&cursor,end,",\"sha256\":\"")) return false;
        if ((size_t)(end-cursor)<65U) return false;
        char hash[65];memcpy(hash,cursor,64U);hash[64]=0;
        uint8_t decoded[32];if(!parse_hash_hex(hash,decoded)||cursor[64]!='"')return false;
        if (memcmp(hash,expected_hash,64U)==0)*referenced=true;
        cursor+=65U;if(!consume_literal(&cursor,end,"}"))return false;
    }
    if (!consume_literal(&cursor,end,"],\"previous_revision\":")) return false;
    if (!consume_u32(&cursor,end) || !consume_literal(&cursor,end,",\"revision\":")) return false;
    if (!consume_u32(&cursor,end) || !consume_literal(&cursor,end,",\"schema\":1}")) return false;
    return cursor == end;
}

static bool manifest_references(vk_asset_store_t *store, uint32_t revision, const uint8_t digest[32])
{
    if (revision == 0U) return false;
    char path[VK_ASSET_PATH_BYTES], hash[65];hex(digest, hash);
    if (!pathf(path, sizeof(path), "/config/assets-r%08x.json", revision)) return true;
    size_t size = 0U;
    if (store->config.fs->file_size(store->config.fs_context, path, &size) != ESP_OK ||
        size == 0U || size > 4092U) return true;
    uint8_t *body = large_alloc(size);
    if (body == NULL) return true;
    bool referenced = false;
    bool valid = read_exact(store, path, body, size) == ESP_OK &&
        parse_manifest_reference(body, size, hash, &referenced);
    free(body);
    return valid ? referenced : true;
}

esp_err_t vk_asset_store_status(vk_asset_store_t *store, uint32_t *free_bytes,
                                uint32_t *revision, bool *transfer_active)
{
    if (!store || !free_bytes || !revision || !transfer_active) return ESP_ERR_INVALID_ARG;
    if (!lock_store(store)) return ESP_ERR_INVALID_STATE;
    esp_err_t result = store->mounted ? store->config.fs->free_bytes(store->config.fs_context, free_bytes) : ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) {*revision = store->selected_revision;*transfer_active = store->transfer_active;}
    unlock_store(store);return result;
}

esp_err_t vk_asset_store_transfer(vk_asset_store_t *store, uint32_t transfer_id,
                                  vk_asset_transfer_t *transfer)
{
    return vk_asset_store_resume(store, transfer_id, transfer);
}

esp_err_t vk_asset_store_catalog(vk_asset_store_t *store, vk_asset_catalog_entry_t *entries,
                                 size_t capacity, size_t *count, uint32_t *revision)
{
    if (!store || !entries || !count || !revision || capacity == 0U) return ESP_ERR_INVALID_ARG;
    if (!lock_store(store)) return ESP_ERR_INVALID_STATE;
    char (*names)[VK_ASSET_PATH_BYTES] =
        large_alloc(VK_ASSET_MAX_COMMITS * sizeof(*names));
    if (names == NULL) {
        unlock_store(store);
        return ESP_ERR_NO_MEM;
    }
    size_t name_count = 0U;
    esp_err_t result = store->mounted ? store->config.fs->list_files(store->config.fs_context, "/assets/", names, VK_ASSET_MAX_COMMITS, &name_count) : ESP_ERR_INVALID_STATE;
    size_t used = 0U;
    for (size_t index = 0; result == ESP_OK && index < name_count; ++index) {
        uint8_t digest[32];if (!parse_asset_name(names[index], digest)) continue;
        if (used == capacity || used == store->config.max_assets) {result = ESP_ERR_NO_MEM;break;}
        size_t size = 0U;uint8_t header[5];size_t got = 0U;
        result = store->config.fs->file_size(store->config.fs_context, names[index], &size);
        if (result == ESP_OK) result = store->config.fs->read_file(store->config.fs_context, names[index], 0U, header, sizeof(header), &got);
        if (result != ESP_OK || got != sizeof(header) || memcmp(header, "VKA1", 4U) != 0 || header[4] < 1U || header[4] > 3U || size > UINT32_MAX) {result = ESP_ERR_INVALID_RESPONSE;break;}
        entries[used].total_bytes = (uint32_t)size;entries[used].kind = (vk_asset_kind_t)(header[4] - 1U);
        entries[used].referenced = manifest_references(store, store->selected_revision, digest) || manifest_references(store, store->previous_revision, digest);
        memcpy(entries[used].sha256, digest, 32U);++used;
    }
    for (size_t left = 1U; left < used; ++left) {
        vk_asset_catalog_entry_t item = entries[left];size_t right = left;
        while (right > 0U && memcmp(entries[right - 1U].sha256, item.sha256, 32U) > 0) {entries[right] = entries[right - 1U];--right;}
        entries[right] = item;
    }
    if (result == ESP_OK) {*count = used;*revision = store->selected_revision;}
    free(names);
    unlock_store(store);return result;
}

esp_err_t vk_asset_store_delete(vk_asset_store_t *store,
                                const uint8_t sha256[VK_ASSET_SHA256_BYTES],
                                uint32_t expected_revision)
{
    if (!store || !sha256) return ESP_ERR_INVALID_ARG;
    if (!lock_store(store)) return ESP_ERR_INVALID_STATE;
    if (!store->mounted || store->state != VK_ASSET_STORE_READY || expected_revision != store->selected_revision) {unlock_store(store);return ESP_ERR_INVALID_STATE;}
    if (manifest_references(store, store->selected_revision, sha256) || manifest_references(store, store->previous_revision, sha256)) {unlock_store(store);return ESP_ERR_NOT_ALLOWED;}
    char path[VK_ASSET_PATH_BYTES];esp_err_t result = asset_path(path, sizeof(path), sha256) ? store->config.fs->remove_file(store->config.fs_context, path) : ESP_ERR_INVALID_SIZE;
    unlock_store(store);return result;
}
