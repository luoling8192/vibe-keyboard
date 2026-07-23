#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk_asset_store.h"
#include "vk_screen_service.h"

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

#define FILE_CAPACITY 24U
#define FILE_BYTES 4096U

typedef struct { char name[VK_ASSET_PATH_BYTES]; uint8_t data[FILE_BYTES]; size_t size; bool used; } file_t;
typedef struct { file_t files[FILE_CAPACITY]; } fs_t;
typedef struct { unsigned swaps; bool reject_b; void *root; } renderer_t;

static file_t *file_find(fs_t *fs, const char *name) { for (size_t i=0;i<FILE_CAPACITY;i++) if (fs->files[i].used&&!strcmp(fs->files[i].name,name)) return &fs->files[i]; return NULL; }
static file_t *file_new(fs_t *fs, const char *name) { if (file_find(fs,name)) return NULL; for(size_t i=0;i<FILE_CAPACITY;i++)if(!fs->files[i].used){fs->files[i].used=true;snprintf(fs->files[i].name,sizeof(fs->files[i].name),"%s",name);return &fs->files[i];}return NULL; }
static esp_err_t mount_fs(void *c,bool format){(void)c;assert(!format);return ESP_OK;}
static esp_err_t unmount_fs(void*c){(void)c;return ESP_OK;}
static esp_err_t format_fs(void*c){(void)c;return ESP_FAIL;}
static esp_err_t erased_fs(void*c,uint32_t o,uint32_t z,uint8_t h[32],bool*a){(void)c;(void)o;(void)z;memset(h,0,32);*a=false;return ESP_OK;}
static esp_err_t read_fs(void*c,const char*n,size_t o,uint8_t*b,size_t cap,size_t*got){file_t*f=file_find(c,n);if(!f)return ESP_ERR_NOT_FOUND;if(o>f->size)return ESP_ERR_INVALID_STATE;*got=f->size-o<cap?f->size-o:cap;memcpy(b,f->data+o,*got);return ESP_OK;}
static esp_err_t size_fs(void*c,const char*n,size_t*z){file_t*f=file_find(c,n);if(!f)return ESP_ERR_NOT_FOUND;*z=f->size;return ESP_OK;}
static esp_err_t new_fs(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)sync;if(z>FILE_BYTES)return ESP_ERR_NO_MEM;file_t*f=file_new(c,n);if(!f)return ESP_ERR_INVALID_STATE;if(z)memcpy(f->data,b,z);f->size=z;return ESP_OK;}
static esp_err_t rewrite_fs(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)sync;file_t*f=file_find(c,n);if(!f)f=file_new(c,n);if(!f||z>FILE_BYTES)return ESP_ERR_NO_MEM;if(z)memcpy(f->data,b,z);f->size=z;return ESP_OK;}
static esp_err_t append_fs(void*c,const char*n,size_t o,const uint8_t*b,size_t z,bool sync){(void)sync;file_t*f=file_find(c,n);if(!f||f->size!=o||o+z>FILE_BYTES)return ESP_ERR_INVALID_STATE;memcpy(f->data+o,b,z);f->size+=z;return ESP_OK;}
static esp_err_t truncate_fs(void*c,const char*n,size_t z,bool sync){(void)sync;file_t*f=file_find(c,n);if(!f||z>f->size)return ESP_ERR_INVALID_STATE;f->size=z;return ESP_OK;}
static esp_err_t remove_fs(void*c,const char*n){file_t*f=file_find(c,n);if(!f)return ESP_ERR_NOT_FOUND;f->used=false;return ESP_OK;}
static esp_err_t list_fs(void*c,const char*p,char names[][VK_ASSET_PATH_BYTES],size_t cap,size_t*count){fs_t*fs=c;*count=0;for(size_t i=0;i<FILE_CAPACITY;i++)if(fs->files[i].used&&(!strcmp(p,"/")||!strncmp(fs->files[i].name,p,strlen(p)))){if(*count==cap)return ESP_ERR_NO_MEM;snprintf(names[(*count)++],VK_ASSET_PATH_BYTES,"%s",fs->files[i].name);}return ESP_OK;}
static esp_err_t free_fs(void*c,uint32_t*out){(void)c;*out=100000;return ESP_OK;}
static esp_err_t valid_asset(void*c,const char*n,const uint8_t h[32],uint32_t z,vk_asset_kind_t k){(void)c;(void)n;(void)h;(void)z;(void)k;return ESP_OK;}
static esp_err_t valid_revision(void*c,uint32_t r,uint32_t p,const uint8_t*a,size_t az,const uint8_t*s,size_t sz){(void)c;(void)r;(void)p;(void)a;(void)az;(void)s;(void)sz;return ESP_OK;}
static const vk_asset_fs_ops_t fs_ops={mount_fs,unmount_fs,format_fs,erased_fs,read_fs,size_fs,new_fs,rewrite_fs,append_fs,truncate_fs,remove_fs,list_fs,free_fs};

