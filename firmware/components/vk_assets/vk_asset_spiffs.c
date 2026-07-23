#include "vk_asset_store.h"

#ifdef ESP_PLATFORM

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_partition.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#define BASE "/vkassets"
#define LABEL "storage"

_Static_assert(CONFIG_SPIFFS_OBJ_NAME_LEN >= VK_ASSET_SPIFFS_NAME_MIN_BYTES,
               "SPIFFS object names cannot hold immutable asset paths");

typedef struct { bool mounted; } spiffs_context_t;
static spiffs_context_t context;
static bool host_path(const char *name, char out[160]) { int n=snprintf(out,160,BASE "%s",name);return name&&name[0]=='/'&&strstr(name,"..") == NULL&&n>0&&n<160; }
static esp_err_t from_errno(void){return errno==ENOSPC?ESP_ERR_NO_MEM:errno==EEXIST?ESP_ERR_INVALID_STATE:errno==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL;}
static esp_err_t mount_fs(void *ctx,bool format){(void)ctx;esp_vfs_spiffs_conf_t c={.base_path=BASE,.partition_label=LABEL,.max_files=8,.format_if_mount_failed=format};esp_err_t e=esp_vfs_spiffs_register(&c);if(e==ESP_OK)context.mounted=true;return e;}
static esp_err_t unmount_fs(void *ctx){(void)ctx;if(!context.mounted)return ESP_OK;esp_err_t e=esp_vfs_spiffs_unregister(LABEL);if(e==ESP_OK)context.mounted=false;return e;}
static esp_err_t format_fs(void *ctx){(void)ctx;return esp_spiffs_format(LABEL);}
static esp_err_t erased(void *ctx,uint32_t off,uint32_t size,uint8_t digest[32],bool *all){(void)ctx;if(!digest||!all)return ESP_ERR_INVALID_ARG;const esp_partition_t*p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,LABEL);if(!p||p->address!=off||p->size!=size)return ESP_ERR_INVALID_STATE;mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);if(mbedtls_sha256_starts(&sha,0)!=0){mbedtls_sha256_free(&sha);return ESP_FAIL;}uint8_t b[512];*all=true;for(size_t o=0;o<size;o+=sizeof(b)){size_t n=size-o<sizeof(b)?size-o:sizeof(b);esp_err_t e=esp_partition_read(p,o,b,n);if(e!=ESP_OK){mbedtls_sha256_free(&sha);return e;}for(size_t i=0;i<n;i++)if(b[i]!=0xff)*all=false;if(mbedtls_sha256_update(&sha,b,n)!=0){mbedtls_sha256_free(&sha);return ESP_FAIL;}if(o!=0U&&(o&0xffffU)==0U)vTaskDelay(1U);}int rc=mbedtls_sha256_finish(&sha,digest);mbedtls_sha256_free(&sha);return rc==0?ESP_OK:ESP_FAIL;}
static esp_err_t size_file(void*ctx,const char*n,size_t*z){(void)ctx;char p[160];struct stat st;if(!z||!host_path(n,p))return ESP_ERR_INVALID_ARG;if(stat(p,&st)!=0)return from_errno();*z=(size_t)st.st_size;return ESP_OK;}
static esp_err_t read_file(void*ctx,const char*n,size_t o,uint8_t*b,size_t cap,size_t*got){(void)ctx;char p[160];if(!b||!got||o>(size_t)INT32_MAX||!host_path(n,p))return ESP_ERR_INVALID_ARG;int fd=open(p,O_RDONLY);if(fd<0)return from_errno();ssize_t r=-1;if(lseek(fd,(off_t)o,SEEK_SET)==(off_t)o){do r=read(fd,b,cap);while(r<0&&errno==EINTR);}int saved=errno;close(fd);errno=saved;if(r<0)return from_errno();*got=(size_t)r;return ESP_OK;}
static esp_err_t write_loop(int fd,const uint8_t*b,size_t n){size_t done=0;while(done<n){ssize_t w;do w=write(fd,b+done,n-done);while(w<0&&errno==EINTR);if(w<=0)return w==0?ESP_ERR_INVALID_SIZE:from_errno();done+=(size_t)w;}return ESP_OK;}
static esp_err_t write_common(const char*n,const uint8_t*b,size_t z,int flags,bool sync){char p[160];if((z&& !b)||!host_path(n,p))return ESP_ERR_INVALID_ARG;int fd=open(p,flags,0600);if(fd<0)return from_errno();esp_err_t e=write_loop(fd,b,z);if(e==ESP_OK&&sync&&fsync(fd)!=0)e=from_errno();if(close(fd)!=0&&e==ESP_OK)e=from_errno();return e;}
static esp_err_t write_new(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)c;return write_common(n,b,z,O_WRONLY|O_CREAT|O_EXCL,sync);}
static esp_err_t rewrite(void*c,const char*n,const uint8_t*b,size_t z,bool sync){(void)c;return write_common(n,b,z,O_WRONLY|O_CREAT|O_TRUNC,sync);}
static esp_err_t append(void*c,const char*n,size_t off,const uint8_t*b,size_t z,bool sync){(void)c;char p[160];if(!b||!z||off>(size_t)INT32_MAX||!host_path(n,p))return ESP_ERR_INVALID_ARG;int fd=open(p,O_WRONLY);if(fd<0)return from_errno();struct stat st;esp_err_t e=ESP_OK;if(fstat(fd,&st)!=0)e=from_errno();else if((size_t)st.st_size!=off)e=ESP_ERR_INVALID_STATE;else if(lseek(fd,(off_t)off,SEEK_SET)!=(off_t)off)e=from_errno();else e=write_loop(fd,b,z);if(e==ESP_OK&&sync&&fsync(fd)!=0)e=from_errno();if(close(fd)!=0&&e==ESP_OK)e=from_errno();return e;}
static esp_err_t truncate_one(void*c,const char*n,size_t z,bool sync){(void)c;char p[160];if(!host_path(n,p)||z>(size_t)INT32_MAX)return ESP_ERR_INVALID_ARG;int fd=open(p,O_WRONLY);if(fd<0)return from_errno();esp_err_t e=ftruncate(fd,(off_t)z)==0?ESP_OK:from_errno();if(e==ESP_OK&&sync&&fsync(fd)!=0)e=from_errno();if(close(fd)!=0&&e==ESP_OK)e=from_errno();return e;}
static esp_err_t remove_one(void*c,const char*n){(void)c;char p[160];if(!host_path(n,p))return ESP_ERR_INVALID_ARG;return unlink(p)==0?ESP_OK:from_errno();}
static esp_err_t list(void*c,const char*prefix,char names[][VK_ASSET_PATH_BYTES],size_t cap,size_t*count)
{
    (void)c;
    if(!prefix||!names||!count||prefix[0]!='/')return ESP_ERR_INVALID_ARG;
    *count=0;
    DIR*d=opendir(BASE);
    if(!d)return errno==ENOENT?ESP_OK:from_errno();
    struct dirent*e;
    while((e=readdir(d))!=NULL){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char logical[VK_ASSET_PATH_BYTES];
        int n=snprintf(logical,sizeof(logical),e->d_name[0]=='/'?"%s":"/%s",e->d_name);
        if(n<=0||(size_t)n>=sizeof(logical)){closedir(d);return ESP_ERR_INVALID_SIZE;}
        if(strcmp(prefix,"/")!=0&&strncmp(logical,prefix,strlen(prefix))!=0)continue;
        if(*count==cap){closedir(d);return ESP_ERR_NO_MEM;}
        memcpy(names[*count],logical,(size_t)n+1U);
        (*count)++;
    }
    closedir(d);
    return ESP_OK;
}
static esp_err_t free_space(void*c,uint32_t*out){(void)c;size_t total=0,used=0;if(!out)return ESP_ERR_INVALID_ARG;esp_err_t e=esp_spiffs_info(LABEL,&total,&used);if(e!=ESP_OK)return e;if(total-used>UINT32_MAX)return ESP_ERR_INVALID_SIZE;*out=(uint32_t)(total-used);return ESP_OK;}
esp_err_t vk_asset_spiffs_make_ops(vk_asset_fs_ops_t*o){if(!o)return ESP_ERR_INVALID_ARG;*o=(vk_asset_fs_ops_t){.mount=mount_fs,.unmount=unmount_fs,.format=format_fs,.partition_is_all_ff=erased,.read_file=read_file,.file_size=size_file,.write_new_file=write_new,.rewrite_file=rewrite,.append_file=append,.truncate_file=truncate_one,.remove_file=remove_one,.list_files=list,.free_bytes=free_space};return ESP_OK;}

#endif
