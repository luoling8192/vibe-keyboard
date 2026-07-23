#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vk_board_lifecycle.h"
#include "vk_board_runtime.h"
#include "vk_nv3007_policy.h"

#define GENERIC_STAGE_COUNT 4
#define LOG_CAPACITY 4096

typedef struct {
    int fail_at;
    int release_fail_at;
    int active[GENERIC_STAGE_COUNT];
    int releases[GENERIC_STAGE_COUNT * 2];
    size_t release_count;
} lifecycle_fixture_t;

#define DEFINE_GENERIC_STAGE(INDEX) \
    static esp_err_t generic_acquire_##INDEX(void *opaque) { \
        lifecycle_fixture_t *fixture = opaque; \
        if (fixture->fail_at == INDEX) return ESP_FAIL; \
        fixture->active[INDEX] = 1; \
        return ESP_OK; \
    } \
    static esp_err_t generic_release_##INDEX(void *opaque) { \
        lifecycle_fixture_t *fixture = opaque; \
        fixture->active[INDEX] = 0; \
        fixture->releases[fixture->release_count++] = INDEX; \
        return fixture->release_fail_at == INDEX ? ESP_ERR_TIMEOUT : ESP_OK; \
    }

DEFINE_GENERIC_STAGE(0)
DEFINE_GENERIC_STAGE(1)
DEFINE_GENERIC_STAGE(2)
DEFINE_GENERIC_STAGE(3)

static const vk_board_lifecycle_stage_t generic_stages[GENERIC_STAGE_COUNT] = {
    {generic_acquire_0, generic_release_0},
    {generic_acquire_1, generic_release_1},
    {generic_acquire_2, generic_release_2},
    {generic_acquire_3, generic_release_3},
};

static void test_lifecycle_cleanup_error_is_observable(void)
{
    lifecycle_fixture_t fixture = {.fail_at = 3, .release_fail_at = 1};
    vk_board_lifecycle_t lifecycle = {
        .stages = generic_stages,
        .stage_count = GENERIC_STAGE_COUNT,
        .context = &fixture,
    };
    assert(vk_board_lifecycle_start(&lifecycle) == ESP_FAIL);
    assert(lifecycle.acquired_count == 0);
    assert(vk_board_lifecycle_is_tainted(&lifecycle));
    assert(lifecycle.cleanup_error == ESP_ERR_TIMEOUT);
    assert(fixture.release_count == 3);
    assert(fixture.releases[0] == 2);
    assert(fixture.releases[1] == 1);
    assert(fixture.releases[2] == 0);
    assert(vk_board_lifecycle_start(&lifecycle) == ESP_ERR_INVALID_STATE);
    assert(vk_board_lifecycle_cleanup(&lifecycle) == ESP_ERR_TIMEOUT);
    assert(fixture.release_count == 3);
}

static void test_lifecycle_clean_reinitialization(void)
{
    lifecycle_fixture_t fixture = {.fail_at = -1, .release_fail_at = -1};
    vk_board_lifecycle_t lifecycle = {
        .stages = generic_stages,
        .stage_count = GENERIC_STAGE_COUNT,
        .context = &fixture,
    };
    assert(vk_board_lifecycle_start(&lifecycle) == ESP_OK);
    assert(vk_board_lifecycle_cleanup(&lifecycle) == ESP_OK);
    assert(!vk_board_lifecycle_is_tainted(&lifecycle));
    assert(vk_board_lifecycle_start(&lifecycle) == ESP_OK);
    assert(vk_board_lifecycle_cleanup(&lifecycle) == ESP_OK);
}

