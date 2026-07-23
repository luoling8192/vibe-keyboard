#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*vk_board_display_flush_cb_t)(esp_err_t transport_result);

#define VK_FLASH_SIZE_BYTES       (16U * 1024U * 1024U)
#define VK_PSRAM_SIZE_BYTES       (8U * 1024U * 1024U)

#define VK_LCD_HOST               SPI2_HOST
#define VK_LCD_MOSI               GPIO_NUM_21
#define VK_LCD_MISO               GPIO_NUM_NC
#define VK_LCD_SCLK               GPIO_NUM_14
#define VK_LCD_CS                 GPIO_NUM_11
#define VK_LCD_DC                 GPIO_NUM_13
#define VK_LCD_RESET              GPIO_NUM_12
#define VK_LCD_BACKLIGHT          GPIO_NUM_9
#define VK_LCD_PIXEL_CLOCK_HZ     40000000
#define VK_LCD_WIDTH              428
#define VK_LCD_HEIGHT             142
#define VK_LCD_X_GAP              0
#define VK_LCD_Y_GAP              14
#define VK_LCD_QUEUE_DEPTH        10
#define VK_LCD_BUFFER_LINES       10
#define VK_LCD_BUFFER_PIXELS      (VK_LCD_WIDTH * VK_LCD_BUFFER_LINES)
#define VK_LCD_MAX_TRANSFER       (VK_LCD_WIDTH * VK_LCD_HEIGHT * 2)
#define VK_LCD_WAKE_DELAY_MS      220
#define VK_LCD_BACKLIGHT_FREQUENCY_HZ 5000
#define VK_LCD_BACKLIGHT_INITIAL_DUTY 0
#define VK_LCD_BACKLIGHT_ON_DUTY  255
#define VK_LVGL_DEINIT_TIMEOUT_MS 1000

#define VK_KEY_COUNT              4
#define VK_KEY_SCAN_PERIOD_MS     5
#define VK_KEY_DEBOUNCE_TICKS     2
#define VK_KEY_ACTIVE_LEVEL       0

#define VK_LED_GPIO               GPIO_NUM_8
#define VK_LED_COUNT              17
#define VK_KEY_LED_COUNT          4
#define VK_STRIP_LED_COUNT        13
#define VK_STRIP_LED_OFFSET       4
#define VK_LED_RMT_RESOLUTION_HZ  10000000

#define VK_MIC_PDM_CLK            GPIO_NUM_41
#define VK_MIC_PDM_DATA           GPIO_NUM_40
#define VK_MIC_SAMPLE_RATE_HZ     16000

extern const gpio_num_t vk_key_gpios[VK_KEY_COUNT];
extern const char *const vk_key_ids[VK_KEY_COUNT];

esp_err_t vk_board_init(void);
esp_err_t vk_board_deinit(void);
bool vk_board_is_tainted(void);
esp_err_t vk_board_display_power(bool on);
esp_err_t vk_board_display_sleep(bool sleep);
esp_lcd_panel_handle_t vk_board_panel(void);
lv_display_t *vk_board_display(void);

/* Registers a callback invoked when an asynchronous display flush transfer
 * completes (success or failure).  Only one callback is supported; passing
 * NULL removes it.  The callback is called from the LVGL flush task context. */
void vk_board_set_display_flush_callback(vk_board_display_flush_cb_t callback);
/* Panel transport owners call this to propagate a flush result into the
 * display owner's in-flight/taint accounting.  Safe to call from any task. */
void vk_board_display_flush_complete(esp_err_t transport_result);

esp_err_t vk_nv3007_create(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t *out_panel);
size_t vk_nv3007_init_command_count(void);
bool vk_nv3007_startup_contains_display_on(void);

#ifdef __cplusplus
}
#endif
