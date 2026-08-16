idf_component_register(
    SRCS "vk_usb.c" "vk_usb_tinyusb.c" "vk_usb_descriptors.c" "vk_usb_service.c" "vk_usb_json.c" "vk_usb_capabilities.c" "vk_usb_asset_protocol.c" "vk_usb_led_protocol.c" "vk_usb_runtime.c" "vk_usb_owner.c" "vk_usb_facade.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_app_format esp_hw_support usb
        espressif__tinyusb espressif__usb_device_uac
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_component tinyusb)
else()
    set(tinyusb_component espressif__tinyusb)
endif()
if(usb_device_uac IN_LIST build_components)
    set(uac_component usb_device_uac)
else()
    set(uac_component espressif__usb_device_uac)
endif()

idf_component_get_property(tinyusb_library ${tinyusb_component} COMPONENT_LIB)
target_include_directories(${tinyusb_library} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
idf_component_get_property(uac_library ${uac_component} COMPONENT_LIB)
target_include_directories(${uac_library} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