typedef enum {
    OP_KEYS_CONFIG,
    OP_KEYS_RESET,
    OP_LED_CREATE,
    OP_LED_CLEAR,
    OP_LED_REFRESH,
    OP_LED_DELETE,
    OP_LED_GPIO_RESET,
    OP_BACKLIGHT_TIMER,
    OP_BACKLIGHT_CHANNEL,
    OP_BACKLIGHT_SET,
    OP_BACKLIGHT_STOP,
    OP_BACKLIGHT_GPIO_RESET,
    OP_SPI_INIT,
    OP_SPI_FREE,
    OP_PANEL_IO_CREATE,
    OP_PANEL_IO_DELETE,
    OP_RESET_CONFIG,
    OP_RESET_LOW,
    OP_RESET_GPIO_RESET,
    OP_LVGL_INIT,
    OP_LVGL_DEINIT,
    OP_LVGL_QUERY,
    OP_DELAY,
    OP_PANEL_CREATE,
    OP_PANEL_DELETE,
    OP_PANEL_RESET,
    OP_PANEL_INIT,
    OP_PANEL_POWER_ON,
    OP_PANEL_POWER_OFF,
    OP_DISPLAY_ADD,
    OP_DISPLAY_REMOVE,
} operation_t;

typedef struct {
    operation_t fail_operation;
    operation_t cleanup_fail_operation;
    bool fail_enabled;
    bool cleanup_fail_enabled;
    bool cleanup_phase;
    bool lvgl_stuck;
    operation_t log[LOG_CAPACITY];
    uint32_t duty_log[LOG_CAPACITY];
    size_t log_count;
    size_t nonzero_backlight_count;
} runtime_fixture_t;

