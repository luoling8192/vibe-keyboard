#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk_usb_capabilities.h"

static vk_usb_capability_snapshot_t available(void)
{
    vk_usb_capability_snapshot_t snapshot = {0};
    snapshot.assets = (vk_usb_assets_capability_t){
        .state=VK_USB_CAPABILITY_AVAILABLE,.storage_state=VK_USB_STORAGE_READY,
        .free_bytes=1048576,.reserve_bytes=1,.upload_max_bytes=1048575,
        .max_asset_bytes=1048576,.chunk_bytes=4084,.max_assets=64,.max_frames=4,
        .min_frame_ms=1,.max_frame_ms=65535,.max_active_decoded_bytes=243104,
        .decoder_scratch_bytes=4096,.revision=0};
    snapshot.screen = (vk_usb_screen_capability_t){
        .state=VK_USB_CAPABILITY_AVAILABLE,.modes=15,.max_commit_bytes=4092,
        .max_layout_bytes=3072,.max_assets=64,.max_objects=32,.max_depth=4,
        .max_widgets=16,.max_fonts=4,.max_pet_states=6,.max_string_bytes=256,
        .max_json_tokens=512,.max_widget_value_bytes=256,.revision=0,
        .configured=false,.font_count=1};
    strcpy(snapshot.screen.fonts[0].id,"vk-sans");
    snapshot.screen.fonts[0].version=1;
    memset(snapshot.screen.fonts[0].metrics_sha256,'a',64);
    snapshot.screen.fonts[0].metrics_sha256[64]=0;
    return snapshot;
}

int main(void)
{
    char output[VK_USB_MAX_JSON_BYTES+1];size_t length;
    vk_usb_capability_snapshot_t snapshot={0};
    assert(vk_usb_capability_snapshot_validate(&snapshot,false));
    assert(vk_usb_capability_snapshot_encode(&snapshot,output,sizeof(output),&length)==ESP_OK);
    output[length]=0;
    assert(strcmp(output,"{\"event\":\"vk_capabilities\",\"protocol\":1,\"display\":{\"width\":428,\"height\":142,\"format\":\"rgb565\"},\"features\":{}}") == 0);

    snapshot.assets.state=VK_USB_CAPABILITY_UNAVAILABLE;
    snapshot.assets.unavailable_reason=VK_USB_ASSETS_REASON_DISPLAY_ACCEPTANCE_REQUIRED;
    snapshot.screen.state=VK_USB_CAPABILITY_UNAVAILABLE;
    snapshot.screen.unavailable_reason=VK_USB_SCREEN_REASON_DISPLAY_ACCEPTANCE_REQUIRED;
    snapshot.update.state=VK_USB_CAPABILITY_UNAVAILABLE;
    snapshot.update.unavailable_reason=VK_USB_UPDATE_REASON_BOOTLOADER_MIGRATION_REQUIRED;
    assert(vk_usb_capability_snapshot_validate(&snapshot,false));
    assert(vk_usb_capability_snapshot_encode(&snapshot,output,sizeof(output),&length)==ESP_OK);
    output[length]=0;
    assert(strstr(output,"\"assets\":{\"available\":false,\"reason\":\"display_acceptance_required\",\"version\":1}") != NULL);
    assert(strstr(output,"\"update\":{\"available\":false,\"reason\":\"bootloader_migration_required\",\"version\":1}") != NULL);

    snapshot=available();
    assert(vk_usb_capability_snapshot_validate(&snapshot,false));
    assert(vk_usb_capability_snapshot_encode(&snapshot,output,sizeof(output),&length)==ESP_OK);
    output[length]=0;
    assert(strstr(output,"\"encodings\":[\"raw\",\"row_rle\"]") != NULL);
    assert(strstr(output,"\"modes\":[\"image\",\"pet\",\"dashboard\",\"custom\"]") != NULL);

    snapshot.assets.upload_max_bytes=1048576;
    assert(!vk_usb_capability_snapshot_validate(&snapshot,false));
    snapshot=available();snapshot.screen.max_assets=65;
    assert(!vk_usb_capability_snapshot_validate(&snapshot,false));
    snapshot=available();snapshot.screen.state=VK_USB_CAPABILITY_AVAILABLE;snapshot.assets.state=VK_USB_CAPABILITY_UNAVAILABLE;
    assert(!vk_usb_capability_snapshot_validate(&snapshot,false));
    snapshot=available();snapshot.assets.upload_max_bytes=0;
    assert(vk_usb_capability_snapshot_validate(&snapshot,false));
    snapshot=available();snapshot.update=(vk_usb_update_capability_t){.state=VK_USB_CAPABILITY_AVAILABLE,.chunk_bytes=512,.max_image_bytes=1,.target=VK_USB_UPDATE_TARGET_OTA_0};
    assert(!vk_usb_capability_snapshot_validate(&snapshot,false));
    assert(!vk_usb_capability_snapshot_validate(&snapshot,true));
    puts("vk_usb capabilities tests passed");
}
