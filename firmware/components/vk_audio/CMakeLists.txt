idf_component_register(
    SRCS "vk_audio.c" "vk_audio_pipeline.c" "vk_audio_runtime.c" "vk_audio_backend.c" "vk_audio_dependency_probe.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-sr micro-opus vk_board vk_usb
    PRIV_REQUIRES esp_driver_i2s esp_mm
)
