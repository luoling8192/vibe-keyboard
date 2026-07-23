#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_screen.h"
#include "vk_usb_json.h"

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif

typedef struct {
    unsigned creates;
    unsigned swaps;
    unsigned destroys;
    unsigned widgets;
    unsigned overlay_depth;
    bool fail_create;
    char last_widget[64];
} fake_renderer_t;

static esp_err_t lock_renderer(void *context) { (void)context; return ESP_OK; }
static void unlock_renderer(void *context) { (void)context; }
static esp_err_t create_candidate(void *context, const vk_screen_model_t *model, void **root)
{
    fake_renderer_t *renderer = context;
    renderer->creates++;
    if (renderer->fail_create) return ESP_FAIL;
    vk_screen_model_t *copy = malloc(sizeof(*copy));
    assert(copy != NULL);
    *copy = *model;
    *root = copy;
    return ESP_OK;
}
static esp_err_t swap_root(void *context, void *candidate, void **old_root)
{
    fake_renderer_t *renderer = context;
    renderer->swaps++;
    *old_root = NULL;
    assert(candidate != NULL);
    return ESP_OK;
}
static void destroy_root(void *context, void *root)
{
    ((fake_renderer_t *)context)->destroys++;
    free(root);
}
static esp_err_t apply_widget(void *context, void *root, const char *object,
                              const char *value, uint32_t sequence,
                              vk_screen_widget_state_t state)
{
    (void)root;(void)object;(void)sequence;(void)state;
    fake_renderer_t *renderer = context;
    renderer->widgets++;
    snprintf(renderer->last_widget, sizeof(renderer->last_widget), "%s", value);
    return ESP_OK;
}
static esp_err_t show_overlay(void *context, vk_screen_overlay_t overlay)
{
    (void)overlay;((fake_renderer_t *)context)->overlay_depth++;return ESP_OK;
}
static esp_err_t clear_overlay(void *context, vk_screen_overlay_t overlay)
{
    (void)overlay;fake_renderer_t *renderer=context;if(renderer->overlay_depth==0U)return ESP_FAIL;renderer->overlay_depth--;return ESP_OK;
}

static esp_err_t resolve_asset(void *context, const char *sha, vk_screen_asset_info_t *info)
{
    (void)context;
    if (sha == NULL || strlen(sha) != 64U) return ESP_ERR_NOT_FOUND;
    *info = (vk_screen_asset_info_t){
        .kind = VK_VKA1_IMAGE,
        .width = 1,
        .height = 1,
        .frame_count = 1,
        .decoded_bytes_per_frame = 2,
    };
    return ESP_OK;
}

static vk_screen_config_t config(fake_renderer_t *renderer)
{
    vk_screen_config_t result = {
        .capability = {
            .state = VK_USB_CAPABILITY_AVAILABLE,
            .modes = VK_USB_SCREEN_MODE_IMAGE | VK_USB_SCREEN_MODE_PET |
                     VK_USB_SCREEN_MODE_DASHBOARD | VK_USB_SCREEN_MODE_CUSTOM,
            .max_commit_bytes = 4092,
            .max_layout_bytes = 3072,
            .max_assets = 64,
            .max_objects = 32,
            .max_depth = 4,
            .max_widgets = 16,
            .max_fonts = 4,
            .max_pet_states = 6,
            .max_string_bytes = 256,
            .max_json_tokens = 512,
            .max_widget_value_bytes = 256,
            .font_count = 1,
        },
        .assets = {.resolve = resolve_asset},
        .renderer = {
            .lock = lock_renderer,
            .unlock = unlock_renderer,
            .create_candidate = create_candidate,
            .apply_widget = apply_widget,
            .show_overlay = show_overlay,
            .clear_overlay = clear_overlay,
            .swap_root = swap_root,
            .destroy_root = destroy_root,
            .context = renderer,
        },
        .root_budget_bytes = 4096,
        .decoder_scratch_bytes = 64,
    };
    snprintf(result.capability.fonts[0].id, sizeof(result.capability.fonts[0].id), "vk-sans");
    result.capability.fonts[0].version = 1;
    snprintf(result.capability.fonts[0].metrics_sha256,
             sizeof(result.capability.fonts[0].metrics_sha256), "%s",
             VK_SCREEN_FONT_METRICS_SHA256);
    return result;
}

static vk_usb_screen_command_t command(const char *json, vk_usb_json_document_t *document,
                                       vk_usb_screen_configured_mode_t mode,
                                       uint32_t expected, uint32_t revision)
{
    assert(vk_usb_json_parse(document, (const uint8_t *)json, strlen(json)) == VK_USB_JSON_OK);
    uint16_t root = vk_usb_json_root(document);
    return (vk_usb_screen_command_t){
        .kind = VK_USB_SCREEN_COMMIT,
        .expected_epoch = 7,
        .snapshot_generation = 9,
        .expected_revision = expected,
        .revision = revision,
        .configured_mode = mode,
        .document = document,
        .assets_node = vk_usb_json_object_find(document, root, "assets"),
        .screen_node = vk_usb_json_object_find(document, root, "screen"),
    };
}

