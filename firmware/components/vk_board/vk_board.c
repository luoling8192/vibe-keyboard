#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "vk_board.h"
#include "vk_board_runtime.h"

const gpio_num_t vk_key_gpios[VK_KEY_COUNT] = {
    GPIO_NUM_0, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16,
};

const char *const vk_key_ids[VK_KEY_COUNT] = {"k1", "k2", "k3", "k4"};

static vk_board_runtime_t s_runtime;
static bool s_runtime_prepared;
static vk_board_display_flush_cb_t s_display_flush_cb;

static esp_err_t first_error(esp_err_t first, esp_err_t next)
{
    return first != ESP_OK ? first : next;
}

static esp_err_t hardware_keys_config(void *context)
{
    (void)context;
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_18) |
                        (1ULL << GPIO_NUM_17) | (1ULL << GPIO_NUM_16),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t hardware_keys_reset(void *context)
{
    (void)context;
    esp_err_t error = ESP_OK;
    for (size_t index = 0; index < VK_KEY_COUNT; ++index) {
        error = first_error(error, gpio_reset_pin(vk_key_gpios[index]));
    }
    return error;
}

static esp_err_t hardware_led_create(void *context, void **strip)
{
    (void)context;
    led_strip_config_t strip_config = {
        .strip_gpio_num = VK_LED_GPIO,
        .max_leds = VK_LED_COUNT,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = VK_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(
        &strip_config, &rmt_config, (led_strip_handle_t *)strip
    );
}

static esp_err_t hardware_led_clear(void *context, void *strip)
{
    (void)context;
    return led_strip_clear((led_strip_handle_t)strip);
}

static esp_err_t hardware_led_refresh(void *context, void *strip)
{
    (void)context;
    return led_strip_refresh((led_strip_handle_t)strip);
}

static esp_err_t hardware_led_delete(void *context, void *strip)
{
    (void)context;
    return led_strip_del((led_strip_handle_t)strip);
}

static esp_err_t hardware_led_gpio_reset(void *context)
{
    (void)context;
    return gpio_reset_pin(VK_LED_GPIO);
}

static esp_err_t hardware_backlight_timer_config(void *context)
{
    (void)context;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = VK_LCD_BACKLIGHT_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer);
}

