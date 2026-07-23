#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "vk_board.h"
#include "vk_nv3007_policy.h"

#define NV3007_CMD_CASET   0x2A
#define NV3007_CMD_RASET   0x2B
#define NV3007_CMD_RAMWR   0x2C
#define NV3007_CMD_DISPOFF 0x28
#define NV3007_CMD_DISPON  0x29
#define NV3007_CMD_SLPIN   0x10
#define NV3007_CMD_SLPOUT  0x11

static const char *TAG = "vk_nv3007";

typedef struct {
    uint8_t command;
    uint8_t data[4];
    uint8_t data_length;
    uint16_t delay_ms;
} vk_nv3007_init_command_t;

static const vk_nv3007_init_command_t s_init_commands[] = {
#include "vk_nv3007_init.inc"
};

_Static_assert(sizeof(s_init_commands) / sizeof(s_init_commands[0]) == 119, "NV3007 init table must contain 119 entries");

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
} vk_nv3007_panel_t;

static esp_err_t nv3007_reset(esp_lcd_panel_t *panel)
{
    (void)panel;
    ESP_RETURN_ON_ERROR(gpio_set_level(VK_LCD_RESET, 0), TAG, "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(VK_LCD_RESET, 1), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t nv3007_init(esp_lcd_panel_t *panel)
{
    vk_nv3007_panel_t *nv = __containerof(panel, vk_nv3007_panel_t, base);
    for (size_t i = 0; i < sizeof(s_init_commands) / sizeof(s_init_commands[0]); ++i) {
        const vk_nv3007_init_command_t *entry = &s_init_commands[i];
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(nv->io, entry->command, entry->data_length ? entry->data : NULL, entry->data_length),
            TAG,
            "init command 0x%02x failed",
            entry->command
        );
        if (entry->delay_ms != 0) {
            vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t nv3007_del(esp_lcd_panel_t *panel)
{
    free(__containerof(panel, vk_nv3007_panel_t, base));
    return ESP_OK;
}

static esp_err_t tx_coordinates(vk_nv3007_panel_t *nv, uint8_t command, int start, int end)
{
    uint8_t data[] = {
        (uint8_t)((start >> 8) & 0xff), (uint8_t)(start & 0xff),
        (uint8_t)(((end - 1) >> 8) & 0xff), (uint8_t)((end - 1) & 0xff),
    };
    return esp_lcd_panel_io_tx_param(nv->io, command, data, sizeof(data));
}

static esp_err_t nv3007_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    ESP_RETURN_ON_FALSE(color_data != NULL, ESP_ERR_INVALID_ARG, TAG, "color data is null");
    ESP_RETURN_ON_FALSE(x_start >= 0 && y_start >= 0 && x_end > x_start && y_end > y_start,
                        ESP_ERR_INVALID_ARG, TAG, "invalid draw area");
    ESP_RETURN_ON_FALSE(x_end <= VK_LCD_WIDTH && y_end <= VK_LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "draw area out of bounds");
    vk_nv3007_panel_t *nv = __containerof(panel, vk_nv3007_panel_t, base);
    x_start += VK_LCD_X_GAP;
    x_end += VK_LCD_X_GAP;
    y_start += VK_LCD_Y_GAP;
    y_end += VK_LCD_Y_GAP;
    ESP_RETURN_ON_ERROR(tx_coordinates(nv, NV3007_CMD_CASET, x_start, x_end), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(tx_coordinates(nv, NV3007_CMD_RASET, y_start, y_end), TAG, "RASET failed");
    size_t bytes = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * sizeof(uint16_t);
    return esp_lcd_panel_io_tx_color(nv->io, NV3007_CMD_RAMWR, color_data, bytes);
}

static esp_err_t nv3007_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    (void)panel;
    return vk_nv3007_validate_mirror(mirror_x, mirror_y);
}

static esp_err_t nv3007_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    (void)panel;
    return vk_nv3007_validate_swap_xy(swap_axes);
}

static esp_err_t nv3007_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    (void)panel;
    (void)invert;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t nv3007_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    (void)panel;
    return (x_gap == VK_LCD_X_GAP && y_gap == VK_LCD_Y_GAP) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t nv3007_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    vk_nv3007_panel_t *nv = __containerof(panel, vk_nv3007_panel_t, base);
    return esp_lcd_panel_io_tx_param(nv->io, on ? NV3007_CMD_DISPON : NV3007_CMD_DISPOFF, NULL, 0);
}

static esp_err_t nv3007_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    vk_nv3007_panel_t *nv = __containerof(panel, vk_nv3007_panel_t, base);
    return esp_lcd_panel_io_tx_param(nv->io, sleep ? NV3007_CMD_SLPIN : NV3007_CMD_SLPOUT, NULL, 0);
}

esp_err_t vk_nv3007_create(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(io != NULL && out_panel != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    vk_nv3007_panel_t *nv = calloc(1, sizeof(*nv));
    ESP_RETURN_ON_FALSE(nv != NULL, ESP_ERR_NO_MEM, TAG, "panel allocation failed");
    nv->io = io;
    nv->base.reset = nv3007_reset;
    nv->base.init = nv3007_init;
    nv->base.del = nv3007_del;
    nv->base.draw_bitmap = nv3007_draw_bitmap;
    nv->base.mirror = nv3007_mirror;
    nv->base.swap_xy = nv3007_swap_xy;
    nv->base.set_gap = nv3007_set_gap;
    nv->base.invert_color = nv3007_invert_color;
    nv->base.disp_on_off = nv3007_disp_on_off;
    nv->base.disp_sleep = nv3007_disp_sleep;
    *out_panel = &nv->base;
    return ESP_OK;
}

size_t vk_nv3007_init_command_count(void)
{
    return sizeof(s_init_commands) / sizeof(s_init_commands[0]);
}

bool vk_nv3007_startup_contains_display_on(void)
{
    for (size_t i = 0; i < sizeof(s_init_commands) / sizeof(s_init_commands[0]); ++i) {
        if (s_init_commands[i].command == NV3007_CMD_DISPON) {
            return true;
        }
    }
    return false;
}
