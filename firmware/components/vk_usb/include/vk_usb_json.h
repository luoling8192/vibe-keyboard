#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_USB_JSON_MAX_BYTES 4092U
#define VK_USB_JSON_MAX_DEPTH 12U
#define VK_USB_JSON_MAX_TOKENS 1024U
#define VK_USB_JSON_MAX_STRING_BYTES 512U
#define VK_USB_JSON_NO_NODE UINT16_MAX

typedef enum {
    VK_USB_JSON_OK = 0,
    VK_USB_JSON_ERROR_BYTES,
    VK_USB_JSON_ERROR_DEPTH,
    VK_USB_JSON_ERROR_TOKENS,
    VK_USB_JSON_ERROR_STRING,
    VK_USB_JSON_ERROR_INVALID,
    VK_USB_JSON_ERROR_OVERFLOW,
} vk_usb_json_status_t;

typedef enum {
    VK_USB_JSON_OBJECT = 0,
    VK_USB_JSON_ARRAY,
    VK_USB_JSON_STRING,
    VK_USB_JSON_NUMBER,
    VK_USB_JSON_BOOLEAN,
    VK_USB_JSON_NULL,
} vk_usb_json_kind_t;

typedef struct {
    uint16_t start;
    uint16_t end;
    uint16_t parent;
    uint16_t first_child;
    uint16_t next_sibling;
    vk_usb_json_kind_t kind;
    bool object_key;
} vk_usb_json_node_t;

/* Caller-owned fixed workspace. Keep this object in service/heap state, not a small task stack. */
typedef struct {
    const uint8_t *bytes;
    size_t length;
    uint16_t root;
    uint16_t node_count;
    vk_usb_json_node_t nodes[VK_USB_JSON_MAX_TOKENS];
    uint8_t scratch[(VK_USB_JSON_MAX_STRING_BYTES + 1U) * 2U];
} vk_usb_json_document_t;

vk_usb_json_status_t vk_usb_json_parse(vk_usb_json_document_t *document,
                                        const uint8_t *bytes,
                                        size_t length);
uint16_t vk_usb_json_root(const vk_usb_json_document_t *document);
vk_usb_json_kind_t vk_usb_json_kind(const vk_usb_json_document_t *document, uint16_t node);
uint16_t vk_usb_json_object_find(const vk_usb_json_document_t *document,
                                 uint16_t object,
                                 const char *ascii_key);
bool vk_usb_json_object_exact_keys(const vk_usb_json_document_t *document,
                                   uint16_t object,
                                   const char *const *ascii_keys,
                                   size_t key_count);
bool vk_usb_json_string_equal(const vk_usb_json_document_t *document,
                              uint16_t node,
                              const char *utf8_value);
vk_usb_json_status_t vk_usb_json_string_copy(const vk_usb_json_document_t *document,
                                             uint16_t node,
                                             char *destination,
                                             size_t capacity);
vk_usb_json_status_t vk_usb_json_uint64(const vk_usb_json_document_t *document,
                                        uint16_t node,
                                        uint64_t maximum,
                                        bool nonzero,
                                        uint64_t *value);
vk_usb_json_status_t vk_usb_json_uint32(const vk_usb_json_document_t *document,
                                        uint16_t node,
                                        uint32_t maximum,
                                        bool nonzero,
                                        uint32_t *value);
vk_usb_json_status_t vk_usb_json_int64(const vk_usb_json_document_t *document,
                                       uint16_t node,
                                       int64_t minimum,
                                       int64_t maximum,
                                       int64_t *value);
bool vk_usb_json_boolean(const vk_usb_json_document_t *document,
                         uint16_t node,
                         bool *value);
bool vk_usb_json_is_null(const vk_usb_json_document_t *document, uint16_t node);
uint16_t vk_usb_json_first_child(const vk_usb_json_document_t *document, uint16_t node);
uint16_t vk_usb_json_next_sibling(const vk_usb_json_document_t *document, uint16_t node);
bool vk_usb_json_node_is_object_key(const vk_usb_json_document_t *document, uint16_t node);
size_t vk_usb_json_array_count(const vk_usb_json_document_t *document, uint16_t array);
bool vk_usb_json_node_range(const vk_usb_json_document_t *document,
                            uint16_t node,
                            size_t *start,
                            size_t *end);

#ifdef __cplusplus
}
#endif
