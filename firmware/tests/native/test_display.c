#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_display.h"

static void test_exact_product_profile(void)
{
    const vk_display_profile_t *profile = vk_display_product_profile();
    assert(vk_display_validate_profile(profile) == ESP_OK);
    assert(profile->width == 428U && profile->height == 142U);
    assert(profile->buffer_lines == 10U && profile->buffer_count == 2U);
    assert(profile->queue_depth == 10U && profile->pixel_clock_hz == 40000000U);
    assert(profile->x_gap == 14 && profile->y_gap == 13);
    assert(profile->rgb565 && profile->swap_bytes && profile->dma_capable);
    assert(profile->psram_enabled && !profile->software_rotate);
    assert(!profile->full_refresh && !profile->direct_mode);

    vk_display_profile_t altered = *profile;
    altered.width = 320U;
    assert(vk_display_validate_profile(&altered) == ESP_ERR_INVALID_ARG);
    altered = *profile;
    altered.swap_bytes = false;
    assert(vk_display_validate_profile(&altered) == ESP_ERR_INVALID_ARG);
}

static void test_transport_and_truthful_screen_gate(void)
{
    vk_display_t display;
    assert(vk_display_start(&display, vk_display_product_profile(), true, true) == ESP_OK);
    assert(vk_display_transport_ready(&display));
    assert(!vk_display_screen_available(&display));

    vk_display_dependencies_t dependencies = {
        .store_ready = true,
        .screen_owner_ready = true,
        .font_profile_ready = true,
        .physical_acceptance_admitted = false,
    };
    assert(vk_display_set_dependencies(&display, &dependencies) == ESP_OK);
    assert(!vk_display_screen_available(&display));
    dependencies.physical_acceptance_admitted = true;
    assert(vk_display_set_dependencies(&display, &dependencies) == ESP_OK);
    assert(vk_display_screen_available(&display));
    dependencies.store_ready = false;
    assert(vk_display_set_dependencies(&display, &dependencies) == ESP_OK);
    assert(!vk_display_screen_available(&display));
    assert(vk_display_stop(&display) == ESP_OK);
}

static void test_flush_bounds_failure_and_stop(void)
{
    vk_display_t display;
    assert(vk_display_start(&display, vk_display_product_profile(), true, true) == ESP_OK);
    assert(vk_display_begin_flush(&display, 0, 0, 428, 10, 8560U) == ESP_OK);
    assert(vk_display_begin_flush(&display, 0, 0, 428, 11, 9416U) == ESP_ERR_INVALID_SIZE);
    assert(vk_display_begin_flush(&display, -1, 0, 1, 1, 4U) == ESP_ERR_INVALID_ARG);
    assert(vk_display_stop(&display) == ESP_ERR_TIMEOUT);
    assert(vk_display_is_tainted(&display));
    assert(!vk_display_screen_available(&display));
    assert(vk_display_complete_flush(&display, ESP_OK) == ESP_OK);
    assert(vk_display_stop(&display) == ESP_OK);

    assert(vk_display_start(&display, vk_display_product_profile(), true, true) == ESP_OK);
    uint32_t token = 0U;
    assert(vk_display_begin_flush_token(&display, 0, 0, 1, 1, 2U, &token) == ESP_OK && token != 0U);
    assert(vk_display_complete_flush_token(&display, token + 1U, ESP_OK) == ESP_ERR_INVALID_STATE);
    assert(vk_display_complete_flush_token(&display, token, ESP_FAIL) == ESP_FAIL);
    assert(vk_display_complete_flush_token(&display, token, ESP_OK) == ESP_ERR_INVALID_STATE);
    assert(vk_display_is_tainted(&display));
    assert(!vk_display_transport_ready(&display));
}

typedef struct {
    vk_display_t *display;
    unsigned accepted;
} flush_thread_t;

static void *flush_worker(void *opaque)
{
    flush_thread_t *thread = opaque;
    for (unsigned i = 0; i < 1000U; ++i) {
        if (vk_display_begin_flush(thread->display, 0, 0, 1, 1, 2U) == ESP_OK) {
            ++thread->accepted;
            assert(vk_display_complete_flush(thread->display, ESP_OK) == ESP_OK);
        }
    }
    return NULL;
}

static void test_concurrent_flush_owner(void)
{
    vk_display_t display;
    assert(vk_display_start(&display, vk_display_product_profile(), true, true) == ESP_OK);
    flush_thread_t first = {.display = &display};
    flush_thread_t second = {.display = &display};
    pthread_t a, b;
    assert(pthread_create(&a, NULL, flush_worker, &first) == 0);
    assert(pthread_create(&b, NULL, flush_worker, &second) == 0);
    assert(pthread_join(a, NULL) == 0);
    assert(pthread_join(b, NULL) == 0);
    assert(first.accepted + second.accepted > 0U);
    assert(vk_display_stop(&display) == ESP_OK);
}

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((uint16_t)(red >> 3U) << 11U) |
                      ((uint16_t)(green >> 2U) << 5U) |
                      (uint16_t)(blue >> 3U));
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    char *data = malloc((size_t)length + 1U);
    assert(data != NULL);
    assert(fread(data, 1U, (size_t)length, file) == (size_t)length);
    data[length] = '\0';
    assert(fclose(file) == 0);
    return data;
}

static void test_shared_preview_fixture(const char *fixture_path)
{
    char *fixture = read_file(fixture_path);
    assert(strstr(fixture, "\"version\":1") != NULL);
    assert(strstr(fixture, "\"black_rgb565\":0") != NULL);
    assert(strstr(fixture, "\"white_rgb565\":65535") != NULL);
    assert(strstr(fixture, "\"alpha_half_red_over_black_rgb565\":32768") != NULL);
    assert(strstr(fixture, "\"metrics_sha256\":\"b6567a24b312e6e80c2f5ea200e4377d42926e11bd55544752b2533c2235b22b\"") != NULL);
    assert(strstr(fixture, "\"glyph_origins_x\":[4,8]") != NULL);
    assert(strstr(fixture, "\"positive_half_away\":\"1.3\"") != NULL);
    assert(strstr(fixture, "\"negative_half_away\":\"-1.3\"") != NULL);
    assert(strstr(fixture, "{\"height\":5,\"id\":\"a\",\"width\":10,\"x\":5,\"y\":2,\"z\":0}") != NULL);
    assert(strstr(fixture, "{\"height\":5,\"id\":\"b\",\"width\":10,\"x\":16,\"y\":2,\"z\":0}") != NULL);
    free(fixture);

    assert(rgb565(0U, 0U, 0U) == 0U);
    assert(rgb565(255U, 255U, 255U) == 65535U);
    /* Contract alpha: floor((255*128 + 127) / 255) == 128. */
    assert(rgb565(128U, 0U, 0U) == 32768U);
}

int main(int argc, char **argv)
{
    test_exact_product_profile();
    test_transport_and_truthful_screen_gate();
    test_flush_bounds_failure_and_stop();
    assert(argc == 2);
    test_concurrent_flush_owner();
    test_shared_preview_fixture(argv[1]);
    puts("display tests passed");
    return 0;
}
