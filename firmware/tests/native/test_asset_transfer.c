#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk_asset_transfer.h"

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

#define MAX_FILES 12
#define MAX_DATA 8192

typedef struct { char name[96]; uint8_t data[MAX_DATA]; size_t size; bool used; } file_t;
typedef struct { file_t files[MAX_FILES]; bool mounted; uint32_t free_bytes; uint64_t now; } fixture_t;

static file_t *find_file(fixture_t *fixture, const char *name) { for (size_t i=0;i<MAX_FILES;i++) if(fixture->files[i].used&&!strcmp(fixture->files[i].name,name)) return &fixture->files[i]; return NULL; }
static file_t *new_file(fixture_t *fixture,const char *name){if(find_file(fixture,name))return NULL;for(size_t i=0;i<MAX_FILES;i++)if(!fixture->files[i].used){fixture->files[i].used=true;snprintf(fixture->files[i].name,sizeof(fixture->files[i].name),"%s",name);return &fixture->files[i];}return NULL;}
static esp_err_t mount_fs(void *c,bool format){assert(!format);return ((fixture_t*)c)->mounted?ESP_OK:ESP_FAIL;}
static esp_err_t unmount_fs(void*c){(void)c;return ESP_OK;}static esp_err_t format_fs(void*c){(void)c;assert(false);return ESP_FAIL;}
static esp_err_t erased_fs(void*c,uint32_t o,uint32_t s,uint8_t h[32],bool*a){(void)c;(void)o;(void)s;memset(h,0,32);*a=false;return ESP_OK;}
static esp_err_t read_fs(void*c,const char*n,size_t o,uint8_t*b,size_t z,size_t*g){file_t*f=find_file(c,n);if(!f)return ESP_ERR_NOT_FOUND;if(o>f->size)return ESP_ERR_INVALID_STATE;*g=f->size-o<z?f->size-o:z;memcpy(b,f->data+o,*g);return ESP_OK;}
static esp_err_t size_fs(void*c,const char*n,size_t*z){file_t*f=find_file(c,n);if(!f)return ESP_ERR_NOT_FOUND;*z=f->size;return ESP_OK;}
static esp_err_t write_new(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)sync;file_t*f=new_file(c,n);if(!f||z>MAX_DATA)return ESP_ERR_NO_MEM;if(z)memcpy(f->data,b,z);f->size=z;return ESP_OK;}
static esp_err_t rewrite(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)sync;file_t*f=find_file(c,n);if(!f)f=new_file(c,n);if(!f||z>MAX_DATA)return ESP_ERR_NO_MEM;if(z)memcpy(f->data,b,z);f->size=z;return ESP_OK;}
static esp_err_t append(void*c,const char*n,size_t o,const uint8_t*b,size_t z,bool sync){(void)sync;file_t*f=find_file(c,n);if(!f||f->size!=o||o+z>MAX_DATA)return ESP_ERR_INVALID_STATE;memcpy(f->data+o,b,z);f->size+=z;return ESP_OK;}
static esp_err_t truncate_file(void*c,const char*n,size_t z,bool sync){(void)sync;file_t*f=find_file(c,n);if(!f||z>f->size)return ESP_ERR_INVALID_STATE;f->size=z;return ESP_OK;}
static esp_err_t remove_file(void*c,const char*n){file_t*f=find_file(c,n);if(!f)return ESP_ERR_NOT_FOUND;f->used=false;return ESP_OK;}
static esp_err_t list_fs(void*c,const char*p,char names[][96],size_t cap,size_t*count){fixture_t*f=c;*count=0;for(size_t i=0;i<MAX_FILES;i++)if(f->files[i].used&&!strncmp(f->files[i].name,p,strlen(p))){if(*count==cap)return ESP_ERR_NO_MEM;snprintf(names[(*count)++],96,"%s",f->files[i].name);}return ESP_OK;}
static esp_err_t free_fs(void*c,uint32_t*out){*out=((fixture_t*)c)->free_bytes;return ESP_OK;}
static esp_err_t validate_vka(void*c,const char*n,const uint8_t h[32],uint32_t z,vk_asset_kind_t k){(void)c;(void)n;(void)h;(void)z;(void)k;return ESP_OK;}
static esp_err_t validate_revision(void*c,uint32_t r,uint32_t p,const uint8_t*a,size_t az,const uint8_t*s,size_t sz){(void)c;(void)r;(void)p;(void)a;(void)az;(void)s;(void)sz;return ESP_OK;}
static uint64_t clock_ms(void*c){return ((fixture_t*)c)->now;}
static const vk_asset_fs_ops_t fs_ops={mount_fs,unmount_fs,format_fs,erased_fs,read_fs,size_fs,write_new,rewrite,append,truncate_file,remove_file,list_fs,free_fs};

