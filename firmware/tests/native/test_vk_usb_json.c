#include "vk_usb_json.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vk_usb_json_document_t document;

static vk_usb_json_status_t parse_text(const char *text)
{
    return vk_usb_json_parse(&document, (const uint8_t *)text, strlen(text));
}

static char *nested_arrays(size_t depth, size_t total_bytes)
{
    size_t minimum = depth * 2U + 1U;
    assert(total_bytes >= minimum);
    char *text = malloc(total_bytes + 1U);
    assert(text != NULL);
    memset(text, '[', depth);
    text[depth] = '0';
    memset(text + depth + 1U, ']', depth);
    memset(text + minimum, ' ', total_bytes - minimum);
    text[total_bytes] = '\0';
    return text;
}

static char *array_tokens(size_t values)
{
    size_t length = values == 0U ? 2U : values * 2U + 1U;
    char *text = malloc(length + 1U);
    assert(text != NULL);
    size_t offset = 0U;
    text[offset++] = '[';
    for (size_t index = 0U; index < values; ++index) {
        if (index != 0U) text[offset++] = ',';
        text[offset++] = '0';
    }
    text[offset++] = ']';
    text[offset] = '\0';
    return text;
}

static char *quoted_string(size_t bytes)
{
    char *text = malloc(bytes + 3U);
    assert(text != NULL);
    text[0] = '"';
    memset(text + 1U, 'a', bytes);
    text[bytes + 1U] = '"';
    text[bytes + 2U] = '\0';
    return text;
}

static void test_depth_and_bytes(void)
{
    char *text = nested_arrays(11U, 23U);
    assert(parse_text(text) == VK_USB_JSON_OK);
    free(text);
    text = nested_arrays(12U, 25U);
    assert(parse_text(text) == VK_USB_JSON_OK);
    free(text);
    text = nested_arrays(13U, 27U);
    assert(parse_text(text) == VK_USB_JSON_ERROR_DEPTH);
    free(text);

    text = nested_arrays(1900U, VK_USB_JSON_MAX_BYTES);
    assert(parse_text(text) == VK_USB_JSON_ERROR_DEPTH);
    free(text);
    text = nested_arrays(1900U, VK_USB_JSON_MAX_BYTES + 1U);
    assert(parse_text(text) == VK_USB_JSON_ERROR_BYTES);
    free(text);

    text = malloc(VK_USB_JSON_MAX_BYTES + 2U);
    assert(text != NULL);
    memcpy(text, "null", 4U);
    memset(text + 4U, ' ', VK_USB_JSON_MAX_BYTES - 4U);
    text[VK_USB_JSON_MAX_BYTES] = '\0';
    assert(parse_text(text) == VK_USB_JSON_OK);
    text[VK_USB_JSON_MAX_BYTES] = ' ';
    text[VK_USB_JSON_MAX_BYTES + 1U] = '\0';
    assert(parse_text(text) == VK_USB_JSON_ERROR_BYTES);
    free(text);
}

static void test_tokens(void)
{
    char *text = array_tokens(1023U); /* root + values = 1024 */
    assert(parse_text(text) == VK_USB_JSON_OK);
    free(text);
    text = array_tokens(1024U);
    assert(parse_text(text) == VK_USB_JSON_ERROR_TOKENS);
    free(text);
    assert(parse_text("{\"a\":0}") == VK_USB_JSON_OK);
    assert(document.node_count == 3U); /* object + key + value */
}

