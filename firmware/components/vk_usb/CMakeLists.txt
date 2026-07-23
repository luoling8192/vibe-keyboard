idf_component_register(
    SRCS "vk_usb.c" "vk_usb_service.c" "vk_usb_json.c" "vk_usb_capabilities.c" "vk_usb_asset_protocol.c" "vk_usb_led_protocol.c" "vk_usb_runtime.c" "vk_usb_owner.c" "vk_usb_facade.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_timer esp_app_format esp_hw_support
)