static esp_err_t record(runtime_fixture_t *fixture, operation_t operation)
{
    assert(fixture->log_count < LOG_CAPACITY);
    fixture->log[fixture->log_count] = operation;
    fixture->duty_log[fixture->log_count] = UINT32_MAX;
    ++fixture->log_count;
    if (!fixture->cleanup_phase && fixture->fail_enabled && fixture->fail_operation == operation) {
        return ESP_FAIL;
    }
    if (fixture->cleanup_phase && fixture->cleanup_fail_enabled &&
            fixture->cleanup_fail_operation == operation) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

#define SIMPLE_OP(NAME, OPERATION) \
    static esp_err_t NAME(void *context) { return record(context, OPERATION); }

SIMPLE_OP(fake_keys_config, OP_KEYS_CONFIG)
SIMPLE_OP(fake_keys_reset, OP_KEYS_RESET)
SIMPLE_OP(fake_led_gpio_reset, OP_LED_GPIO_RESET)
SIMPLE_OP(fake_backlight_timer, OP_BACKLIGHT_TIMER)
SIMPLE_OP(fake_backlight_stop, OP_BACKLIGHT_STOP)
SIMPLE_OP(fake_backlight_gpio_reset, OP_BACKLIGHT_GPIO_RESET)
SIMPLE_OP(fake_spi_init, OP_SPI_INIT)
SIMPLE_OP(fake_spi_free, OP_SPI_FREE)
SIMPLE_OP(fake_reset_config, OP_RESET_CONFIG)
SIMPLE_OP(fake_reset_low, OP_RESET_LOW)
SIMPLE_OP(fake_reset_gpio_reset, OP_RESET_GPIO_RESET)
SIMPLE_OP(fake_lvgl_init, OP_LVGL_INIT)
SIMPLE_OP(fake_lvgl_deinit, OP_LVGL_DEINIT)

static esp_err_t fake_panel_reset(void *context, void *panel)
{
    assert(panel == (void *)0x33);
    return record(context, OP_PANEL_RESET);
}

static esp_err_t fake_panel_init(void *context, void *panel)
{
    assert(panel == (void *)0x33);
    return record(context, OP_PANEL_INIT);
}

static esp_err_t fake_panel_power(void *context, void *panel, bool on)
{
    assert(panel == (void *)0x33);
    return record(context, on ? OP_PANEL_POWER_ON : OP_PANEL_POWER_OFF);
}

static esp_err_t fake_led_create(void *context, void **strip)
{
    esp_err_t error = record(context, OP_LED_CREATE);
    if (error == ESP_OK) *strip = (void *)0x11;
    return error;
}

static esp_err_t fake_led_clear(void *context, void *strip)
{
    assert(strip == (void *)0x11);
    return record(context, OP_LED_CLEAR);
}

static esp_err_t fake_led_refresh(void *context, void *strip)
{
    assert(strip == (void *)0x11);
    return record(context, OP_LED_REFRESH);
}

static esp_err_t fake_led_delete(void *context, void *strip)
{
    assert(strip == (void *)0x11);
    return record(context, OP_LED_DELETE);
}

static esp_err_t fake_backlight_channel(void *context, uint32_t duty)
{
    runtime_fixture_t *fixture = context;
    esp_err_t error = record(fixture, OP_BACKLIGHT_CHANNEL);
    fixture->duty_log[fixture->log_count - 1] = duty;
    if (duty != 0) ++fixture->nonzero_backlight_count;
    return error;
}

static esp_err_t fake_backlight_set(void *context, uint32_t duty)
{
    runtime_fixture_t *fixture = context;
    esp_err_t error = record(fixture, OP_BACKLIGHT_SET);
    fixture->duty_log[fixture->log_count - 1] = duty;
    if (duty != 0) ++fixture->nonzero_backlight_count;
    return error;
}

static esp_err_t fake_panel_io_create(void *context, void **panel_io)
{
    esp_err_t error = record(context, OP_PANEL_IO_CREATE);
    if (error == ESP_OK) *panel_io = (void *)0x22;
    return error;
}

static esp_err_t fake_panel_io_delete(void *context, void *panel_io)
{
    assert(panel_io == (void *)0x22);
    return record(context, OP_PANEL_IO_DELETE);
}

static bool fake_lvgl_is_initialized(void *context)
{
    runtime_fixture_t *fixture = context;
    (void)record(fixture, OP_LVGL_QUERY);
    return fixture->lvgl_stuck;
}

static void fake_delay(void *context, uint32_t delay_ms)
{
    assert(delay_ms == 1);
    (void)record(context, OP_DELAY);
}

static esp_err_t fake_panel_create(void *context, void *panel_io, void **panel)
{
    assert(panel_io == (void *)0x22);
    esp_err_t error = record(context, OP_PANEL_CREATE);
    if (error == ESP_OK) *panel = (void *)0x33;
    return error;
}

static esp_err_t fake_panel_delete(void *context, void *panel)
{
    assert(panel == (void *)0x33);
    return record(context, OP_PANEL_DELETE);
}

static esp_err_t fake_display_add(void *context, void *panel_io, void *panel, void **display)
{
    assert(panel_io == (void *)0x22);
    assert(panel == (void *)0x33);
    esp_err_t error = record(context, OP_DISPLAY_ADD);
    if (error == ESP_OK) *display = (void *)0x44;
    return error;
}

static esp_err_t fake_display_remove(void *context, void *display)
{
    assert(display == (void *)0x44);
    return record(context, OP_DISPLAY_REMOVE);
}

static const vk_board_runtime_ops_t fake_ops = {
    .keys_config = fake_keys_config,
    .keys_reset = fake_keys_reset,
    .led_create = fake_led_create,
    .led_clear = fake_led_clear,
    .led_refresh = fake_led_refresh,
    .led_delete = fake_led_delete,
    .led_gpio_reset = fake_led_gpio_reset,
    .backlight_timer_config = fake_backlight_timer,
    .backlight_channel_config = fake_backlight_channel,
    .backlight_set = fake_backlight_set,
    .backlight_stop = fake_backlight_stop,
    .backlight_gpio_reset = fake_backlight_gpio_reset,
    .spi_init = fake_spi_init,
    .spi_free = fake_spi_free,
    .panel_io_create = fake_panel_io_create,
    .panel_io_delete = fake_panel_io_delete,
    .reset_gpio_config = fake_reset_config,
    .reset_set_low = fake_reset_low,
    .reset_gpio_reset = fake_reset_gpio_reset,
    .lvgl_init = fake_lvgl_init,
    .lvgl_deinit = fake_lvgl_deinit,
    .lvgl_is_initialized = fake_lvgl_is_initialized,
    .delay_ms = fake_delay,
    .panel_create = fake_panel_create,
    .panel_delete = fake_panel_delete,
    .panel_reset = fake_panel_reset,
    .panel_init = fake_panel_init,
    .panel_power = fake_panel_power,
    .display_add = fake_display_add,
    .display_remove = fake_display_remove,
};

static void prepare_runtime(vk_board_runtime_t *runtime, runtime_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    vk_board_runtime_prepare(runtime, &fake_ops, fixture);
}

static size_t operation_count(const runtime_fixture_t *fixture, operation_t operation)
{
    size_t count = 0;
    for (size_t index = 0; index < fixture->log_count; ++index) {
        if (fixture->log[index] == operation) ++count;
    }
    return count;
}

static void test_production_stage_failures(void)
{
    const operation_t failures[] = {
        OP_KEYS_CONFIG, OP_LED_CREATE, OP_LED_CLEAR, OP_LED_REFRESH,
        OP_BACKLIGHT_TIMER, OP_BACKLIGHT_CHANNEL, OP_SPI_INIT,
        OP_PANEL_IO_CREATE, OP_RESET_CONFIG, OP_LVGL_INIT, OP_PANEL_CREATE,
        OP_PANEL_RESET, OP_PANEL_INIT, OP_PANEL_POWER_ON, OP_DISPLAY_ADD,
        OP_BACKLIGHT_SET,
    };
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        vk_board_runtime_t runtime;
        runtime_fixture_t fixture;
        prepare_runtime(&runtime, &fixture);
        fixture.fail_enabled = true;
        fixture.fail_operation = failures[index];
        assert(vk_board_runtime_start(&runtime) == ESP_FAIL);
        assert(runtime.lifecycle.acquired_count == 0);
        if (failures[index] == OP_BACKLIGHT_SET) {
            assert(fixture.nonzero_backlight_count == 1);
        } else {
            assert(fixture.nonzero_backlight_count == 0);
        }
        assert(runtime.led_strip == NULL);
        assert(runtime.panel_io == NULL);
        assert(runtime.panel == NULL);
        assert(runtime.display == NULL);
        assert(vk_board_runtime_cleanup(&runtime) == runtime.lifecycle.cleanup_error);
        if (!vk_board_runtime_is_tainted(&runtime)) {
            fixture.fail_enabled = false;
            fixture.log_count = 0;
            assert(vk_board_runtime_start(&runtime) == ESP_OK);
            assert(fixture.nonzero_backlight_count == 1);
            fixture.cleanup_phase = true;
            assert(vk_board_runtime_cleanup(&runtime) == ESP_OK);
        }
    }
}

