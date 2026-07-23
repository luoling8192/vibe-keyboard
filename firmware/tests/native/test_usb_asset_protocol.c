#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk_usb_asset_protocol.h"

static vk_usb_json_document_t document;
static const char *sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static uint16_t parse(const char *json)
{
    assert(vk_usb_json_parse(&document, (const uint8_t *)json, strlen(json)) == VK_USB_JSON_OK);
    return vk_usb_json_root(&document);
}

static vk_usb_assets_capability_t assets(void)
{
    vk_usb_assets_capability_t value = {0};
    value.state = VK_USB_CAPABILITY_AVAILABLE;
    value.storage_state = VK_USB_STORAGE_READY;
    value.upload_max_bytes = 1000U;
    value.max_asset_bytes = 2000U;
    value.max_assets = 64U;
    return value;
}

static vk_usb_screen_capability_t screen(void)
{
    vk_usb_screen_capability_t value = {0};
    value.state = VK_USB_CAPABILITY_AVAILABLE;
    value.modes = VK_USB_SCREEN_MODE_IMAGE | VK_USB_SCREEN_MODE_PET | VK_USB_SCREEN_MODE_DASHBOARD | VK_USB_SCREEN_MODE_CUSTOM;
    value.max_commit_bytes = 4092U;
    value.max_layout_bytes = 3072U;
    value.max_assets = 64U;
    value.max_objects = 32U;
    value.max_widgets = 16U;
    value.max_pet_states = 6U;
    value.revision = 0U;
    return value;
}

static void test_asset_commands(void)
{
    vk_usb_assets_capability_t capability = assets(); vk_usb_asset_command_t command; char json[300];
    snprintf(json,sizeof(json),"{\"event\":\"vk_asset_begin\",\"transfer_id\":7,\"sha256\":\"%s\",\"total_bytes\":12,\"kind\":\"image\"}",sha);
    assert(vk_usb_asset_command_decode(&document, parse(json), &capability, 3U, 4U, &command) == ESP_OK);
    assert(command.kind == VK_USB_ASSET_BEGIN && command.expected_epoch == 3U && command.snapshot_generation == 4U && command.asset_kind_value == VK_USB_ASSET_KIND_IMAGE);
    assert(vk_usb_asset_command_decode(&document, parse("{\"event\":\"vk_asset_list\",\"snapshot_id\":0,\"cursor\":0,\"limit\":64}"), &capability, 3U, 4U, &command) == ESP_OK);
    assert(command.kind == VK_USB_ASSET_LIST && command.snapshot_id == 0U && command.cursor == 0U && command.limit == 64U);
    assert(vk_usb_asset_command_decode(&document, parse("{\"event\":\"vk_asset_list\",\"snapshot_id\":0,\"cursor\":1,\"limit\":1}"), &capability, 3U, 4U, &command) == ESP_ERR_INVALID_ARG);
    assert(vk_usb_asset_command_decode(&document, parse("{\"event\":\"vk_asset_query\",\"transfer_id\":1,\"extra\":0}"), &capability, 3U, 4U, &command) == ESP_ERR_INVALID_ARG);
}

static void test_screen_commands(void)
{
    vk_usb_screen_capability_t capability=screen();vk_usb_screen_command_t command;char json[700];
    assert(vk_usb_screen_command_decode(&document,parse("{\"event\":\"vk_screen_query\"}"),&capability,428U,142U,5U,6U,&command)==ESP_OK);
    assert(command.kind==VK_USB_SCREEN_QUERY&&command.expected_epoch==5U&&command.snapshot_generation==6U);
    snprintf(json,sizeof(json),"{\"assets\":{\"assets\":[{\"bytes\":12,\"kind\":\"image\",\"sha256\":\"%s\"}]},\"event\":\"vk_screen_commit\",\"expected_revision\":0,\"revision\":1,\"screen\":{\"configured_mode\":\"image\",\"image\":{\"background_rgb888\":0,\"fit\":\"contain\",\"sha256\":\"%s\"},\"layout\":null,\"pet\":null}}",sha,sha);
    assert(vk_usb_screen_command_decode(&document,parse(json),&capability,428U,142U,5U,6U,&command)==ESP_OK);
    assert(command.kind==VK_USB_SCREEN_COMMIT&&command.configured_mode==VK_USB_SCREEN_IMAGE&&command.revision==1U);
    strcat(json," "); /* trailing whitespace remains valid JSON and body budget. */
    assert(vk_usb_screen_command_decode(&document,parse(json),&capability,428U,142U,5U,6U,&command)==ESP_OK);
    const char *dashboard="{\"assets\":{\"assets\":[]},\"event\":\"vk_screen_commit\",\"expected_revision\":0,\"revision\":1,\"screen\":{\"configured_mode\":\"dashboard\",\"image\":null,\"layout\":{\"background_rgb888\":1054752,\"mode\":\"dashboard\",\"objects\":[{\"align\":\"left\",\"clip\":true,\"color_rgb888\":16777215,\"font\":{\"id\":\"vk-sans\",\"version\":1},\"height\":28,\"id\":\"title\",\"overflow\":\"clip\",\"text\":\"VIBEBOARD CUSTOM\",\"type\":\"static_label\",\"visible\":true,\"width\":396,\"x\":16,\"y\":28,\"z\":0},{\"align\":\"left\",\"clip\":true,\"color_rgb888\":7262463,\"font\":{\"id\":\"vk-sans\",\"version\":1},\"height\":28,\"id\":\"status-value\",\"overflow\":\"clip\",\"type\":\"dynamic_label\",\"visible\":true,\"widget_id\":\"status\",\"width\":396,\"x\":16,\"y\":72,\"z\":1}],\"revision\":1,\"version\":1,\"widgets\":[{\"fallback\":\"READY - K1 K2 K3 K4\",\"id\":\"status\",\"target\":\"status-value\",\"type\":\"text\"}]},\"pet\":null}}";
    assert(vk_usb_screen_command_decode(&document,parse(dashboard),&capability,428U,142U,5U,6U,&command)==ESP_OK);
    assert(command.kind==VK_USB_SCREEN_COMMIT&&command.configured_mode==VK_USB_SCREEN_DASHBOARD&&command.revision==1U);
    capability.max_commit_bytes=32U;
    assert(vk_usb_screen_command_decode(&document,parse(json),&capability,428U,142U,5U,6U,&command)==ESP_ERR_INVALID_ARG);
}

