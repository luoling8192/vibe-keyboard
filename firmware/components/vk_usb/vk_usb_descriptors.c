#include "vk_usb_descriptors.h"

#include <string.h>

#include "tusb.h"
#include "uac_descriptors.h"

#define VK_USB_VENDOR_ID 0x303a
#define VK_USB_PRODUCT_ID 0x1001

enum {
    ITF_NUM_CDC_CONTROL = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_AUDIO_CONTROL,
    ITF_NUM_AUDIO_MIC,
    ITF_NUM_TOTAL,
};

enum {
    STR_LANGUAGE = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC,
    STR_AUDIO_CONTROL,
    STR_AUDIO_MIC,
};

static char s_serial[32] = "000000000000";

void vk_usb_descriptors_set_serial(const char *serial)
{
    if (serial == NULL) return;
    size_t length = strnlen(serial, sizeof(s_serial));
    if (length == 0U || length >= sizeof(s_serial)) return;
    memcpy(s_serial, serial, length + 1U);
}

static tusb_desc_device_t const s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = VK_USB_VENDOR_ID,
    .idProduct = VK_USB_PRODUCT_ID,
    .bcdDevice = 0x0200,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_device_descriptor;
}

#define VK_USB_CONFIGURATION_LENGTH \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN)

static uint8_t const s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, VK_USB_CONFIGURATION_LENGTH,
                          0x00, 500),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CONTROL, STR_CDC, 0x81, 8, 0x02, 0x82,
                       64),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STR_AUDIO_CONTROL, 0x00, 0x83,
                         0x00),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_configuration_descriptor;
}

static char const *s_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "VibeBoard",
    "VibeBoard Microphone + Control",
    s_serial,
    "VibeBoard Control",
    "VibeBoard Audio",
    "VibeBoard Microphone",
};

static uint16_t s_string_descriptor[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language_id)
{
    (void)language_id;
    if (index >= sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0])) {
        return NULL;
    }
    uint8_t count;
    if (index == STR_LANGUAGE) {
        memcpy(&s_string_descriptor[1], s_string_descriptors[0], 2);
        count = 1;
    } else {
        const char *string = s_string_descriptors[index];
        size_t length = strlen(string);
        if (length > 31U) length = 31U;
        count = (uint8_t)length;
        for (uint8_t position = 0; position < count; ++position) {
            s_string_descriptor[1 + position] = (uint8_t)string[position];
        }
    }
    s_string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8U) |
                                        (2U * count + 2U));
    return s_string_descriptor;
}

_Static_assert(ITF_NUM_AUDIO_MIC == VK_USB_AUDIO_MIC_INTERFACE,
               "UAC runtime and descriptor interface numbers must match");
_Static_assert(sizeof(s_configuration_descriptor) == VK_USB_CONFIGURATION_LENGTH,
               "composite USB descriptor length mismatch");