static void test_partial_led_failure_deletes_once(void)
{
    const operation_t failures[] = {OP_LED_CLEAR, OP_LED_REFRESH};
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        vk_board_runtime_t runtime;
        runtime_fixture_t fixture;
        prepare_runtime(&runtime, &fixture);
        fixture.fail_enabled = true;
        fixture.fail_operation = failures[index];
        assert(vk_board_runtime_start(&runtime) == ESP_FAIL);
        assert(operation_count(&fixture, OP_LED_DELETE) == 1);
        assert(runtime.led_strip == NULL);
    }
}

static void test_success_cleanup_order(void)
{
    vk_board_runtime_t runtime;
    runtime_fixture_t fixture;
    prepare_runtime(&runtime, &fixture);
    assert(vk_board_runtime_start(&runtime) == ESP_OK);
    assert(fixture.nonzero_backlight_count == 1);
    assert(operation_count(&fixture, OP_PANEL_POWER_ON) == 1);
    assert(operation_count(&fixture, OP_PANEL_POWER_OFF) == 0);
    bool saw_initial_zero_duty = false;
    for (size_t index = 0; index < fixture.log_count; ++index) {
        if (fixture.log[index] == OP_BACKLIGHT_CHANNEL && fixture.duty_log[index] == 0) {
            saw_initial_zero_duty = true;
        }
    }
    assert(saw_initial_zero_duty);
    fixture.cleanup_phase = true;
    size_t cleanup_start = fixture.log_count;
    assert(vk_board_runtime_cleanup(&runtime) == ESP_OK);
    const operation_t required_order[] = {
        OP_BACKLIGHT_SET, OP_BACKLIGHT_STOP, OP_DISPLAY_REMOVE,
        OP_PANEL_POWER_OFF, OP_PANEL_DELETE, OP_LVGL_DEINIT, OP_LVGL_QUERY,
        OP_RESET_LOW, OP_RESET_GPIO_RESET, OP_PANEL_IO_DELETE,
        OP_SPI_FREE, OP_BACKLIGHT_SET, OP_BACKLIGHT_STOP,
        OP_BACKLIGHT_GPIO_RESET, OP_LED_CLEAR, OP_LED_REFRESH,
        OP_LED_DELETE, OP_LED_GPIO_RESET, OP_KEYS_RESET,
    };
    assert(fixture.log_count - cleanup_start == sizeof(required_order) / sizeof(required_order[0]));
    for (size_t index = 0; index < sizeof(required_order) / sizeof(required_order[0]); ++index) {
        assert(fixture.log[cleanup_start + index] == required_order[index]);
    }
    size_t count = fixture.log_count;
    assert(vk_board_runtime_cleanup(&runtime) == ESP_OK);
    assert(fixture.log_count == count);
}