static void test_events(void)
{
    char output[4093];size_t length;vk_usb_asset_event_t event={.kind=VK_USB_ASSET_EVENT_READY,.transfer_id=7,.total_bytes=12,.next_offset=0,.chunk_bytes=4084,.asset_kind=VK_USB_ASSET_KIND_IMAGE};strcpy(event.sha256,sha);
    assert(vk_usb_asset_event_encode(&event,output,sizeof(output),&length)==ESP_OK);output[length]='\0';
    assert(strcmp(output,"{\"chunk_bytes\":4084,\"event\":\"vk_asset_ready\",\"kind\":\"image\",\"next_offset\":0,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"total_bytes\":12,\"transfer_id\":7}")==0);
    vk_usb_asset_list_entry_t entries[2]={{.total_bytes=1,.kind=VK_USB_ASSET_KIND_IMAGE,.referenced=true},{.total_bytes=2,.kind=VK_USB_ASSET_KIND_ANIMATION}};memset(entries[0].sha256,'a',64);entries[0].sha256[64]=0;memset(entries[1].sha256,'b',64);entries[1].sha256[64]=0;
    event=(vk_usb_asset_event_t){.kind=VK_USB_ASSET_EVENT_PAGE,.snapshot_id=9,.cursor=0,.revision=1,.entry_count=2,.entries=entries};
    assert(vk_usb_asset_event_encode(&event,output,sizeof(output),&length)==ESP_OK&&length<=4092U);
    vk_usb_screen_event_t state={.kind=VK_USB_SCREEN_EVENT_STATE};assert(vk_usb_screen_event_encode(&state,output,sizeof(output),&length)==ESP_OK);output[length]=0;assert(strcmp(output,"{\"assets_manifest_sha256\":null,\"configured\":false,\"configured_mode\":null,\"event\":\"vk_screen_state\",\"revision\":0,\"screen_manifest_sha256\":null}")==0);
    vk_usb_protocol_error_t error={.operation=VK_USB_ERROR_ASSET,.code="bad_offset",.has_transfer_id=true,.transfer_id=7,.has_next_offset=true,.next_offset=12};assert(vk_usb_protocol_error_encode(&error,output,sizeof(output),&length)==ESP_OK);output[length]=0;assert(strcmp(output,"{\"code\":\"bad_offset\",\"event\":\"vk_error\",\"next_offset\":12,\"operation\":\"asset\",\"transfer_id\":7}")==0);
    error.operation=VK_USB_ERROR_SCREEN;assert(vk_usb_protocol_error_encode(&error,output,sizeof(output),&length)==ESP_ERR_INVALID_ARG);
    error=(vk_usb_protocol_error_t){.operation=VK_USB_ERROR_SCREEN,.code="render_failed",.message="safe"};assert(vk_usb_protocol_error_encode(&error,output,sizeof(output),&length)==ESP_OK);
    error.message="bad\"message";assert(vk_usb_protocol_error_encode(&error,output,sizeof(output),&length)==ESP_ERR_INVALID_ARG);
}

int main(void){test_asset_commands();test_screen_commands();test_events();puts("vk_usb asset protocol tests passed");}