static esp_err_t resolve(void *context,const char *sha,vk_screen_asset_info_t *info){renderer_t*r=context;if(r->reject_b&&sha[0]=='b')return ESP_ERR_NOT_FOUND;*info=(vk_screen_asset_info_t){.kind=VK_VKA1_IMAGE,.width=1,.height=1,.frame_count=1,.decoded_bytes_per_frame=2,.container_bytes=1};return ESP_OK;}
static esp_err_t renderer_lock(void*c){(void)c;return ESP_OK;}static void renderer_unlock(void*c){(void)c;}
static esp_err_t create_root(void*c,const vk_screen_model_t*m,void**root){(void)m;*root=(uint8_t*)c+1;return ESP_OK;}
static esp_err_t swap_root(void*c,void*root,void**old){renderer_t*r=c;*old=r->root;r->root=root;r->swaps++;return ESP_OK;}
static void destroy_root(void*c,void*r){(void)c;(void)r;}

static vk_screen_config_t screen_config(renderer_t *renderer){vk_screen_config_t c={.capability={.state=VK_USB_CAPABILITY_AVAILABLE,.modes=VK_USB_SCREEN_MODE_IMAGE,.max_commit_bytes=4092,.max_layout_bytes=3072,.max_assets=64,.max_objects=32,.max_depth=4,.max_widgets=16,.max_fonts=1,.max_pet_states=6,.max_string_bytes=256,.max_json_tokens=512,.max_widget_value_bytes=256,.font_count=1},.assets={.resolve=resolve,.context=renderer},.renderer={.lock=renderer_lock,.unlock=renderer_unlock,.create_candidate=create_root,.swap_root=swap_root,.destroy_root=destroy_root,.context=renderer},.root_budget_bytes=4096,.decoder_scratch_bytes=64};snprintf(c.capability.fonts[0].id,sizeof(c.capability.fonts[0].id),"vk-sans");c.capability.fonts[0].version=1;snprintf(c.capability.fonts[0].metrics_sha256,sizeof(c.capability.fonts[0].metrics_sha256),"%s",VK_SCREEN_FONT_METRICS_SHA256);return c;}
static void publish(vk_asset_store_t*s,uint32_t revision,uint32_t previous,char hash){char assets[256],screen[512],sha[65];memset(sha,hash,64);sha[64]=0;snprintf(assets,sizeof(assets),"{\"assets\":[{\"bytes\":1,\"kind\":\"image\",\"sha256\":\"%s\"}],\"previous_revision\":%u,\"revision\":%u,\"schema\":1}",sha,previous,revision);snprintf(screen,sizeof(screen),"{\"configured_mode\":\"image\",\"image\":{\"background_rgb888\":0,\"fit\":\"contain\",\"sha256\":\"%s\"},\"layout\":null,\"pet\":null,\"previous_revision\":%u,\"revision\":%u,\"schema\":1}",sha,previous,revision);vk_asset_revision_t r={revision,previous,(const uint8_t*)assets,strlen(assets),(const uint8_t*)screen,strlen(screen)};uint8_t sh[32],ah[32];assert(vk_asset_store_publish_revision(s,&r,sh,ah)==ESP_OK);}

int main(void){fs_t fs={0};vk_asset_store_config_t sc={.fs=&fs_ops,.fs_context=&fs,.validate_vka1=valid_asset,.validate_revision=valid_revision,.partition_offset=0xa20000,.partition_size=0x5e0000,.reserve_bytes=1,.max_asset_bytes=4096,.max_assets=64};vk_asset_store_t store;assert(vk_asset_store_init(&store,&sc)==ESP_OK);assert(vk_asset_store_mount(&store)==ESP_OK);publish(&store,1,0,'a');publish(&store,2,1,'b');vk_asset_store_t reboot;assert(vk_asset_store_init(&reboot,&sc)==ESP_OK);assert(vk_asset_store_mount(&reboot)==ESP_OK);vk_asset_recovery_t recovery;assert(vk_asset_store_recover(&reboot,&recovery)==ESP_OK&&recovery.current_revision==2&&recovery.previous_revision==1);renderer_t renderer={.reject_b=true};vk_screen_t screen;vk_screen_config_t cfg=screen_config(&renderer);assert(vk_screen_init(&screen,&cfg)==ESP_OK);vk_screen_service_t service={.store=&reboot,.screen=&screen};vk_usb_json_document_t document;uint8_t envelope[VK_USB_MAX_JSON_BYTES];assert(vk_screen_service_restore(&service,&recovery,&document,envelope,sizeof(envelope))==ESP_OK);assert(screen.current.revision==1&&renderer.swaps==1&&service.store_recovered);assert(vk_screen_stop(&screen)==ESP_OK);puts("screen durable recovery fallback passed");}