static void test_strings_and_unicode(void)
{
    char *text = quoted_string(511U);
    assert(parse_text(text) == VK_USB_JSON_OK);
    free(text);
    text = quoted_string(512U);
    assert(parse_text(text) == VK_USB_JSON_OK);
    free(text);
    text = quoted_string(513U);
    assert(parse_text(text) == VK_USB_JSON_ERROR_STRING);
    free(text);

    assert(parse_text("{\"a\":1,\"\\u0061\":2}") == VK_USB_JSON_ERROR_INVALID);
    assert(parse_text("\"\\u0000\"") == VK_USB_JSON_ERROR_INVALID);
    assert(parse_text("{\"future\":{\"value\":\"\\u0000\"}}") == VK_USB_JSON_ERROR_INVALID);
    assert(parse_text("{\"\\u0000\":1}") == VK_USB_JSON_ERROR_INVALID);
    const uint8_t raw_zero_value[] = {'"', 0x00U, '"'};
    assert(vk_usb_json_parse(&document, raw_zero_value, sizeof(raw_zero_value)) == VK_USB_JSON_ERROR_INVALID);
    const uint8_t raw_zero_key[] = {'{', '"', 0x00U, '"', ':', '0', '}'};
    assert(vk_usb_json_parse(&document, raw_zero_key, sizeof(raw_zero_key)) == VK_USB_JSON_ERROR_INVALID);
    assert(parse_text("\"\\ud800\"") == VK_USB_JSON_ERROR_INVALID);
    assert(parse_text("\"\\udc00\"") == VK_USB_JSON_ERROR_INVALID);
    assert(parse_text("\"\\ud83d\\ude00\"") == VK_USB_JSON_OK);
    const uint8_t invalid_utf8[] = {'"', 0xc0U, 0x80U, '"'};
    assert(vk_usb_json_parse(&document, invalid_utf8, sizeof(invalid_utf8)) == VK_USB_JSON_ERROR_INVALID);
}

static void test_types_and_structure(void)
{
    assert(parse_text("{\"future\":[true,false,null,{\"x\":-1.5e+2}]}") == VK_USB_JSON_OK);
    uint16_t root = vk_usb_json_root(&document);
    uint16_t future = vk_usb_json_object_find(&document, root, "future");
    assert(future != VK_USB_JSON_NO_NODE);
    assert(vk_usb_json_kind(&document, future) == VK_USB_JSON_ARRAY);

    const char *invalid[] = {"", "[", "{\"a\":", "[0,]", "{\"a\":0,}", "true false", "01", "+1", "\"unterminated"};
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) assert(parse_text(invalid[index]) == VK_USB_JSON_ERROR_INVALID);
}

static void check_uint(const char *lexeme, uint64_t maximum, bool nonzero, vk_usb_json_status_t expected, uint64_t expected_value)
{
    assert(parse_text(lexeme) == VK_USB_JSON_OK || expected == VK_USB_JSON_ERROR_INVALID);
    if (document.root == VK_USB_JSON_NO_NODE) return;
    uint64_t value = UINT64_MAX;
    vk_usb_json_status_t status = vk_usb_json_uint64(&document, document.root, maximum, nonzero, &value);
    assert(status == expected);
    if (status == VK_USB_JSON_OK) assert(value == expected_value);
}

static void test_canonical_unsigned(void)
{
    check_uint("0", UINT64_MAX, false, VK_USB_JSON_OK, 0U);
    check_uint("1", UINT64_MAX, true, VK_USB_JSON_OK, 1U);
    check_uint("4294967295", UINT32_MAX, false, VK_USB_JSON_OK, UINT32_MAX);
    check_uint("4294967296", UINT32_MAX, false, VK_USB_JSON_ERROR_OVERFLOW, 0U);
    check_uint("18446744073709551615", UINT64_MAX, false, VK_USB_JSON_OK, UINT64_MAX);
    check_uint("18446744073709551616", UINT64_MAX, false, VK_USB_JSON_ERROR_OVERFLOW, 0U);
    check_uint("99999999999999999999999999999999999999999999999999", UINT64_MAX, false, VK_USB_JSON_ERROR_OVERFLOW, 0U);
    check_uint("-0", UINT64_MAX, false, VK_USB_JSON_ERROR_INVALID, 0U);
    check_uint("1.0", UINT64_MAX, false, VK_USB_JSON_ERROR_INVALID, 0U);
    check_uint("1e0", UINT64_MAX, false, VK_USB_JSON_ERROR_INVALID, 0U);
    check_uint("true", UINT64_MAX, false, VK_USB_JSON_ERROR_INVALID, 0U);
    assert(parse_text("00") == VK_USB_JSON_ERROR_INVALID);
    const uint8_t unicode_digits[] = {'"', 0xd9U, 0xa1U, '"'};
    assert(vk_usb_json_parse(&document, unicode_digits, sizeof(unicode_digits)) == VK_USB_JSON_OK);
    uint64_t value;
    assert(vk_usb_json_uint64(&document, document.root, UINT64_MAX, false, &value) == VK_USB_JSON_ERROR_INVALID);
}

int main(void)
{
    test_depth_and_bytes();
    test_tokens();
    test_strings_and_unicode();
    test_types_and_structure();
    test_canonical_unsigned();
    puts("vk_usb_json adversarial tests passed");
    return 0;
}
