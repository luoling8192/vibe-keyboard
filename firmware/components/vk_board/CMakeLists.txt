idf_component_register(
    SRCS "vk_board.c" "vk_board_lifecycle.c" "vk_board_runtime.c" "vk_nv3007.c" "vk_nv3007_policy.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_ledc esp_timer esp_lcd
    REQUIRES esp_lvgl_port led_strip lvgl
)
