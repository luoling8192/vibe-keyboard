#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "vk_screen.h"

static vk_screen_t screen;
static unsigned widget_calls;
static esp_err_t lock_renderer(void *context){(void)context;return ESP_OK;}
static void unlock_renderer(void *context){(void)context;}
static esp_err_t create_renderer(void *context,const vk_screen_model_t*model,void**root){(void)context;(void)model;*root=(void*)1;return ESP_OK;}
static esp_err_t swap_renderer(void *context,void*candidate,void**old){(void)context;(void)candidate;*old=NULL;return ESP_OK;}
static void destroy_renderer(void *context,void*root){(void)context;(void)root;}
static esp_err_t widget_renderer(void *context,void*root,const char*object,const char*value,uint32_t sequence,vk_screen_widget_state_t state){(void)context;(void)root;(void)object;(void)value;(void)sequence;(void)state;widget_calls++;return ESP_OK;}
static esp_err_t overlay_renderer(void *context,vk_screen_overlay_t overlay){(void)context;(void)overlay;return ESP_OK;}
static esp_err_t resolve(void*context,const char*sha,vk_screen_asset_info_t*info){(void)context;(void)sha;*info=(vk_screen_asset_info_t){.kind=VK_VKA1_IMAGE,.width=1,.height=1,.frame_count=1,.decoded_bytes_per_frame=2};return ESP_OK;}
static void *update_thread(void *unused){(void)unused;for(uint32_t sequence=1;sequence<1000;sequence++){vk_screen_widget_update_t update={.revision=1,.sequence=sequence,.state=VK_SCREEN_WIDGET_FRESH,.has_text=true};snprintf(update.widget_id,sizeof(update.widget_id),"status");snprintf(update.text,sizeof(update.text),"A");char out[513];(void)vk_screen_widget_update(&screen,&update,out);}return NULL;}
static void *overlay_thread(void *unused){(void)unused;for(unsigned i=0;i<1000;i++){if(vk_screen_push_overlay(&screen,VK_SCREEN_OVERLAY_UPLOAD)==ESP_OK)(void)vk_screen_pop_overlay(&screen,VK_SCREEN_OVERLAY_UPLOAD);}return NULL;}
int main(void){vk_screen_config_t cfg={.capability={.state=VK_USB_CAPABILITY_AVAILABLE,.max_objects=32,.max_depth=4,.max_widgets=16,.max_widget_value_bytes=256},.assets={.resolve=resolve},.renderer={.lock=lock_renderer,.unlock=unlock_renderer,.create_candidate=create_renderer,.apply_widget=widget_renderer,.show_overlay=overlay_renderer,.clear_overlay=overlay_renderer,.swap_root=swap_renderer,.destroy_root=destroy_renderer},.root_budget_bytes=4096,.decoder_scratch_bytes=64};assert(vk_screen_init(&screen,&cfg)==ESP_OK);screen.current.configured=true;screen.current.revision=1;screen.current.widget_count=1;screen.current.widgets[0].type=VK_SCREEN_WIDGET_TEXT;snprintf(screen.current.widgets[0].id,sizeof(screen.current.widgets[0].id),"status");snprintf(screen.current.widgets[0].target,sizeof(screen.current.widgets[0].target),"status");pthread_t a,b;assert(pthread_create(&a,NULL,update_thread,NULL)==0);assert(pthread_create(&b,NULL,overlay_thread,NULL)==0);pthread_join(a,NULL);pthread_join(b,NULL);assert(vk_screen_stop(&screen)==ESP_OK);puts("screen concurrency tests passed");return 0;}
