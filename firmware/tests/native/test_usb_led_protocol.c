#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk_usb_led_protocol.h"

static vk_usb_json_document_t document;
static vk_usb_led_capability_t available(void)
{
    vk_usb_led_capability_t result={.state=VK_USB_CAPABILITY_AVAILABLE,.key_pixels={0,1,2,3},
        .strip_first=4,.strip_count=13,.max_brightness=64,.max_frame_channel_sum=3264};
    return result;
}
static void parse(const char *json)
{
    assert(vk_usb_json_parse(&document,(const uint8_t *)json,strlen(json))==VK_USB_JSON_OK);
}
int main(void)
{
    vk_usb_led_capability_t capability=available();vk_usb_led_command_t command;
    parse("{\"event\":\"vk_led_query\",\"request_id\":1}");
    assert(vk_usb_led_command_decode(&document,vk_usb_json_root(&document),&capability,&command)==ESP_OK);
    assert(command.kind==VK_USB_LED_QUERY&&command.request_id==1U);
    parse("{\"event\":\"vk_led_config\",\"request_id\":2,\"enabled\":true,\"brightness\":64}");
    assert(vk_usb_led_command_decode(&document,vk_usb_json_root(&document),&capability,&command)==ESP_OK);
    assert(command.kind==VK_USB_LED_CONFIG&&command.enabled&&command.brightness==64U);
    parse("{\"event\":\"vk_led_config\",\"request_id\":2,\"enabled\":true,\"brightness\":65}");
    assert(vk_usb_led_command_decode(&document,vk_usb_json_root(&document),&capability,&command)==ESP_ERR_INVALID_ARG);
    capability.state=VK_USB_CAPABILITY_UNAVAILABLE;capability.unavailable_reason=VK_USB_LED_REASON_CALIBRATION_REQUIRED;
    parse("{\"event\":\"vk_led_config\",\"request_id\":2,\"enabled\":true,\"brightness\":1}");
    assert(vk_usb_led_command_decode(&document,vk_usb_json_root(&document),&capability,&command)==ESP_ERR_NOT_SUPPORTED);
    parse("{\"event\":\"vk_led_config\",\"request_id\":0,\"enabled\":true,\"brightness\":1}");
    assert(vk_usb_led_command_decode(&document,vk_usb_json_root(&document),&capability,&command)==ESP_ERR_INVALID_ARG);

    char output[256];size_t length=0;
    vk_usb_led_state_event_t state={.source=VK_USB_LED_STATE_QUERY,.request_id=1,.available=false,
        .unavailable_reason=VK_USB_LED_REASON_CALIBRATION_REQUIRED};
    assert(vk_usb_led_state_encode(&state,output,sizeof(output),&length)==ESP_OK);
    assert(strstr(output,"\"calibration_required\"")!=NULL);
    state=(vk_usb_led_state_event_t){.source=VK_USB_LED_STATE_APPLIED,.request_id=2,.available=true,
        .enabled=true,.brightness=32,.effective=VK_USB_LED_EFFECTIVE_CONNECTED};
    assert(vk_usb_led_state_encode(&state,output,sizeof(output),&length)==ESP_OK);
    assert(strstr(output,"\"source\":\"applied\"")!=NULL);
    vk_usb_led_error_event_t error={.request_id=2,.code=VK_USB_LED_ERROR_UNAVAILABLE};
    assert(vk_usb_led_error_encode(&error,output,sizeof(output),&length)==ESP_OK);
    assert(strstr(output,"\"operation\":\"led\"")!=NULL);
    puts("usb led protocol tests passed");
    return 0;
}
