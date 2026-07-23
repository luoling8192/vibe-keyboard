#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { VK_VKA1_OK = 0, VK_VKA1_INVALID, VK_VKA1_LIMIT, VK_VKA1_HASH, VK_VKA1_RANGE, VK_VKA1_RLE } vk_vka1_result_t;
typedef enum { VK_VKA1_IMAGE = 1, VK_VKA1_ANIMATION = 2, VK_VKA1_GLYPH_BITMAP = 3 } vk_vka1_kind_t;
typedef enum { VK_VKA1_RAW = 0, VK_VKA1_ROW_RLE = 1 } vk_vka1_encoding_t;

typedef struct {
    uint16_t max_frames;
    uint16_t min_frame_ms;
    uint16_t max_frame_ms;
    uint32_t max_container_bytes;
    uint32_t max_decoded_bytes;
} vk_vka1_limits_t;

typedef struct {
    vk_vka1_kind_t kind;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint32_t decoded_bytes_per_frame;
    uint8_t sha256[32];
} vk_vka1_info_t;

typedef struct {
    uint32_t offset;
    uint32_t encoded_length;
    uint16_t duration_ms;
    vk_vka1_encoding_t encoding;
} vk_vka1_frame_info_t;

vk_vka1_result_t vk_vka1_validate(const uint8_t *bytes, size_t length, const vk_vka1_limits_t *limits, vk_vka1_info_t *info);
vk_vka1_result_t vk_vka1_frame_info(const uint8_t *bytes, size_t length, uint16_t frame_index, vk_vka1_frame_info_t *frame);
vk_vka1_result_t vk_vka1_decode_frame(const uint8_t *bytes, size_t length, uint16_t frame_index, uint16_t *pixels, size_t pixel_capacity);

#ifdef __cplusplus
}
#endif
