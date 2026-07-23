idf_component_register(
    SRCS "vk_asset_store.c" "vk_asset_spiffs.c"
    INCLUDE_DIRS "include"
    REQUIRES spiffs mbedtls esp_partition
)