static void hex_sha(const uint8_t digest[32],char output[65]){static const char h[]="0123456789abcdef";for(size_t i=0;i<32;i++){output[i*2]=h[digest[i]>>4];output[i*2+1]=h[digest[i]&15];}output[64]=0;}

int main(void)
{
    fixture_t fixture={.mounted=true,.free_bytes=10000};
    vk_asset_store_t store;vk_asset_store_config_t store_config={.fs=&fs_ops,.fs_context=&fixture,.validate_vka1=validate_vka,.validate_revision=validate_revision,.partition_offset=1,.partition_size=10000,.reserve_bytes=100,.max_asset_bytes=4096,.max_assets=8};
    assert(vk_asset_store_init(&store,&store_config)==ESP_OK);assert(vk_asset_store_mount(&store)==ESP_OK);
    vk_asset_transfer_service_t service;vk_asset_transfer_config_t config={.store=&store,.now_ms=clock_ms,.clock_context=&fixture,.chunk_bytes=4,.max_frames=4,.min_frame_ms=10,.max_frame_ms=1000,.max_active_decoded_bytes=121552,.decoder_scratch_bytes=4096,.production_available=true};
    assert(vk_asset_transfer_init(&service,&config)==ESP_OK);
    vk_usb_capability_snapshot_t snapshot;assert(vk_asset_transfer_get_capabilities(&service,7,&snapshot)==ESP_OK);assert(snapshot.assets.state==VK_USB_CAPABILITY_AVAILABLE&&snapshot.assets.storage_state==VK_USB_STORAGE_READY&&snapshot.assets.upload_max_bytes==4096);assert(snapshot.screen.state==VK_USB_CAPABILITY_ABSENT);assert(snapshot.update.state==VK_USB_CAPABILITY_UNAVAILABLE);
    uint8_t payload[]={1,2,3,4,5};uint8_t digest[32];vk_asset_sha256(payload,sizeof(payload),digest);char hash[65];hex_sha(digest,hash);
    vk_usb_asset_command_t begin={.kind=VK_USB_ASSET_BEGIN,.expected_epoch=7,.snapshot_generation=9,.transfer_id=11,.total_bytes=5,.asset_kind_value=VK_USB_ASSET_KIND_IMAGE};snprintf(begin.asset_kind,sizeof(begin.asset_kind),"image");snprintf(begin.sha256,sizeof(begin.sha256),"%s",hash);
    assert(vk_asset_transfer_handle_command(&service,&begin)==ESP_OK);
    vk_usb_asset_command_t tuple;uint32_t next;assert(vk_asset_transfer_get_state(&service,11,7,9,&tuple,&next)==ESP_OK&&next==0&&tuple.total_bytes==5&&!strcmp(tuple.sha256,hash));
    vk_usb_asset_chunk_t first={.expected_epoch=7,.snapshot_generation=9,.transfer_id=11,.offset=0,.payload_length=4};memcpy(first.payload,payload,4);assert(vk_asset_transfer_handle_chunk(&service,&first)==ESP_OK);
    assert(vk_asset_transfer_get_state(&service,11,8,10,&tuple,&next)==ESP_OK&&next==4);
    vk_asset_transfer_service_t reconnected;assert(vk_asset_transfer_init(&reconnected,&config)==ESP_OK);assert(vk_asset_transfer_get_state(&reconnected,11,8,10,&tuple,&next)==ESP_OK&&next==4);
    vk_usb_asset_chunk_t last={.expected_epoch=8,.snapshot_generation=10,.transfer_id=11,.offset=4,.payload_length=1};last.payload[0]=5;assert(vk_asset_transfer_handle_chunk(&reconnected,&last)==ESP_OK);
    vk_usb_asset_command_t end=tuple;end.kind=VK_USB_ASSET_END;end.expected_epoch=8;end.snapshot_generation=10;assert(vk_asset_transfer_handle_command(&reconnected,&end)==ESP_OK);
    assert(vk_asset_transfer_get_state(&reconnected,11,8,10,&tuple,&next)==ESP_ERR_NOT_FOUND);
    vk_asset_transfer_config_t unavailable=config;unavailable.production_available=false;vk_asset_transfer_service_t gated;assert(vk_asset_transfer_init(&gated,&unavailable)==ESP_OK);assert(vk_asset_transfer_get_capabilities(&gated,1,&snapshot)==ESP_OK&&snapshot.assets.state==VK_USB_CAPABILITY_UNAVAILABLE);
    puts("asset transfer integration tests passed");
}
