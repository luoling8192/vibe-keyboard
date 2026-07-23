#include "vk_usb_json.h"

#include <limits.h>
#include <string.h>

typedef struct {
    vk_usb_json_document_t *document;
    size_t offset;
} parser_t;

static void skip_space(parser_t *parser)
{
    while (parser->offset < parser->document->length) {
        uint8_t byte = parser->document->bytes[parser->offset];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') break;
        ++parser->offset;
    }
}

static int hex_value(uint8_t byte)
{
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'f') return (int)(byte - 'a' + 10U);
    if (byte >= 'A' && byte <= 'F') return (int)(byte - 'A' + 10U);
    return -1;
}

static bool parse_hex4(const uint8_t *bytes, size_t length, size_t *offset, uint32_t *value)
{
    if (*offset > length || length - *offset < 4U) return false;
    uint32_t result = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        int digit = hex_value(bytes[(*offset)++]);
        if (digit < 0) return false;
        result = (result << 4U) | (uint32_t)digit;
    }
    *value = result;
    return true;
}

static bool decode_utf8_scalar(const uint8_t *bytes, size_t length, size_t *offset, uint32_t *scalar)
{
    if (*offset >= length) return false;
    uint8_t first = bytes[(*offset)++];
    if (first < 0x80U) { *scalar = first; return true; }
    size_t continuation;
    uint32_t value;
    uint32_t minimum;
    if (first >= 0xc2U && first <= 0xdfU) { continuation = 1U; value = first & 0x1fU; minimum = 0x80U; }
    else if (first >= 0xe0U && first <= 0xefU) { continuation = 2U; value = first & 0x0fU; minimum = 0x800U; }
    else if (first >= 0xf0U && first <= 0xf4U) { continuation = 3U; value = first & 0x07U; minimum = 0x10000U; }
    else return false;
    if (*offset > length || length - *offset < continuation) return false;
    for (size_t index = 0U; index < continuation; ++index) {
        uint8_t byte = bytes[(*offset)++];
        if ((byte & 0xc0U) != 0x80U) return false;
        value = (value << 6U) | (uint32_t)(byte & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) return false;
    *scalar = value;
    return true;
}

static size_t encoded_length(uint32_t scalar)
{
    if (scalar <= 0x7fU) return 1U;
    if (scalar <= 0x7ffU) return 2U;
    if (scalar <= 0xffffU) return 3U;
    return 4U;
}

static bool append_scalar(uint8_t *output, size_t capacity, size_t *count, uint32_t scalar)
{
    size_t length = encoded_length(scalar);
    if (*count > capacity || capacity - *count < length) return false;
    if (length == 1U) output[(*count)++] = (uint8_t)scalar;
    else if (length == 2U) {
        output[(*count)++] = (uint8_t)(0xc0U | (scalar >> 6U));
        output[(*count)++] = (uint8_t)(0x80U | (scalar & 0x3fU));
    } else if (length == 3U) {
        output[(*count)++] = (uint8_t)(0xe0U | (scalar >> 12U));
        output[(*count)++] = (uint8_t)(0x80U | ((scalar >> 6U) & 0x3fU));
        output[(*count)++] = (uint8_t)(0x80U | (scalar & 0x3fU));
    } else {
        output[(*count)++] = (uint8_t)(0xf0U | (scalar >> 18U));
        output[(*count)++] = (uint8_t)(0x80U | ((scalar >> 12U) & 0x3fU));
        output[(*count)++] = (uint8_t)(0x80U | ((scalar >> 6U) & 0x3fU));
        output[(*count)++] = (uint8_t)(0x80U | (scalar & 0x3fU));
    }
    return true;
}

static vk_usb_json_status_t decode_string_range(const uint8_t *bytes,
                                                 size_t start,
                                                 size_t end,
                                                 uint8_t *output,
                                                 size_t capacity,
                                                 size_t *decoded_length)
{
    if (start >= end || bytes[start] != '"' || bytes[end - 1U] != '"') return VK_USB_JSON_ERROR_INVALID;
    size_t offset = start + 1U;
    size_t count = 0U;
    while (offset < end - 1U) {
        uint8_t byte = bytes[offset];
        uint32_t scalar;
        if (byte < 0x20U) return VK_USB_JSON_ERROR_INVALID;
        if (byte == '\\') {
            ++offset;
            if (offset >= end - 1U) return VK_USB_JSON_ERROR_INVALID;
            uint8_t escaped = bytes[offset++];
            switch (escaped) {
                case '"': scalar = '"'; break;
                case '\\': scalar = '\\'; break;
                case '/': scalar = '/'; break;
                case 'b': scalar = '\b'; break;
                case 'f': scalar = '\f'; break;
                case 'n': scalar = '\n'; break;
                case 'r': scalar = '\r'; break;
                case 't': scalar = '\t'; break;
                case 'u': {
                    uint32_t first;
                    if (!parse_hex4(bytes, end - 1U, &offset, &first)) return VK_USB_JSON_ERROR_INVALID;
                    if (first >= 0xd800U && first <= 0xdbffU) {
                        if (offset > end - 1U || end - 1U - offset < 6U || bytes[offset++] != '\\' || bytes[offset++] != 'u') return VK_USB_JSON_ERROR_INVALID;
                        uint32_t second;
                        if (!parse_hex4(bytes, end - 1U, &offset, &second) || second < 0xdc00U || second > 0xdfffU) return VK_USB_JSON_ERROR_INVALID;
                        scalar = 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
                    } else {
                        if (first >= 0xdc00U && first <= 0xdfffU) return VK_USB_JSON_ERROR_INVALID;
                        scalar = first;
                    }
                    break;
                }
                default: return VK_USB_JSON_ERROR_INVALID;
            }
        } else if (!decode_utf8_scalar(bytes, end - 1U, &offset, &scalar)) {
            return VK_USB_JSON_ERROR_INVALID;
        }
        if (scalar == 0U) return VK_USB_JSON_ERROR_INVALID;
        size_t scalar_length = encoded_length(scalar);
        if (count > VK_USB_JSON_MAX_STRING_BYTES || VK_USB_JSON_MAX_STRING_BYTES - count < scalar_length) return VK_USB_JSON_ERROR_STRING;
        if (output != NULL && !append_scalar(output, capacity, &count, scalar)) return VK_USB_JSON_ERROR_STRING;
        if (output == NULL) count += scalar_length;
    }
    if (offset != end - 1U) return VK_USB_JSON_ERROR_INVALID;
    *decoded_length = count;
    return VK_USB_JSON_OK;
}

static vk_usb_json_status_t allocate_node(parser_t *parser,
                                           vk_usb_json_kind_t kind,
                                           uint16_t parent,
                                           bool object_key,
                                           uint16_t *node)
{
    if (parser->document->node_count >= VK_USB_JSON_MAX_TOKENS) return VK_USB_JSON_ERROR_TOKENS;
    uint16_t index = parser->document->node_count++;
    parser->document->nodes[index] = (vk_usb_json_node_t){
        .parent = parent,
        .first_child = VK_USB_JSON_NO_NODE,
        .next_sibling = VK_USB_JSON_NO_NODE,
        .kind = kind,
        .object_key = object_key,
    };
    *node = index;
    return VK_USB_JSON_OK;
}

static void append_child(vk_usb_json_document_t *document, uint16_t parent, uint16_t child)
{
    vk_usb_json_node_t *container = &document->nodes[parent];
    if (container->first_child == VK_USB_JSON_NO_NODE) { container->first_child = child; return; }
    uint16_t current = container->first_child;
    while (document->nodes[current].next_sibling != VK_USB_JSON_NO_NODE) current = document->nodes[current].next_sibling;
    document->nodes[current].next_sibling = child;
}

static vk_usb_json_status_t parse_string_node(parser_t *parser,
                                               uint16_t parent,
                                               bool object_key,
                                               uint16_t *node)
{
    size_t start = parser->offset;
    if (start >= parser->document->length || parser->document->bytes[start] != '"') return VK_USB_JSON_ERROR_INVALID;
    ++parser->offset;
    bool escaped = false;
    while (parser->offset < parser->document->length) {
        uint8_t byte = parser->document->bytes[parser->offset++];
        if (!escaped && byte == '"') break;
        if (!escaped && byte == '\\') escaped = true;
        else escaped = false;
    }
    if (parser->offset <= start + 1U || parser->document->bytes[parser->offset - 1U] != '"') return VK_USB_JSON_ERROR_INVALID;
    size_t decoded = 0U;
    vk_usb_json_status_t status = decode_string_range(parser->document->bytes, start, parser->offset, NULL, 0U, &decoded);
    if (status != VK_USB_JSON_OK) return status;
    status = allocate_node(parser, VK_USB_JSON_STRING, parent, object_key, node);
    if (status != VK_USB_JSON_OK) return status;
    parser->document->nodes[*node].start = (uint16_t)start;
    parser->document->nodes[*node].end = (uint16_t)parser->offset;
    return VK_USB_JSON_OK;
}

static vk_usb_json_status_t decode_node(const vk_usb_json_document_t *document,
                                        uint16_t node,
                                        uint8_t *output,
                                        size_t capacity,
                                        size_t *length)
{
    if (node >= document->node_count || document->nodes[node].kind != VK_USB_JSON_STRING) return VK_USB_JSON_ERROR_INVALID;
    return decode_string_range(document->bytes, document->nodes[node].start, document->nodes[node].end, output, capacity, length);
}

static vk_usb_json_status_t check_duplicate_key(vk_usb_json_document_t *document,
                                                 uint16_t object,
                                                 uint16_t candidate)
{
    size_t candidate_length = 0U;
    vk_usb_json_status_t status = decode_node(document, candidate, document->scratch, VK_USB_JSON_MAX_STRING_BYTES + 1U, &candidate_length);
    if (status != VK_USB_JSON_OK) return status;
    uint16_t child = document->nodes[object].first_child;
    while (child != VK_USB_JSON_NO_NODE && child != candidate) {
        if (document->nodes[child].object_key) {
            size_t previous_length = 0U;
            status = decode_node(document, child, document->scratch + VK_USB_JSON_MAX_STRING_BYTES + 1U, VK_USB_JSON_MAX_STRING_BYTES + 1U, &previous_length);
            if (status != VK_USB_JSON_OK) return status;
            if (candidate_length == previous_length && memcmp(document->scratch, document->scratch + VK_USB_JSON_MAX_STRING_BYTES + 1U, candidate_length) == 0) return VK_USB_JSON_ERROR_INVALID;
        }
        child = document->nodes[child].next_sibling;
    }
    return VK_USB_JSON_OK;
}

static vk_usb_json_status_t parse_value(parser_t *parser, uint16_t parent, uint8_t depth, uint16_t *node);

static vk_usb_json_status_t parse_object(parser_t *parser, uint16_t parent, uint8_t depth, uint16_t *node)
{
    vk_usb_json_status_t status = allocate_node(parser, VK_USB_JSON_OBJECT, parent, false, node);
    if (status != VK_USB_JSON_OK) return status;
    parser->document->nodes[*node].start = (uint16_t)parser->offset++;
    skip_space(parser);
    if (parser->offset < parser->document->length && parser->document->bytes[parser->offset] == '}') {
        parser->document->nodes[*node].end = (uint16_t)++parser->offset;
        return VK_USB_JSON_OK;
    }
    while (parser->offset < parser->document->length) {
        uint16_t key;
        status = parse_string_node(parser, *node, true, &key);
        if (status != VK_USB_JSON_OK) return status;
        append_child(parser->document, *node, key);
        status = check_duplicate_key(parser->document, *node, key);
        if (status != VK_USB_JSON_OK) return status;
        skip_space(parser);
        if (parser->offset >= parser->document->length || parser->document->bytes[parser->offset++] != ':') return VK_USB_JSON_ERROR_INVALID;
        skip_space(parser);
        uint16_t value;
        status = parse_value(parser, *node, (uint8_t)(depth + 1U), &value);
        if (status != VK_USB_JSON_OK) return status;
        append_child(parser->document, *node, value);
        skip_space(parser);
        if (parser->offset >= parser->document->length) return VK_USB_JSON_ERROR_INVALID;
        if (parser->document->bytes[parser->offset] == '}') {
            parser->document->nodes[*node].end = (uint16_t)++parser->offset;
            return VK_USB_JSON_OK;
        }
        if (parser->document->bytes[parser->offset++] != ',') return VK_USB_JSON_ERROR_INVALID;
        skip_space(parser);
    }
    return VK_USB_JSON_ERROR_INVALID;
}

static vk_usb_json_status_t parse_array(parser_t *parser, uint16_t parent, uint8_t depth, uint16_t *node)
{
    vk_usb_json_status_t status = allocate_node(parser, VK_USB_JSON_ARRAY, parent, false, node);
    if (status != VK_USB_JSON_OK) return status;
    parser->document->nodes[*node].start = (uint16_t)parser->offset++;
    skip_space(parser);
    if (parser->offset < parser->document->length && parser->document->bytes[parser->offset] == ']') {
        parser->document->nodes[*node].end = (uint16_t)++parser->offset;
        return VK_USB_JSON_OK;
    }
    while (parser->offset < parser->document->length) {
        uint16_t value;
        status = parse_value(parser, *node, (uint8_t)(depth + 1U), &value);
        if (status != VK_USB_JSON_OK) return status;
        append_child(parser->document, *node, value);
        skip_space(parser);
        if (parser->offset >= parser->document->length) return VK_USB_JSON_ERROR_INVALID;
        if (parser->document->bytes[parser->offset] == ']') {
            parser->document->nodes[*node].end = (uint16_t)++parser->offset;
            return VK_USB_JSON_OK;
        }
        if (parser->document->bytes[parser->offset++] != ',') return VK_USB_JSON_ERROR_INVALID;
        skip_space(parser);
    }
    return VK_USB_JSON_ERROR_INVALID;
}

static bool is_delimiter(const vk_usb_json_document_t *document, size_t offset)
{
    if (offset >= document->length) return true;
    uint8_t byte = document->bytes[offset];
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == ',' || byte == ']' || byte == '}';
}

static vk_usb_json_status_t parse_number(parser_t *parser, uint16_t parent, uint16_t *node)
{
    size_t start = parser->offset;
    const uint8_t *bytes = parser->document->bytes;
    size_t length = parser->document->length;
    if (parser->offset < length && bytes[parser->offset] == '-') ++parser->offset;
    if (parser->offset >= length) return VK_USB_JSON_ERROR_INVALID;
    if (bytes[parser->offset] == '0') {
        ++parser->offset;
        if (parser->offset < length && bytes[parser->offset] >= '0' && bytes[parser->offset] <= '9') return VK_USB_JSON_ERROR_INVALID;
    } else {
        if (bytes[parser->offset] < '1' || bytes[parser->offset] > '9') return VK_USB_JSON_ERROR_INVALID;
        do { ++parser->offset; } while (parser->offset < length && bytes[parser->offset] >= '0' && bytes[parser->offset] <= '9');
    }
    if (parser->offset < length && bytes[parser->offset] == '.') {
        ++parser->offset;
        size_t fraction = parser->offset;
        while (parser->offset < length && bytes[parser->offset] >= '0' && bytes[parser->offset] <= '9') ++parser->offset;
        if (parser->offset == fraction) return VK_USB_JSON_ERROR_INVALID;
    }
    if (parser->offset < length && (bytes[parser->offset] == 'e' || bytes[parser->offset] == 'E')) {
        ++parser->offset;
        if (parser->offset < length && (bytes[parser->offset] == '+' || bytes[parser->offset] == '-')) ++parser->offset;
        size_t exponent = parser->offset;
        while (parser->offset < length && bytes[parser->offset] >= '0' && bytes[parser->offset] <= '9') ++parser->offset;
        if (parser->offset == exponent) return VK_USB_JSON_ERROR_INVALID;
    }
    if (!is_delimiter(parser->document, parser->offset)) return VK_USB_JSON_ERROR_INVALID;
    vk_usb_json_status_t status = allocate_node(parser, VK_USB_JSON_NUMBER, parent, false, node);
    if (status != VK_USB_JSON_OK) return status;
    parser->document->nodes[*node].start = (uint16_t)start;
    parser->document->nodes[*node].end = (uint16_t)parser->offset;
    return VK_USB_JSON_OK;
}

static bool consume_literal(parser_t *parser, const char *literal)
{
    size_t count = strlen(literal);
    if (parser->offset > parser->document->length || parser->document->length - parser->offset < count) return false;
    if (memcmp(parser->document->bytes + parser->offset, literal, count) != 0) return false;
    parser->offset += count;
    return is_delimiter(parser->document, parser->offset);
}

static vk_usb_json_status_t parse_value(parser_t *parser, uint16_t parent, uint8_t depth, uint16_t *node)
{
    if (depth > VK_USB_JSON_MAX_DEPTH) return VK_USB_JSON_ERROR_DEPTH;
    skip_space(parser);
    if (parser->offset >= parser->document->length) return VK_USB_JSON_ERROR_INVALID;
    uint8_t byte = parser->document->bytes[parser->offset];
    if (byte == '{') return parse_object(parser, parent, depth, node);
    if (byte == '[') return parse_array(parser, parent, depth, node);
    if (byte == '"') return parse_string_node(parser, parent, false, node);
    size_t start = parser->offset;
    vk_usb_json_kind_t kind;
    if (byte == 't') { if (!consume_literal(parser, "true")) return VK_USB_JSON_ERROR_INVALID; kind = VK_USB_JSON_BOOLEAN; }
    else if (byte == 'f') { if (!consume_literal(parser, "false")) return VK_USB_JSON_ERROR_INVALID; kind = VK_USB_JSON_BOOLEAN; }
    else if (byte == 'n') { if (!consume_literal(parser, "null")) return VK_USB_JSON_ERROR_INVALID; kind = VK_USB_JSON_NULL; }
    else return parse_number(parser, parent, node);
    vk_usb_json_status_t status = allocate_node(parser, kind, parent, false, node);
    if (status != VK_USB_JSON_OK) return status;
    parser->document->nodes[*node].start = (uint16_t)start;
    parser->document->nodes[*node].end = (uint16_t)parser->offset;
    return VK_USB_JSON_OK;
}

vk_usb_json_status_t vk_usb_json_parse(vk_usb_json_document_t *document, const uint8_t *bytes, size_t length)
{
    if (document == NULL || (bytes == NULL && length != 0U)) return VK_USB_JSON_ERROR_INVALID;
    memset(document, 0, sizeof(*document));
    document->root = VK_USB_JSON_NO_NODE;
    document->bytes = bytes;
    document->length = length;
    if (length > VK_USB_JSON_MAX_BYTES) return VK_USB_JSON_ERROR_BYTES;
    parser_t parser = {.document = document, .offset = 0U};
    skip_space(&parser);
    uint16_t root;
    vk_usb_json_status_t status = parse_value(&parser, VK_USB_JSON_NO_NODE, 0U, &root);
    if (status != VK_USB_JSON_OK) return status;
    skip_space(&parser);
    if (parser.offset != length) return VK_USB_JSON_ERROR_INVALID;
    document->root = root;
    return VK_USB_JSON_OK;
}

uint16_t vk_usb_json_root(const vk_usb_json_document_t *document) { return document == NULL ? VK_USB_JSON_NO_NODE : document->root; }

vk_usb_json_kind_t vk_usb_json_kind(const vk_usb_json_document_t *document, uint16_t node)
{
    return document != NULL && node < document->node_count ? document->nodes[node].kind : VK_USB_JSON_NULL;
}

uint16_t vk_usb_json_object_find(const vk_usb_json_document_t *document, uint16_t object, const char *ascii_key)
{
    if (document == NULL || ascii_key == NULL || object >= document->node_count || document->nodes[object].kind != VK_USB_JSON_OBJECT) return VK_USB_JSON_NO_NODE;
    size_t key_length = strlen(ascii_key);
    if (key_length > VK_USB_JSON_MAX_STRING_BYTES) return VK_USB_JSON_NO_NODE;
    uint16_t child = document->nodes[object].first_child;
    while (child != VK_USB_JSON_NO_NODE) {
        if (!document->nodes[child].object_key) return VK_USB_JSON_NO_NODE;
        size_t decoded_length = 0U;
        vk_usb_json_document_t *mutable_document = (vk_usb_json_document_t *)(uintptr_t)document;
        if (decode_node(document, child, mutable_document->scratch, VK_USB_JSON_MAX_STRING_BYTES + 1U, &decoded_length) != VK_USB_JSON_OK) return VK_USB_JSON_NO_NODE;
        uint16_t value = document->nodes[child].next_sibling;
        if (value == VK_USB_JSON_NO_NODE) return VK_USB_JSON_NO_NODE;
        if (decoded_length == key_length && memcmp(mutable_document->scratch, ascii_key, key_length) == 0) return value;
        child = document->nodes[value].next_sibling;
    }
    return VK_USB_JSON_NO_NODE;
}

bool vk_usb_json_object_exact_keys(const vk_usb_json_document_t *document, uint16_t object, const char *const *ascii_keys, size_t key_count)
{
    if (document == NULL || object >= document->node_count || document->nodes[object].kind != VK_USB_JSON_OBJECT) return false;
    size_t actual = 0U;
    uint16_t child = document->nodes[object].first_child;
    while (child != VK_USB_JSON_NO_NODE) {
        if (!document->nodes[child].object_key) return false;
        uint16_t value = document->nodes[child].next_sibling;
        if (value == VK_USB_JSON_NO_NODE) return false;
        ++actual;
        child = document->nodes[value].next_sibling;
    }
    if (actual != key_count) return false;
    for (size_t index = 0U; index < key_count; ++index) if (vk_usb_json_object_find(document, object, ascii_keys[index]) == VK_USB_JSON_NO_NODE) return false;
    return true;
}

bool vk_usb_json_string_equal(const vk_usb_json_document_t *document, uint16_t node, const char *utf8_value)
{
    if (document == NULL || utf8_value == NULL) return false;
    vk_usb_json_document_t *mutable_document = (vk_usb_json_document_t *)(uintptr_t)document;
    size_t decoded_length = 0U;
    if (decode_node(document, node, mutable_document->scratch, VK_USB_JSON_MAX_STRING_BYTES + 1U, &decoded_length) != VK_USB_JSON_OK) return false;
    size_t expected = strlen(utf8_value);
    return decoded_length == expected && memcmp(mutable_document->scratch, utf8_value, expected) == 0;
}

vk_usb_json_status_t vk_usb_json_string_copy(const vk_usb_json_document_t *document, uint16_t node, char *destination, size_t capacity)
{
    if (document == NULL || destination == NULL || capacity == 0U) return VK_USB_JSON_ERROR_INVALID;
    size_t decoded_length = 0U;
    vk_usb_json_status_t status = decode_node(document, node, (uint8_t *)destination, capacity - 1U, &decoded_length);
    if (status != VK_USB_JSON_OK) return status;
    if (memchr(destination, '\0', decoded_length) != NULL) return VK_USB_JSON_ERROR_INVALID;
    destination[decoded_length] = '\0';
    return VK_USB_JSON_OK;
}

vk_usb_json_status_t vk_usb_json_uint64(const vk_usb_json_document_t *document, uint16_t node, uint64_t maximum, bool nonzero, uint64_t *value)
{
    if (document == NULL || value == NULL || node >= document->node_count || document->nodes[node].kind != VK_USB_JSON_NUMBER) return VK_USB_JSON_ERROR_INVALID;
    size_t start = document->nodes[node].start;
    size_t end = document->nodes[node].end;
    if (start >= end) return VK_USB_JSON_ERROR_INVALID;
    if (document->bytes[start] == '0') {
        if (end - start != 1U) return VK_USB_JSON_ERROR_INVALID;
        if (nonzero) return VK_USB_JSON_ERROR_INVALID;
        *value = 0U;
        return VK_USB_JSON_OK;
    }
    if (document->bytes[start] < '1' || document->bytes[start] > '9') return VK_USB_JSON_ERROR_INVALID;
    uint64_t result = 0U;
    for (size_t offset = start; offset < end; ++offset) {
        uint8_t byte = document->bytes[offset];
        if (byte < '0' || byte > '9') return VK_USB_JSON_ERROR_INVALID;
        uint64_t digit = (uint64_t)(byte - '0');
        if (result > (UINT64_MAX - digit) / 10U) return VK_USB_JSON_ERROR_OVERFLOW;
        result = result * 10U + digit;
        if (result > maximum) return VK_USB_JSON_ERROR_OVERFLOW;
    }
    *value = result;
    return VK_USB_JSON_OK;
}

vk_usb_json_status_t vk_usb_json_uint32(const vk_usb_json_document_t *document, uint16_t node, uint32_t maximum, bool nonzero, uint32_t *value)
{
    if (value == NULL) return VK_USB_JSON_ERROR_INVALID;
    uint64_t result;
    vk_usb_json_status_t status = vk_usb_json_uint64(document, node, maximum, nonzero, &result);
    if (status == VK_USB_JSON_OK) *value = (uint32_t)result;
    return status;
}

vk_usb_json_status_t vk_usb_json_int64(const vk_usb_json_document_t *document,
                                       uint16_t node,
                                       int64_t minimum,
                                       int64_t maximum,
                                       int64_t *value)
{
    if (document == NULL || value == NULL || minimum > maximum || node >= document->node_count ||
        document->nodes[node].kind != VK_USB_JSON_NUMBER) return VK_USB_JSON_ERROR_INVALID;
    size_t start = document->nodes[node].start;
    size_t end = document->nodes[node].end;
    if (start >= end) return VK_USB_JSON_ERROR_INVALID;
    bool negative = document->bytes[start] == '-';
    if (negative) {
        ++start;
        if (start >= end || document->bytes[start] == '0') return VK_USB_JSON_ERROR_INVALID;
    } else if (document->bytes[start] == '0') {
        if (end - start != 1U || 0 < minimum || 0 > maximum) return VK_USB_JSON_ERROR_INVALID;
        *value = 0;
        return VK_USB_JSON_OK;
    }
    if (document->bytes[start] < '1' || document->bytes[start] > '9') return VK_USB_JSON_ERROR_INVALID;
    uint64_t magnitude = 0U;
    const uint64_t limit = negative ? ((uint64_t)INT64_MAX + 1U) : (uint64_t)INT64_MAX;
    for (size_t offset = start; offset < end; ++offset) {
        uint8_t byte = document->bytes[offset];
        if (byte < '0' || byte > '9') return VK_USB_JSON_ERROR_INVALID;
        uint64_t digit = (uint64_t)(byte - '0');
        if (magnitude > (limit - digit) / 10U) return VK_USB_JSON_ERROR_OVERFLOW;
        magnitude = magnitude * 10U + digit;
    }
    int64_t result = negative ? (magnitude == (uint64_t)INT64_MAX + 1U ? INT64_MIN : -(int64_t)magnitude) : (int64_t)magnitude;
    if (result < minimum || result > maximum) return VK_USB_JSON_ERROR_OVERFLOW;
    *value = result;
    return VK_USB_JSON_OK;
}

bool vk_usb_json_boolean(const vk_usb_json_document_t *document, uint16_t node, bool *value)
{
    if (document == NULL || value == NULL || node >= document->node_count ||
        document->nodes[node].kind != VK_USB_JSON_BOOLEAN) return false;
    size_t start = document->nodes[node].start;
    size_t length = (size_t)document->nodes[node].end - start;
    if (length == 4U && memcmp(document->bytes + start, "true", 4U) == 0) { *value = true; return true; }
    if (length == 5U && memcmp(document->bytes + start, "false", 5U) == 0) { *value = false; return true; }
    return false;
}

bool vk_usb_json_is_null(const vk_usb_json_document_t *document, uint16_t node)
{
    return document != NULL && node < document->node_count && document->nodes[node].kind == VK_USB_JSON_NULL;
}

uint16_t vk_usb_json_first_child(const vk_usb_json_document_t *document, uint16_t node)
{
    return document != NULL && node < document->node_count ? document->nodes[node].first_child : VK_USB_JSON_NO_NODE;
}

uint16_t vk_usb_json_next_sibling(const vk_usb_json_document_t *document, uint16_t node)
{
    return document != NULL && node < document->node_count ? document->nodes[node].next_sibling : VK_USB_JSON_NO_NODE;
}

bool vk_usb_json_node_is_object_key(const vk_usb_json_document_t *document, uint16_t node)
{
    return document != NULL && node < document->node_count && document->nodes[node].object_key;
}

size_t vk_usb_json_array_count(const vk_usb_json_document_t *document, uint16_t array)
{
    if (document == NULL || array >= document->node_count || document->nodes[array].kind != VK_USB_JSON_ARRAY) return 0U;
    size_t count = 0U;
    for (uint16_t child = document->nodes[array].first_child; child != VK_USB_JSON_NO_NODE; child = document->nodes[child].next_sibling) ++count;
    return count;
}

bool vk_usb_json_node_range(const vk_usb_json_document_t *document, uint16_t node, size_t *start, size_t *end)
{
    if (document == NULL || start == NULL || end == NULL || node >= document->node_count) return false;
    *start = document->nodes[node].start;
    *end = document->nodes[node].end;
    return *start <= *end && *end <= document->length;
}
