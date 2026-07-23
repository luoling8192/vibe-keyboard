idf_component_register(
    SRCS "vk_screen.c" "vk_screen_lvgl.c" "vk_screen_service.c" "vk_screen_product.c"
    INCLUDE_DIRS "include"
    REQUIRES vk_usb vk_vka1 vk_assets vk_asset_transfer vk_display vk_board lvgl__lvgl espressif__esp_lvgl_port
    PRIV_REQUIRES esp_timer
)