static esp_err_t hardware_backlight_channel_config(void *context, uint32_t duty)
{
    (void)context;
    ledc_channel_config_t channel = {
        .gpio_num = VK_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = duty,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

static esp_err_t hardware_backlight_set(void *context, uint32_t duty)
{
    (void)context;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    return error == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) : error;
}

static esp_err_t hardware_backlight_stop(void *context)
{
    (void)context;
    return ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

static esp_err_t hardware_backlight_gpio_reset(void *context)
{
    (void)context;
    return gpio_reset_pin(VK_LCD_BACKLIGHT);
}

static esp_err_t hardware_spi_init(void *context)
{
    (void)context;
    spi_bus_config_t bus = {
        .mosi_io_num = VK_LCD_MOSI,
        .miso_io_num = VK_LCD_MISO,
        .sclk_io_num = VK_LCD_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = VK_LCD_MAX_TRANSFER,
    };
    return spi_bus_initialize(VK_LCD_HOST, &bus, SPI_DMA_CH_AUTO);
}

static esp_err_t hardware_spi_free(void *context)
{
    (void)context;
    return spi_bus_free(VK_LCD_HOST);
}

static esp_err_t hardware_panel_io_create(void *context, void **panel_io)
{
    (void)context;
    esp_lcd_panel_io_spi_config_t config = {
        .cs_gpio_num = VK_LCD_CS,
        .dc_gpio_num = VK_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = VK_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = VK_LCD_QUEUE_DEPTH,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    return esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)VK_LCD_HOST,
        &config,
        (esp_lcd_panel_io_handle_t *)panel_io
    );
}

static esp_err_t hardware_panel_io_delete(void *context, void *panel_io)
{
    (void)context;
    return esp_lcd_panel_io_del((esp_lcd_panel_io_handle_t)panel_io);
}

static esp_err_t hardware_reset_gpio_config(void *context)
{
    (void)context;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << VK_LCD_RESET,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t hardware_reset_set_low(void *context)
{
    (void)context;
    return gpio_set_level(VK_LCD_RESET, 0);
}

static esp_err_t hardware_reset_gpio_reset(void *context)
{
    (void)context;
    return gpio_reset_pin(VK_LCD_RESET);
}

static esp_err_t hardware_lvgl_init(void *context)
{
    (void)context;
    lvgl_port_cfg_t config = ESP_LVGL_PORT_INIT_CONFIG();
    config.task_priority = 5;
    config.task_stack = 8192;
    config.task_affinity = -1;
    config.task_max_sleep_ms = 500;
    config.timer_period_ms = 5;
    return lvgl_port_init(&config);
}

static esp_err_t hardware_lvgl_deinit(void *context)
{
    (void)context;
    return lvgl_port_deinit();
}

static bool hardware_lvgl_is_initialized(void *context)
{
    (void)context;
    return lv_is_initialized();
}

static void hardware_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static esp_err_t hardware_panel_create(void *context, void *panel_io, void **panel)
{
    (void)context;
    return vk_nv3007_create(
        (esp_lcd_panel_io_handle_t)panel_io, (esp_lcd_panel_handle_t *)panel
    );
}

static esp_err_t hardware_panel_delete(void *context, void *panel)
{
    (void)context;
    return esp_lcd_panel_del((esp_lcd_panel_handle_t)panel);
}

static esp_err_t hardware_panel_reset(void *context, void *panel)
{
    (void)context;
    return esp_lcd_panel_reset((esp_lcd_panel_handle_t)panel);
}

static esp_err_t hardware_panel_init(void *context, void *panel)
{
    (void)context;
    return esp_lcd_panel_init((esp_lcd_panel_handle_t)panel);
}

static esp_err_t hardware_panel_power(void *context, void *panel, bool on)
{
    (void)context;
    return esp_lcd_panel_disp_on_off((esp_lcd_panel_handle_t)panel, on);
}

static esp_err_t hardware_display_add(
    void *context,
    void *panel_io,
    void *panel,
    void **display
) {
    (void)context;
    const lvgl_port_display_cfg_t config = {
        .io_handle = (esp_lcd_panel_io_handle_t)panel_io,
        .panel_handle = (esp_lcd_panel_handle_t)panel,
        .buffer_size = VK_LCD_BUFFER_PIXELS,
        .double_buffer = true,
        .hres = VK_LCD_WIDTH,
        .vres = VK_LCD_HEIGHT,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    *display = lvgl_port_add_disp(&config);
    return *display != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t hardware_display_remove(void *context, void *display)
{
    (void)context;
    return lvgl_port_remove_disp((lv_display_t *)display);
}

static const vk_board_runtime_ops_t s_hardware_ops = {
    .keys_config = hardware_keys_config,
    .keys_reset = hardware_keys_reset,
    .led_create = hardware_led_create,
    .led_clear = hardware_led_clear,
    .led_refresh = hardware_led_refresh,
    .led_delete = hardware_led_delete,
    .led_gpio_reset = hardware_led_gpio_reset,
    .backlight_timer_config = hardware_backlight_timer_config,
    .backlight_channel_config = hardware_backlight_channel_config,
    .backlight_set = hardware_backlight_set,
    .backlight_stop = hardware_backlight_stop,
    .backlight_gpio_reset = hardware_backlight_gpio_reset,
    .spi_init = hardware_spi_init,
    .spi_free = hardware_spi_free,
    .panel_io_create = hardware_panel_io_create,
    .panel_io_delete = hardware_panel_io_delete,
    .reset_gpio_config = hardware_reset_gpio_config,
    .reset_set_low = hardware_reset_set_low,
    .reset_gpio_reset = hardware_reset_gpio_reset,
    .lvgl_init = hardware_lvgl_init,
    .lvgl_deinit = hardware_lvgl_deinit,
    .lvgl_is_initialized = hardware_lvgl_is_initialized,
    .delay_ms = hardware_delay_ms,
    .panel_create = hardware_panel_create,
    .panel_delete = hardware_panel_delete,
    .panel_reset = hardware_panel_reset,
    .panel_init = hardware_panel_init,
    .panel_power = hardware_panel_power,
    .display_add = hardware_display_add,
    .display_remove = hardware_display_remove,
};

static void ensure_runtime_prepared(void)
{
    if (!s_runtime_prepared) {
        vk_board_runtime_prepare(&s_runtime, &s_hardware_ops, NULL);
        s_runtime_prepared = true;
    }
}

esp_err_t vk_board_init(void)
{
    ensure_runtime_prepared();
    return vk_board_runtime_start(&s_runtime);
}

esp_err_t vk_board_deinit(void)
{
    ensure_runtime_prepared();
    return vk_board_runtime_cleanup(&s_runtime);
}

bool vk_board_is_tainted(void)
{
    ensure_runtime_prepared();
    return vk_board_runtime_is_tainted(&s_runtime);
}

static esp_err_t force_backlight_off(void)
{
    esp_err_t error = hardware_backlight_set(NULL, 0);
    return first_error(error, hardware_backlight_stop(NULL));
}

static esp_err_t finish_power_operation(esp_err_t panel_error, uint32_t duty)
{
    if (panel_error != ESP_OK) {
        (void)force_backlight_off();
        return panel_error;
    }
    esp_err_t backlight_error = hardware_backlight_set(NULL, duty);
    if (backlight_error != ESP_OK) {
        (void)force_backlight_off();
    }
    return backlight_error;
}

esp_err_t vk_board_display_power(bool on)
{
    if (s_runtime.panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t panel_error = esp_lcd_panel_disp_on_off(
        (esp_lcd_panel_handle_t)s_runtime.panel, on
    );
    return finish_power_operation(panel_error, on ? VK_LCD_BACKLIGHT_ON_DUTY : 0);
}

esp_err_t vk_board_display_sleep(bool sleep)
{
    if (s_runtime.panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t panel_error = esp_lcd_panel_disp_sleep(
        (esp_lcd_panel_handle_t)s_runtime.panel, sleep
    );
    if (panel_error != ESP_OK || sleep) {
        return finish_power_operation(panel_error, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(VK_LCD_WAKE_DELAY_MS));
    return finish_power_operation(ESP_OK, VK_LCD_BACKLIGHT_ON_DUTY);
}

esp_lcd_panel_handle_t vk_board_panel(void)
{
    return (esp_lcd_panel_handle_t)s_runtime.panel;
}

lv_display_t *vk_board_display(void)
{
    return (lv_display_t *)s_runtime.display;
}

void vk_board_set_display_flush_callback(vk_board_display_flush_cb_t callback)
{
    s_display_flush_cb = callback;
}

void vk_board_display_flush_complete(esp_err_t transport_result)
{
    if (s_display_flush_cb != NULL) (void)s_display_flush_cb(transport_result);
}