static void test_shared_fixture(void)
{
    vk_screen_rect_t children[2] = {
        {.width=10,.height=5},
        {.width=10,.height=5},
    };
    assert(vk_screen_layout_place(true,0,0,31,9,1,1,1,children,2)==ESP_OK);
    assert(children[0].x==5&&children[0].y==2&&children[1].x==16&&children[1].y==2);
    char value[32];
    assert(vk_screen_format_milli(1250,1,value,sizeof(value))==ESP_OK&&strcmp(value,"1.3")==0);
    assert(vk_screen_format_milli(-1250,1,value,sizeof(value))==ESP_OK&&strcmp(value,"-1.3")==0);
    assert(vk_screen_format_milli(0,2,value,sizeof(value))==ESP_OK&&strcmp(value,"0.00")==0);
    int16_t origin;uint16_t advance, pen=0;
    assert(vk_screen_font_glyph_origin("vk-sans",1,VK_SCREEN_FONT_METRICS_SHA256,0x20,pen,&origin,&advance)==ESP_OK);
    assert(origin==0&&advance==4);pen=(uint16_t)(pen+advance);
    assert(vk_screen_font_glyph_origin("vk-sans",1,VK_SCREEN_FONT_METRICS_SHA256,0x41,(int16_t)pen,&origin,&advance)==ESP_OK);
    assert(origin==4&&advance==10);
    /* The shared fixture starts this sample at x=4, yielding [4,8]. */
    assert(vk_screen_font_glyph_origin("vk-sans",1,VK_SCREEN_FONT_METRICS_SHA256,0x20,4,&origin,&advance)==ESP_OK&&origin==4);
    assert(vk_screen_font_glyph_origin("vk-sans",1,VK_SCREEN_FONT_METRICS_SHA256,0x41,8,&origin,&advance)==ESP_OK&&origin==8);
}