static void test_cleanup_failure_taints_runtime(void)
{
    const operation_t failures[] = {OP_DISPLAY_REMOVE, OP_PANEL_POWER_OFF};
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index) {
        vk_board_runtime_t runtime;
        runtime_fixture_t fixture;
        prepare_runtime(&runtime, &fixture);
        assert(vk_board_runtime_start(&runtime) == ESP_OK);
        fixture.cleanup_phase = true;
        fixture.cleanup_fail_enabled = true;
        fixture.cleanup_fail_operation = failures[index];
        assert(vk_board_runtime_cleanup(&runtime) == ESP_ERR_TIMEOUT);
        assert(vk_board_runtime_is_tainted(&runtime));
        assert(vk_board_runtime_start(&runtime) == ESP_ERR_INVALID_STATE);
    }
}

static void test_lvgl_timeout_taints_runtime(void)
{
    vk_board_runtime_t runtime;
    runtime_fixture_t fixture;
    prepare_runtime(&runtime, &fixture);
    assert(vk_board_runtime_start(&runtime) == ESP_OK);
    fixture.cleanup_phase = true;
    fixture.lvgl_stuck = true;
    assert(vk_board_runtime_cleanup(&runtime) == ESP_ERR_TIMEOUT);
    assert(vk_board_runtime_is_tainted(&runtime));
    assert(vk_board_runtime_start(&runtime) == ESP_ERR_INVALID_STATE);
}

static void test_rotation_policy(void)
{
    assert(vk_nv3007_validate_swap_xy(false) == ESP_OK);
    assert(vk_nv3007_validate_swap_xy(true) == ESP_ERR_NOT_SUPPORTED);
    assert(vk_nv3007_validate_mirror(false, false) == ESP_OK);
    assert(vk_nv3007_validate_mirror(true, false) == ESP_ERR_NOT_SUPPORTED);
    assert(vk_nv3007_validate_mirror(false, true) == ESP_ERR_NOT_SUPPORTED);
    assert(vk_nv3007_validate_mirror(true, true) == ESP_ERR_NOT_SUPPORTED);
}

int main(void)
{
    test_lifecycle_cleanup_error_is_observable();
    test_lifecycle_clean_reinitialization();
    test_production_stage_failures();
    test_partial_led_failure_deletes_once();
    test_success_cleanup_order();
    test_cleanup_failure_taints_runtime();
    test_lvgl_timeout_taints_runtime();
    test_rotation_policy();
    puts("native board runtime tests passed");
    return 0;
}
