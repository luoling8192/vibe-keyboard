idf_component_register(
    SRCS "vk_input.c" "vk_input_debounce.c"
    INCLUDE_DIRS "include"
    REQUIRES vk_audio vk_board vk_usb
    PRIV_REQUIRES esp_driver_gpio esp_timer
)