static void test_image_commit_and_preservation(void)
{
    static const char hash[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char body[1200];
    snprintf(body,sizeof(body),"{\"assets\":{\"assets\":[{\"bytes\":1,\"kind\":\"image\",\"sha256\":\"%s\"}]},\"event\":\"vk_screen_commit\",\"expected_revision\":0,\"revision\":1,\"screen\":{\"configured_mode\":\"image\",\"image\":{\"background_rgb888\":0,\"fit\":\"contain\",\"sha256\":\"%s\"},\"layout\":null,\"pet\":null}}",hash,hash);
    fake_renderer_t renderer={0};vk_screen_t screen;vk_screen_config_t cfg=config(&renderer);assert(vk_screen_init(&screen,&cfg)==ESP_OK);
    vk_usb_json_document_t document;vk_usb_screen_command_t cmd=command(body,&document,VK_USB_SCREEN_IMAGE,0,1);vk_usb_screen_event_t event;
    assert(vk_screen_commit(&screen,&cmd,&event)==ESP_OK);assert(event.revision==1&&screen.current.configured&&renderer.swaps==1);
    renderer.fail_create=true;cmd.revision=2;cmd.expected_revision=1;assert(vk_screen_commit(&screen,&cmd,&event)==ESP_FAIL);assert(screen.current.revision==1&&renderer.swaps==1);
    renderer.fail_create=false;assert(vk_screen_commit(&screen,&cmd,&event)==ESP_OK);assert(event.revision==2&&screen.current.revision==2&&renderer.swaps==2);
    assert(vk_screen_stop(&screen)==ESP_OK);
}

static void test_layout_widget_overlay_and_pet(void)
{
    const char *body="{\"assets\":{\"assets\":[{\"bytes\":1,\"kind\":\"image\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]},\"event\":\"vk_screen_commit\",\"expected_revision\":0,\"revision\":1,\"screen\":{\"configured_mode\":\"custom\",\"image\":null,\"layout\":{\"background_rgb888\":0,\"mode\":\"custom\",\"objects\":[{\"align\":\"left\",\"clip\":true,\"color_rgb888\":16777215,\"font\":{\"id\":\"vk-sans\",\"version\":1},\"height\":20,\"id\":\"status\",\"overflow\":\"clip\",\"type\":\"dynamic_label\",\"visible\":true,\"widget_id\":\"status\",\"width\":100,\"x\":0,\"y\":0,\"z\":0}],\"revision\":1,\"version\":1,\"widgets\":[{\"fallback\":\"-\",\"id\":\"status\",\"target\":\"status\",\"type\":\"text\"}]},\"pet\":null}}";
    fake_renderer_t renderer={0};vk_screen_t screen;vk_screen_config_t cfg=config(&renderer);assert(vk_screen_init(&screen,&cfg)==ESP_OK);vk_usb_json_document_t document;vk_usb_screen_command_t cmd=command(body,&document,VK_USB_SCREEN_CUSTOM,0,1);vk_usb_screen_event_t event;assert(vk_screen_commit(&screen,&cmd,&event)==ESP_OK);
    vk_screen_widget_update_t update={.revision=1,.sequence=1,.state=VK_SCREEN_WIDGET_FRESH,.has_text=true};snprintf(update.widget_id,sizeof(update.widget_id),"status");snprintf(update.text,sizeof(update.text),"A");char rendered[513];assert(vk_screen_widget_update(&screen,&update,rendered)==ESP_OK&&strcmp(rendered,"A")==0);assert(vk_screen_widget_update(&screen,&update,rendered)!=ESP_OK);
    assert(vk_screen_push_overlay(&screen,VK_SCREEN_OVERLAY_UPLOAD)==ESP_OK);assert(vk_screen_push_overlay(&screen,VK_SCREEN_OVERLAY_RECORDING)==ESP_OK);assert(vk_screen_pop_overlay(&screen,VK_SCREEN_OVERLAY_UPLOAD)!=ESP_OK);assert(vk_screen_pop_overlay(&screen,VK_SCREEN_OVERLAY_RECORDING)==ESP_OK);
    uint16_t durations[]={10,20,30},frame;assert(vk_screen_pet_tick(&screen,100,3,durations,&frame)==ESP_OK&&frame==0);assert(vk_screen_pet_tick(&screen,145,3,durations,&frame)==ESP_OK&&frame==2);
    assert(vk_screen_stop(&screen)==ESP_OK);
}

static void test_durable_restore_without_republish(void)
{
    static const uint8_t assets[] = "{\"assets\":[{\"bytes\":1,\"kind\":\"image\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}],\"previous_revision\":0,\"revision\":1,\"schema\":1}";
    static const uint8_t manifest[] = "{\"configured_mode\":\"image\",\"image\":{\"background_rgb888\":0,\"fit\":\"contain\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"layout\":null,\"pet\":null,\"previous_revision\":0,\"revision\":1,\"schema\":1}";
    fake_renderer_t renderer={0};vk_screen_t screen;vk_screen_config_t cfg=config(&renderer);
    assert(vk_screen_init(&screen,&cfg)==ESP_OK);
    vk_usb_json_document_t document;uint8_t envelope[VK_USB_JSON_MAX_BYTES];
    assert(vk_screen_restore(&screen,1,0,assets,sizeof(assets)-1,manifest,sizeof(manifest)-1,
                             &document,envelope,sizeof(envelope))==ESP_OK);
    assert(screen.current.configured&&screen.current.revision==1&&renderer.swaps==1);
    assert(screen.bound_epoch==0&&screen.bound_snapshot_generation==0);
    assert(vk_screen_stop(&screen)==ESP_OK);

    renderer=(fake_renderer_t){.fail_create=true};cfg=config(&renderer);
    assert(vk_screen_init(&screen,&cfg)==ESP_OK);
    assert(vk_screen_restore(&screen,2,1,assets,sizeof(assets)-1,manifest,sizeof(manifest)-1,
                             &document,envelope,sizeof(envelope))!=ESP_OK);
    assert(!screen.current.configured&&screen.current_root==NULL&&renderer.swaps==0);
    assert(vk_screen_stop(&screen)==ESP_OK);
}

static void test_revision_and_memory(void)
{
    assert(vk_screen_revision_is_newer(1,0));assert(vk_screen_revision_is_newer(0,UINT32_MAX));assert(!vk_screen_revision_is_newer(0x80000000U,0));
    fake_renderer_t renderer={0};vk_screen_t screen;vk_screen_config_t cfg=config(&renderer);cfg.root_budget_bytes=65;assert(vk_screen_init(&screen,&cfg)==ESP_OK);
    static const char hash[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";char body[1000];snprintf(body,sizeof(body),"{\"assets\":{\"assets\":[{\"bytes\":1,\"kind\":\"image\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]},\"event\":\"vk_screen_commit\",\"expected_revision\":0,\"revision\":1,\"screen\":{\"configured_mode\":\"image\",\"image\":{\"background_rgb888\":0,\"fit\":\"contain\",\"sha256\":\"%s\"},\"layout\":null,\"pet\":null}}",hash);vk_usb_json_document_t d;vk_usb_screen_command_t c=command(body,&d,VK_USB_SCREEN_IMAGE,0,1);vk_usb_screen_event_t e;assert(vk_screen_commit(&screen,&c,&e)==ESP_ERR_NO_MEM);assert(!screen.current.configured);assert(vk_screen_stop(&screen)==ESP_OK);
}

int main(void)
{
    test_shared_fixture();
    test_image_commit_and_preservation();
    test_layout_widget_overlay_and_pet();
    test_durable_restore_without_republish();
    test_revision_and_memory();
    puts("screen tests passed");
    return 0;
}
