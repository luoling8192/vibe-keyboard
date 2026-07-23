#!/usr/bin/env python3
import csv
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = (ROOT / "components/vk_board/include/vk_board.h").read_text()
BOARD = (ROOT / "components/vk_board/vk_board.c").read_text()
RUNTIME = (ROOT / "components/vk_board/vk_board_runtime.c").read_text()
LIFECYCLE = (ROOT / "components/vk_board/vk_board_lifecycle.c").read_text()
APP_MAIN = (ROOT / "main/app_main.c").read_text()
USB_HEADER = (ROOT / "components/vk_usb/include/vk_usb.h").read_text()
USB_SERVICE = (ROOT / "components/vk_usb/vk_usb_service.c").read_text()
USB_PRODUCTION = (ROOT / "components/vk_usb/vk_usb.c").read_text()
INPUT = (ROOT / "components/vk_input/vk_input.c").read_text()
ASSET_STORE = (ROOT / "components/vk_assets/vk_asset_store.c").read_text()
SCREEN = (ROOT / "components/vk_screen/vk_screen.c").read_text()
SCREEN_PRODUCT = (ROOT / "components/vk_screen/vk_screen_product.c").read_text()
SCREEN_SERVICE_HEADER = (ROOT / "components/vk_screen/include/vk_screen_service.h").read_text()
SDK_DEFAULTS = (ROOT / "sdkconfig.defaults").read_text()
PANEL = (ROOT / "components/vk_board/vk_nv3007.c").read_text()
TABLE = (ROOT / "components/vk_board/vk_nv3007_init.inc").read_text()
LOCK = (ROOT / "dependencies.lock").read_text()
LVGL_PORT = ROOT / "managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c"
VALIDATE_BUILD = (ROOT / "tools/validate_build.py").read_text()
AUTO_FLASH = (ROOT / "tools/auto_flash.py").read_text()
INPUT_AUDIO_CONTRACT = (ROOT.parent / "docs/product/input-audio.md").read_text()
USB_PROTOCOL_CONTRACT = (ROOT.parent / "docs/product/usb-protocol.md").read_text()
INPUT_TASK_CONTRACT = (ROOT.parent / "docs/plan/tasks/firmware-input-001.md").read_text()
SCREEN_ASSET_CONTRACT = (ROOT.parent / "docs/product/screen-assets.md").read_text()
ASSET_PROTOCOL_TASK = (ROOT.parent / "docs/plan/tasks/asset-protocol-001.md").read_text()
VKA1_CORE_TASK = (ROOT.parent / "docs/plan/tasks/vka1-core-001.md").read_text()
SCREEN_ASSET_FIXTURE = ROOT.parent / "docs/product/fixtures/screen-assets-canonical-v1.json"
LED_CONTRACT = (ROOT.parent / "docs/product/led.md").read_text()
FIRMWARE_LED_TASK = (ROOT.parent / "docs/plan/tasks/firmware-led-001.md").read_text()
CLIENT_LED_TASK = (ROOT.parent / "docs/plan/tasks/client-led-001.md").read_text()
LED_RAM_HARNESS_TASK = (ROOT.parent / "docs/plan/tasks/firmware-led-ram-harness-001.md").read_text()
LED_CALIBRATION_TASK = (ROOT.parent / "docs/plan/tasks/firmware-led-calibration-001.md").read_text()
LED_E2E_TASK = (ROOT.parent / "docs/plan/tasks/replacement-led-e2e-001.md").read_text()
LED_ARTIFACT_FIXTURE = ROOT.parent / "docs/product/fixtures/led-calibration-artifacts-v1.json"
CLIENT_HARDWARE_E2E_TASK = (ROOT.parent / "docs/plan/tasks/client-hardware-e2e-001.md").read_text()
DELIVERY_DECOMPOSITION = (ROOT.parent / "docs/plan/analysis/delivery-decomposition.md").read_text()
CLIENT_UI_CONTRACT = (ROOT.parent / "docs/ui/client.md").read_text()

EXPECTED_INIT_COMMANDS = [(95, (), 0, 0),
 (54, (96,), 1, 0),
 (255, (165,), 1, 0),
 (154, (8,), 1, 0),
 (155, (8,), 1, 0),
 (156, (176,), 1, 0),
 (157, (22,), 1, 0),
 (158, (196,), 1, 0),
 (143, (85, 4), 2, 0),
 (132, (144,), 1, 0),
 (131, (123,), 1, 0),
 (133, (51,), 1, 0),
 (96, (0,), 1, 0),
 (112, (0,), 1, 0),
 (97, (2,), 1, 0),
 (113, (2,), 1, 0),
 (98, (4,), 1, 0),
 (114, (4,), 1, 0),
 (108, (41,), 1, 0),
 (124, (41,), 1, 0),
 (109, (49,), 1, 0),
 (125, (49,), 1, 0),
 (110, (15,), 1, 0),
 (126, (15,), 1, 0),
 (102, (33,), 1, 0),
 (118, (33,), 1, 0),
 (104, (58,), 1, 0),
 (120, (58,), 1, 0),
 (99, (7,), 1, 0),
 (115, (7,), 1, 0),
 (100, (5,), 1, 0),
 (116, (5,), 1, 0),
 (101, (2,), 1, 0),
 (117, (2,), 1, 0),
 (103, (35,), 1, 0),
 (119, (35,), 1, 0),
 (105, (8,), 1, 0),
 (121, (8,), 1, 0),
 (106, (19,), 1, 0),
 (122, (19,), 1, 0),
 (107, (19,), 1, 0),
 (123, (19,), 1, 0),
 (111, (0,), 1, 0),
 (127, (0,), 1, 0),
 (80, (0,), 1, 0),
 (82, (214,), 1, 0),
 (83, (8,), 1, 0),
 (84, (8,), 1, 0),
 (85, (30,), 1, 0),
 (86, (28,), 1, 0),
 (160, (43, 36, 0), 3, 0),
 (161, (135,), 1, 0),
 (162, (134,), 1, 0),
 (165, (0,), 1, 0),
 (166, (0,), 1, 0),
 (167, (0,), 1, 0),
 (168, (54,), 1, 0),
 (169, (126,), 1, 0),
 (170, (126,), 1, 0),
 (185, (133,), 1, 0),
 (186, (132,), 1, 0),
 (187, (131,), 1, 0),
 (188, (130,), 1, 0),
 (189, (129,), 1, 0),
 (190, (128,), 1, 0),
 (191, (1,), 1, 0),
 (192, (2,), 1, 0),
 (193, (0,), 1, 0),
 (194, (0,), 1, 0),
 (195, (0,), 1, 0),
 (196, (51,), 1, 0),
 (197, (126,), 1, 0),
 (198, (126,), 1, 0),
 (200, (51, 51), 2, 0),
 (201, (104,), 1, 0),
 (202, (105,), 1, 0),
 (203, (106,), 1, 0),
 (204, (107,), 1, 0),
 (205, (51, 51), 2, 0),
 (206, (108,), 1, 0),
 (207, (109,), 1, 0),
 (208, (110,), 1, 0),
 (209, (111,), 1, 0),
 (171, (3, 103), 2, 0),
 (172, (3, 107), 2, 0),
 (173, (3, 104), 2, 0),
 (174, (3, 108), 2, 0),
 (179, (0,), 1, 0),
 (180, (0,), 1, 0),
 (181, (0,), 1, 0),
 (182, (50,), 1, 0),
 (183, (126,), 1, 0),
 (184, (126,), 1, 0),
 (224, (0,), 1, 0),
 (225, (3, 15), 2, 0),
 (226, (4,), 1, 0),
 (227, (1,), 1, 0),
 (228, (14,), 1, 0),
 (229, (1,), 1, 0),
 (230, (25,), 1, 0),
 (231, (16,), 1, 0),
 (232, (16,), 1, 0),
 (234, (18,), 1, 0),
 (235, (208,), 1, 0),
 (236, (4,), 1, 0),
 (237, (7,), 1, 0),
 (238, (7,), 1, 0),
 (239, (9,), 1, 0),
 (240, (208,), 1, 0),
 (241, (14, 23), 2, 0),
 (242, (44, 27, 11, 32), 4, 0),
 (233, (41,), 1, 0),
 (236, (4,), 1, 0),
 (53, (0,), 1, 0),
 (68, (0, 16), 2, 0),
 (70, (16,), 1, 0),
 (255, (0,), 1, 0),
 (58, (5,), 1, 0),
 (17, (), 0, 220)]
EXPECTED_PARTITION_ENTRIES_HEX = (
    "aa50010200900000004000006e76730000000000000000000000000000000000"
    "aa50010200d00000003000006e76735f6b657973000000000000000000000000"
    "aa50010100000100001000007068795f696e6974000000000000000000000000"
    "aa50010000100100002000006f74616461746100000000000000000000000000"
    "aa50001000000200000050006f74615f30000000000000000000000000000000"
    "aa50001100005200000050006f74615f31000000000000000000000000000000"
    "aa5001820000a20000005e0073746f7261676500000000000000000000000000"
)


def macro(name: str) -> str:
    match = re.search(rf"^#define\s+{name}\s+(.+)$", HEADER, re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing macro {name}")
    return match.group(1).strip()


def parsed_init_commands():
    parsed = []
    pattern = r"^\s*\{0x([0-9A-F]{2}), \{([^}]*)\}, (\d), (\d+)\},$"
    for command, data, length, delay in re.findall(pattern, TABLE, re.MULTILINE):
        values = tuple(int(item.strip(), 0) for item in data.split(",") if item.strip())
        parsed.append((int(command, 16), values, int(length), int(delay)))
    return parsed


class HardwareContractTests(unittest.TestCase):
    def test_display_bus_and_panel_constants(self):
        expected = {
            "VK_LCD_HOST": "SPI2_HOST",
            "VK_LCD_MOSI": "GPIO_NUM_21",
            "VK_LCD_MISO": "GPIO_NUM_NC",
            "VK_LCD_SCLK": "GPIO_NUM_14",
            "VK_LCD_CS": "GPIO_NUM_11",
            "VK_LCD_DC": "GPIO_NUM_13",
            "VK_LCD_RESET": "GPIO_NUM_12",
            "VK_LCD_BACKLIGHT": "GPIO_NUM_9",
            "VK_LCD_PIXEL_CLOCK_HZ": "40000000",
            "VK_LCD_WIDTH": "428",
            "VK_LCD_HEIGHT": "142",
            "VK_LCD_X_GAP": "0",
            "VK_LCD_Y_GAP": "14",
            "VK_LCD_QUEUE_DEPTH": "10",
            "VK_LCD_BUFFER_LINES": "10",
            "VK_LCD_BUFFER_PIXELS": "(VK_LCD_WIDTH * VK_LCD_BUFFER_LINES)",
            "VK_LCD_MAX_TRANSFER": "(VK_LCD_WIDTH * VK_LCD_HEIGHT * 2)",
            "VK_LCD_WAKE_DELAY_MS": "220",
            "VK_LCD_BACKLIGHT_FREQUENCY_HZ": "5000",
            "VK_LCD_BACKLIGHT_INITIAL_DUTY": "0",
            "VK_LCD_BACKLIGHT_ON_DUTY": "255",
        }
        for name, value in expected.items():
            self.assertEqual(macro(name), value)
        for text in [
            ".spi_mode = 0", ".lcd_cmd_bits = 8", ".lcd_param_bits = 8",
            "SPI_DMA_CH_AUTO", ".double_buffer = true", ".color_format = LV_COLOR_FORMAT_RGB565",
            ".buff_dma = true", ".buff_spiram = true", ".swap_bytes = true",
            ".sw_rotate = false", ".full_refresh = false", ".direct_mode = false",
            ".task_priority = 5", ".task_stack = 8192", ".task_affinity = -1",
            ".task_max_sleep_ms = 500", ".timer_period_ms = 5",
            ".duty_resolution = LEDC_TIMER_8_BIT", ".duty = duty",
            "hardware_backlight_channel_config", "hardware_backlight_set",
        ]:
            self.assertIn(text, BOARD)

    def test_exact_init_table_golden(self):
        actual = parsed_init_commands()
        self.assertEqual(actual, EXPECTED_INIT_COMMANDS)
        self.assertTrue(all(len(data) == length for _, data, length, _ in actual))
        self.assertEqual(actual[-1], (0x11, (), 0, 220))
        self.assertNotIn(0x29, [command for command, _, _, _ in actual])

    def test_draw_coordinates_and_power_commands(self):
        for text in [
            "(uint8_t)((start >> 8) & 0xff)", "(uint8_t)(start & 0xff)",
            "((end - 1) >> 8)", "(end - 1) & 0xff",
            "x_start += VK_LCD_X_GAP", "y_start += VK_LCD_Y_GAP",
            "NV3007_CMD_DISPOFF 0x28", "NV3007_CMD_DISPON  0x29",
            "NV3007_CMD_SLPIN   0x10", "NV3007_CMD_SLPOUT  0x11",
        ]:
            self.assertIn(text, PANEL)
        self.assertIn(".panel_power = hardware_panel_power", BOARD)
        self.assertIn(
            "runtime->ops->panel_power(runtime->ops_context, runtime->panel, true)",
            RUNTIME,
        )
        self.assertIn(
            "runtime->ops->panel_power(runtime->ops_context, runtime->panel, false)",
            RUNTIME,
        )
        self.assertNotIn("vk_board_display_power(true)", APP_MAIN)
        self.assertIn("force_backlight_off", BOARD)
        self.assertIn("pdMS_TO_TICKS(VK_LCD_WAKE_DELAY_MS)", BOARD)

    def test_lvgl_rotation_zero_integration_contract(self):
        if not LVGL_PORT.is_file():
            self.skipTest("run idf.py reconfigure to fetch managed components")
        source = LVGL_PORT.read_text()
        self.assertIn("version: 2.8.0~1", LOCK)
        rotation_function = source.split("static void lvgl_port_disp_rotation_update", 2)[2]
        zero_case = rotation_function.split("case LV_DISPLAY_ROTATION_0:", 1)[1].split("break;", 1)[0]
        self.assertIn("esp_lcd_panel_swap_xy(control_handle, disp_ctx->rotation.swap_xy)", zero_case)
        self.assertIn("esp_lcd_panel_mirror(control_handle, disp_ctx->rotation.mirror_x, disp_ctx->rotation.mirror_y)", zero_case)
        self.assertIn(".rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false}", BOARD)
        self.assertIn("vk_nv3007_validate_swap_xy", PANEL)
        self.assertIn("vk_nv3007_validate_mirror", PANEL)

    def test_key_led_and_microphone_contract(self):
        for text in [
            "GPIO_NUM_0, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16",
            ".pull_up_en = GPIO_PULLUP_ENABLE", "LED_MODEL_SK6812",
            "LED_STRIP_COLOR_COMPONENT_FMT_GRB", ".flags.invert_out = false",
            ".flags.with_dma = false", "led_strip_clear", "led_strip_refresh",
        ]:
            self.assertIn(text, BOARD)
        self.assertEqual(macro("VK_KEY_SCAN_PERIOD_MS"), "5")
        self.assertIn("CONFIG_FREERTOS_HZ=1000", SDK_DEFAULTS)
        self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192", SDK_DEFAULTS)
        self.assertIn("_Static_assert(CONFIG_FREERTOS_HZ == 1000", INPUT)
        self.assertIn("_Static_assert(pdMS_TO_TICKS(VK_KEY_SCAN_PERIOD_MS) > 0U", INPUT)
        self.assertIn('(BUILD / "config/sdkconfig.h").read_text()', VALIDATE_BUILD)
        self.assertIn('"#define CONFIG_FREERTOS_HZ 1000"', VALIDATE_BUILD)
        self.assertIn('"#define CONFIG_FREERTOS_HZ 1000"', AUTO_FLASH)
        self.assertIn('"#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192"', VALIDATE_BUILD)
        self.assertIn('"#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192"', AUTO_FLASH)
        self.assertIn('"#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1"', VALIDATE_BUILD)
        self.assertIn('"#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1"', AUTO_FLASH)
        self.assertIn('"#define CONFIG_SPIFFS_OBJ_NAME_LEN 96"', VALIDATE_BUILD)
        self.assertIn('"#define CONFIG_SPIFFS_OBJ_NAME_LEN 96"', AUTO_FLASH)
        self.assertIn('probe_device(port, "no_reset")', AUTO_FLASH)
        self.assertIn('probe_device(port, "default_reset")', AUTO_FLASH)
        self.assertIn('"--before", "no_reset", "--after", "watchdog_reset"', AUTO_FLASH)
        self.assertEqual(macro("VK_KEY_DEBOUNCE_TICKS"), "2")
        self.assertEqual(macro("VK_KEY_ACTIVE_LEVEL"), "0")
        self.assertNotIn("scan_keys", BOARD)
        self.assertEqual(macro("VK_LED_GPIO"), "GPIO_NUM_8")
        self.assertEqual(macro("VK_LED_COUNT"), "17")
        self.assertEqual(macro("VK_KEY_LED_COUNT"), "4")
        self.assertEqual(macro("VK_STRIP_LED_OFFSET"), "4")
        self.assertEqual(macro("VK_STRIP_LED_COUNT"), "13")
        self.assertEqual(macro("VK_LED_RMT_RESOLUTION_HZ"), "10000000")
        self.assertEqual(macro("VK_MIC_PDM_CLK"), "GPIO_NUM_41")
        self.assertEqual(macro("VK_MIC_PDM_DATA"), "GPIO_NUM_40")
        self.assertEqual(macro("VK_MIC_SAMPLE_RATE_HZ"), "16000")

    def test_large_protocol_workspaces_do_not_live_on_task_stacks(self):
        for forbidden in [
            "uint8_t ab[4092]", "uint8_t body[4092]",
            "char names[VK_ASSET_MAX_COMMITS][VK_ASSET_PATH_BYTES]",
        ]:
            self.assertNotIn(forbidden, ASSET_STORE)
        self.assertIn("heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)", ASSET_STORE)
        self.assertIn("heap_caps_calloc(", SCREEN_PRODUCT)
        self.assertNotIn(
            "vk_usb_json_document_t assets_document, screen_document",
            SCREEN_PRODUCT,
        )
        self.assertRegex(
            SCREEN,
            r"vk_screen_model_t \*candidate\s*=\s*calloc\(1U,\s*sizeof\(\*candidate\)\)",
        )
        self.assertNotIn("vk_screen_model_t candidate;", SCREEN)
        for workspace in [
            "uint8_t dispatch[VK_USB_MAX_FRAME_BYTES]",
            "vk_usb_asset_chunk_t asset_chunk",
            "tx_item_t tx_scratch[VK_USB_TYPED_TX_QUEUE_CAPACITY]",
        ]:
            self.assertIn(workspace, USB_SERVICE)
        self.assertNotIn("uint8_t frame[VK_USB_MAX_FRAME_BYTES]", USB_SERVICE)
        for audited in [
            '"vk_screen_commit": 512',
            '"validate_revision": 512',
            '"validate_commit": 768',
            '"consume": 512',
            '"dispatch_chunk": 512',
        ]:
            self.assertIn(audited, VALIDATE_BUILD)
        self.assertIn("extended/dynamic stack frame in audited symbol", VALIDATE_BUILD)

    def test_reverse_cleanup_and_safe_power_paths_are_wired(self):
        for text in [
            "vk_board_lifecycle_start", "vk_board_lifecycle_cleanup",
            "acquire_lvgl", "release_lvgl", "acquire_panel", "release_panel",
            "acquire_display", "release_display", "VK_LVGL_DEINIT_TIMEOUT_MS",
            "ESP_ERR_TIMEOUT", "vk_board_runtime_is_tainted",
        ]:
            self.assertIn(text, RUNTIME + LIFECYCLE)
        for text in [
            "lvgl_port_remove_disp", "esp_lcd_panel_del", "esp_lcd_panel_io_del",
            "lvgl_port_deinit", "spi_bus_free", "ledc_stop", "led_strip_del",
            "gpio_reset_pin",
        ]:
            self.assertIn(text, BOARD)
        stages = RUNTIME.split("static const vk_board_lifecycle_stage_t s_stages", 1)[1].split("};", 1)[0]
        self.assertLess(stages.index("{acquire_lvgl, release_lvgl}"), stages.index("{acquire_panel, release_panel}"))
        self.assertLess(stages.index("{acquire_panel, release_panel}"), stages.index("{acquire_display, release_display}"))
        self.assertLess(stages.index("{initialize_panel, release_initialized_panel}"), stages.index("{acquire_display, release_display}"))
        self.assertIn("if (panel_error != ESP_OK) {\n        (void)force_backlight_off();", BOARD)

    def test_app_main_uses_quiescent_non_aborting_failure_boundary(self):
        self.assertNotIn("ESP_ERROR_CHECK", APP_MAIN)
        self.assertNotIn("abort(", APP_MAIN)
        self.assertNotIn("esp_restart", APP_MAIN)
        self.assertIn("if (error != ESP_OK)", APP_MAIN)
        self.assertIn("(void)vk_board_deinit();", APP_MAIN)
        self.assertIn("return;", APP_MAIN)

    def test_replacement_usb_service_is_typed_bounded_and_production_wired(self):
        for item in [
            "VK_USB_MAX_FRAME_BYTES 4096U", "VK_USB_LEASE_MS 5000U",
            "usb_serial_jtag_driver_install", "usb_serial_jtag_read_bytes",
            "usb_serial_jtag_write_bytes", "vk_usb_service_poll",
            "vk_usb_start()", "vk_usb_stop()",
        ]:
            self.assertIn(item, USB_HEADER + USB_SERVICE + USB_PRODUCTION + APP_MAIN)
        for item in [
            "tinyusb", "tud_", "esp_wifi", "esp_bt", "socket(", "httpd_",
            "provision_handler", "voice_gain_handler", "raw_frame",
        ]:
            self.assertNotIn(item, USB_HEADER + USB_SERVICE + USB_PRODUCTION)
        for item in [
            "CONFIG_ESP_CONSOLE_NONE=y", "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y",
            "CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y", "CONFIG_LOG_DEFAULT_LEVEL_NONE=y",
            "CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y",
        ]:
            self.assertIn(item, SDK_DEFAULTS)

    def test_target_dependency_and_usb_only_link_contract(self):
        for item in [
            'CONFIG_IDF_TARGET="esp32s3"', "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
            "CONFIG_SPIRAM=y", "CONFIG_SPIRAM_MODE_OCT=y", "CONFIG_SPIRAM_SPEED_80M=y",
            "CONFIG_SPIRAM_XIP_FROM_PSRAM=y", "CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",
            "CONFIG_SPIRAM_RODATA=y",
            "CONFIG_BT_ENABLED=n",
        ]:
            self.assertIn(item, SDK_DEFAULTS)
        for item in ["version: 5.5.2", "version: 2.8.0~1", "version: 3.0.3", "version: 9.5.0"]:
            self.assertIn(item, LOCK)

    def test_input_audio_contract_closes_owner_epoch_and_ordering_gaps(self):
        for item in [
            "exact correlation tuple `{epoch:UInt32, generation:UInt32, session:UInt32}`",
            "release failure: never send another button_down",
            "The host establishes a recording only on the first valid AudioFrame",
            "The USB owner is the sole deadline owner",
            "one monotonic absolute deadline exactly 3,250 ms later",
            "Every `prepare`, `release`, `cancel_prepared`, `stop`, and reserved `abort` is a voice-transition barrier",
            "stop pending + a third voice interaction",
            "must not hold a USB lock or synchronously call any USB façade",
            "runtime_failed(E,R,S)",
            "cancel_prepared(E,G,S)",
            "separately reserved single-item lifecycle-abort slot",
            "`button_down` forbids `duration_ms`",
            "zero is forbidden",
            "one stable FIFO filter",
            "pre-epoch `stopping`",
            "`ack_linearization_time < absolute_deadline`",
            "At `ack_linearization_time == absolute_deadline` or later, timeout wins",
        ]:
            self.assertIn(item, INPUT_AUDIO_CONTRACT)
        self.assertNotIn(
            "prepare/release failure: handoff ordinary button_down without session",
            INPUT_AUDIO_CONTRACT,
        )

    def test_usb_input_contract_defines_terminal_and_lifecycle_linearization(self):
        for item in [
            "vk_usb_fail_epoch(expected_epoch, VK_USB_SESSION_ERROR_INPUT_QUEUE_OVERFLOW)",
            'which schedules `{"event":"vk_error","operation":"input","code":"input_queue_overflow"}`',
            "A mismatch returns `EPOCH_CLOSED` and mutates nothing",
            "returns exactly `ACCEPTED|RETRY|EPOCH_CLOSED|OVERFLOW`",
            "USB owner alone owns one monotonic absolute 3,250 ms deadline",
            "dedicated one-item cell",
            "separate single reserved lifecycle-abort slot",
            "Exactly one lifecycle request may be outstanding",
            "A `stopping` request has highest priority",
            "First new-epoch requires old zero/proposed nonzero",
            "`{kind:stopping,old_epoch:0,proposed_epoch:0}`",
            "`old_epoch == 0` is legal only for first new-epoch and pre-epoch stopping",
            "A zero `proposed_epoch` remains required for normal lease expiry and stopping when `old_epoch` is nonzero",
            "`ack_linearization_time < absolute_deadline`",
            "ESP32-S3 built-in USB Serial/JTAG only",
        ]:
            self.assertIn(item, USB_PROTOCOL_CONTRACT)

    def test_input_task_requires_three_owners_and_review_before_code(self):
        self.assertRegex(INPUT_TASK_CONTRACT, r"status: (?:in-progress|done)")
        for item in [
            "Use exactly three owners",
            "fixed four-item ordinary command/result mailboxes plus one separately reserved lifecycle-abort slot",
            "Independent contract review must pass before implementation starts",
            "do not add BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, or USB Audio Class fallback",
        ]:
            self.assertIn(item, INPUT_TASK_CONTRACT)
        self.assertNotIn("One state machine owns debounce", INPUT_TASK_CONTRACT)


class ScreenAssetContractTests(unittest.TestCase):
    def test_product_budget_admits_full_screen_replacement(self):
        budget = re.search(
            r"#define VK_SCREEN_SERVICE_DECODE_BUDGET_BYTES (\d+)U",
            SCREEN_SERVICE_HEADER,
        )
        scratch = re.search(
            r"#define VK_SCREEN_SERVICE_DECODER_SCRATCH_BYTES (\d+)U",
            SCREEN_SERVICE_HEADER,
        )
        self.assertIsNotNone(budget)
        self.assertIsNotNone(scratch)
        self.assertGreaterEqual(
            int(budget.group(1)),
            2 * 428 * 142 * 2 + int(scratch.group(1)),
        )

    def test_canonical_fixture_is_byte_exact_and_font_hash_bound(self):
        fixture_bytes = SCREEN_ASSET_FIXTURE.read_bytes()
        self.assertFalse(fixture_bytes.endswith(b"\n"))
        fixture = json.loads(fixture_bytes)
        self.assertEqual(
            fixture_bytes,
            json.dumps(fixture, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode(),
        )
        for name, record in fixture["canonical_records"].items():
            record_bytes = record.encode()
            self.assertFalse(record_bytes.endswith(b"\n"), name)
            parsed = json.loads(record_bytes)
            self.assertEqual(
                record_bytes,
                json.dumps(parsed, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode(),
                name,
            )
        font = fixture["font_metrics"]
        font_path = SCREEN_ASSET_FIXTURE.parent / font["path"]
        font_bytes = font_path.read_bytes()
        self.assertFalse(font_bytes.endswith(b"\n"))
        self.assertEqual(hashlib.sha256(font_bytes).hexdigest(), font["sha256"] )
        self.assertEqual(
            font_bytes,
            json.dumps(json.loads(font_bytes), sort_keys=True, separators=(",", ":")).encode(),
        )
        self.assertIn("`metrics_sha256` is SHA-256 of the complete file bytes", SCREEN_ASSET_CONTRACT)

    def test_screen_asset_contract_closes_exactness_and_capacity_gaps(self):
        for item in [
            "No Version-1 decoded-buffer sharing exists",
            "current_steady + candidate_steady + decoder_scratch_bytes <= max_active_decoded_bytes",
            "one fixed global scratch allocation owned by the single Version-1 decoder owner",
            "Firmware and Swift use the exact current-epoch capability value",
            "rejects `-0`, `-0.0`, `1.0`, `1.20`",
            "compare emitted bytes directly",
            "The host `assets` object includes exactly `assets`",
            "`max_layout_bytes` is the byte length of the complete canonical embedded `layout` object alone",
            "Bindings are exact and one-to-one",
            "`text|integer|number → dynamic_label|icon_text`; `progress → progress` only",
            "docs/product/fixtures/fonts/<id>-v<version>.metrics.json",
            "30,000 ms monotonic idle deadline",
            "all valid immutable VKA1 objects stored at once",
            "1...min(upload_max_bytes,max_asset_bytes)",
            "`screen.available:true` requires a valid `assets.available:true` block in the same complete current-epoch capability snapshot",
            "available assets block is the sole owner of `max_active_decoded_bytes` and `decoder_scratch_bytes`",
            "duplicate capability snapshot never merges blocks or limits with the prior snapshot",
        ]:
            self.assertIn(item, SCREEN_ASSET_CONTRACT)
        self.assertIn("status: in-progress", ASSET_PROTOCOL_TASK)
        for item in [
            "UInt16 LE  body_length = 8 + N",
            "UInt32 LE  nonzero transfer_id",
            "UInt32 LE  exact next_offset",
            "N <= 4096 - 12 = 4084",
            "declared body length larger than currently buffered bytes remains incomplete",
            "Type `0x40` is never legal device→host",
        ]:
            self.assertIn(item, USB_PROTOCOL_CONTRACT)
        self.assertIn("N` is `1...min(current vk_asset_ready.chunk_bytes,4084)", SCREEN_ASSET_CONTRACT)
        for forbidden in ["BLE fallback", "TinyUSB fallback", "network fallback"]:
            self.assertNotIn(forbidden, SCREEN_ASSET_CONTRACT)

    def test_vka1_canonical_encoder_contract(self):
        for item in [
            "scans each row left-to-right",
            "exactly one run for every maximal sequence",
            "never splits a maximal run",
            "never merges runs across rows",
            "width is at most 428",
            "strictly smaller",
            "equality and larger RowRLE select raw",
            "encoding bits equal exactly the union",
            "byte-identical frame payloads, tables, complete containers, and full-container hashes",
            "a maximal full-width run",
            "row-boundary equal colors that remain separate runs",
            "complete-container SHA-256 known answers",
        ]:
            self.assertIn(item, SCREEN_ASSET_CONTRACT)
        self.assertRegex(VKA1_CORE_TASK, r"status: (?:pending|in-progress|done)")
        self.assertIn("depends-on: [asset-protocol-001]", VKA1_CORE_TASK)
        self.assertIn("equality selects raw", VKA1_CORE_TASK)
        self.assertIn("complete bytes, not only decoded pixels", VKA1_CORE_TASK)

    def test_asset_chunk_wire_goldens_and_bounds(self):
        def encode(transfer_id, offset, payload):
            body_length = 8 + len(payload)
            return b"\x01\x40" + struct.pack("<HII", body_length, transfer_id, offset) + payload

        one = encode(0x01020304, 0x05060708, b"\xaa")
        self.assertEqual(one, bytes.fromhex("01 40 09 00 04 03 02 01 08 07 06 05 aa"))
        self.assertEqual(len(encode(1, 0, bytes(4084))), 4096)
        self.assertEqual(encode(1, 0, bytes(4084))[2:4], bytes.fromhex("fc 0f"))
        self.assertEqual(len(encode(1, 0, bytes(4085))), 4097)
        self.assertEqual(encode(1, 0, bytes(4085))[2:4], bytes.fromhex("fd 0f"))
        self.assertEqual(encode(1, 0, b"")[2:4], bytes.fromhex("08 00"))
        self.assertNotEqual(one[4:8], struct.pack(">I", 0x01020304))


class LEDContractTests(unittest.TestCase):
    def test_led_contract_is_fail_dark_and_evidence_bounded(self):
        for item in [
            "GPIO8",
            "17-pixel",
            "SK6812",
            "three-component GRB order",
            "10 MHz value is the RMT encoder tick resolution, not the SK6812 wire bit rate",
            "Raw pixels `0...3` are the four key LEDs",
            "raw pixels `4...16` are the thirteen strip LEDs",
            "advertises LED unavailable",
            'reason":"calibration_required',
            "mapping from logical `k1...k4`",
            "remain intentionally unspecified until physical calibration",
            "fixed eight-item vk_led ordinary mailbox",
            "No public API accepts a pixel index",
            "rejects rather than clamps",
            "clear → refresh",
            "exact value 1",
            "`duration_ms` is `1...250`",
            "mapping artifact alone can never make LED available",
            "independently reviewed sustained-current evidence",
        ]:
            self.assertIn(item, LED_CONTRACT)
        for forbidden in [
            '"k1":0',
            '"max_brightness":255',
            "Bluetooth fallback",
            "authorization_sha256}",
        ]:
            self.assertNotIn(forbidden, LED_CONTRACT)
        for item in [
            "ELF32 little-endian Xtensa executable",
            "`p_paddr == p_vaddr`",
            "half-open IRAM `[0x40370000,0x403E0000)`",
            "half-open DRAM `[0x3FC88000,0x3FD00000)`",
            "`elf2image --chip esp32s3 --use_segments --ram-only-header`",
            "VKLED-STIMULUS-V1\\0",
            "VKLED-HARNESS-MANIFEST-V1\\0",
            "VKLED-CALIBRATION-AUTH-V1\\0",
            "never embedded in or fed back into the descriptor, ELF, image, or manifest",
            "one record can cause at most one execution attempt",
            "padded_filesz = checked((p_filesz + 3) & ~3)",
            "padded_filesz <= p_memsz",
            "41 42 43 00",
            "descriptor_sha256 = SHA-256(descriptor_bytes)",
            "stimulus_identity = SHA-256(\"VKLED-STIMULUS-V1\\0\" || descriptor_bytes)",
            "harness_manifest_sha256 = SHA-256(manifest_bytes)",
            "harness_manifest_identity = SHA-256(\"VKLED-HARNESS-MANIFEST-V1\\0\" || manifest_bytes)",
            "authorization_record_sha256 = SHA-256(authorization_record_bytes)",
            "authorization_identity = SHA-256(\"VKLED-CALIBRATION-AUTH-V1\\0\" || authorization_record_bytes)",
        ]:
            self.assertIn(item, LED_CONTRACT)

    def test_led_artifact_known_answers_are_byte_exact(self):
        fixture = json.loads(LED_ARTIFACT_FIXTURE.read_text())
        descriptor = fixture["descriptor_bytes_utf8"].encode("utf-8")
        manifest = fixture["manifest_bytes_utf8"].encode("utf-8")
        authorization = fixture["authorization_record_bytes_utf8"].encode("utf-8")
        self.assertTrue(descriptor.endswith(b"\n"))
        self.assertTrue(manifest.endswith(b"\n"))
        self.assertTrue(authorization.endswith(b"\n"))
        self.assertEqual(hashlib.sha256(descriptor).hexdigest(), fixture["descriptor_sha256"])
        self.assertEqual(
            hashlib.sha256(b"VKLED-STIMULUS-V1\0" + descriptor).hexdigest(),
            fixture["stimulus_identity"],
        )
        self.assertEqual(hashlib.sha256(manifest).hexdigest(), fixture["harness_manifest_sha256"])
        self.assertEqual(
            hashlib.sha256(b"VKLED-HARNESS-MANIFEST-V1\0" + manifest).hexdigest(),
            fixture["harness_manifest_identity"],
        )
        self.assertEqual(hashlib.sha256(authorization).hexdigest(), fixture["authorization_record_sha256"])
        self.assertEqual(
            hashlib.sha256(b"VKLED-CALIBRATION-AUTH-V1\0" + authorization).hexdigest(),
            fixture["authorization_identity"],
        )
        manifest_obj = json.loads(manifest)
        authorization_obj = json.loads(authorization)
        self.assertEqual(manifest_obj["descriptor_sha256"], fixture["descriptor_sha256"])
        self.assertEqual(manifest_obj["stimulus_identity"], fixture["stimulus_identity"])
        self.assertEqual(authorization_obj["stimulus_identity"], fixture["stimulus_identity"])
        self.assertEqual(authorization_obj["harness_manifest_sha256"], fixture["harness_manifest_sha256"])
        self.assertEqual(authorization_obj["harness_manifest_identity"], fixture["harness_manifest_identity"])
        self.assertNotEqual(fixture["descriptor_sha256"], fixture["stimulus_identity"])
        self.assertNotEqual(fixture["harness_manifest_sha256"], fixture["harness_manifest_identity"])
        self.assertNotEqual(fixture["authorization_record_sha256"], fixture["authorization_identity"])

    def test_led_usb_lifecycle_and_owner_contract_are_exact(self):
        for item in [
            '{"event":"vk_led_query","request_id":1}',
            '{"event":"vk_led_config","request_id":2,"enabled":true,"brightness":N}',
            '"source":"query"',
            '"source":"applied"',
            "nonzero `UInt32`",
            "at most one outstanding config",
            "one non-extending 1,000 ms monotonic absolute deadline",
            "response_linearization_time < absolute_deadline",
            "equality or a later timestamp is timeout",
            "late matching applied response may update observed device state but cannot complete",
            "Host write completion and a query response are never config acknowledgement",
            "hardware_failure > stopping > epoch_off",
            "cleanup_proof",
            "ack_obligation",
            "invalidates the old acknowledgement sink exactly once",
            "only the fresh stopping token/generation can receive its result",
            "participant-specific one-item acknowledgement sink",
            "same monotonic absolute 3,250 ms transition deadline",
            "LED `TAINTED`, begin failure, or timeout permanently taints/closes that USB composition",
            "Bluetooth, BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, and USB Audio Class are prohibited",
            "`vk_led` is the sole LED state and animation owner",
            "complete 17-pixel logical-RGB frame",
        ]:
            self.assertIn(item, LED_CONTRACT)
        self.assertIn("every registered participant", USB_PROTOCOL_CONTRACT)
        self.assertIn("Current feature keys are `assets`, `screen`, `led`, and `update`", USB_PROTOCOL_CONTRACT)

    def test_led_tasks_and_dag_preserve_separate_calibration_gate(self):
        self.assertRegex(FIRMWARE_LED_TASK, r"status: (ready|in-progress|done)")
        for item in [
            "Do not fill logical key-pixel mapping, `max_brightness`, or `max_frame_channel_sum` with guessed values",
            "Register LED as an asynchronous USB lifecycle participant",
            "Do not add BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, or USB Audio Class fallback",
        ]:
            self.assertIn(item, FIRMWARE_LED_TASK)
        for item in [
            "status: blocked",
            "unresolved",
            "cache_hal_get_cache_line_size",
            "esp_cache_msync",
            "CONFIG_APP_BUILD_TYPE_PURE_RAM_APP=y",
            "--chip esp32s3 --no-stub load_ram",
            "[0x40370000,0x403E0000)",
            "[0x3FC88000,0x3FD00000)",
            "ELF32 little-endian Xtensa `ET_EXEC`",
            "p_paddr == p_vaddr",
            "elf2image --chip esp32s3 --use_segments --ram-only-header",
            "IROM/DROM, PSRAM/EXTRAM, RTC, overlap, alias, wrap",
            "missing/extra/split/reordered/differently merged/padded/image-byte mismatch",
            "padded_filesz = checked((p_filesz + 3) & ~3)",
            "41 42 43 → 41 42 43 00",
            "external and never feeds back into the image",
            "Do not fall back to a temporary flash image",
            "no serial, USB device, ROM entry, reset, `load_ram`, flash, LED illumination",
        ]:
            self.assertIn(item, LED_RAM_HARNESS_TASK)
        for item in [
            "status: blocked",
            "depends-on: [firmware-led-ram-harness-001]",
            "VKLED-CALIBRATION-AUTH-V1",
            "never embedded into or fed back into the harness image",
            "append-only private ledger",
            "one ROM-entry reset",
            "one `--no-stub load_ram`",
            "one separately authorized recovery reset or power-cycle",
            "unchanged selection metadata",
            "separately documented, authorized, and independently reviewed sustained-current method",
        ]:
            self.assertIn(item, LED_CALIBRATION_TASK)
        for item in [
            "status: pending",
            "depends-on: [firmware-bootstrap-001, firmware-led-001, firmware-led-ram-harness-001, firmware-led-calibration-001, client-led-001]",
            "Every raw-pixel/channel stimulus requires separate explicit authorization for one ROM-entry reset",
        ]:
            self.assertIn(item, LED_E2E_TASK)
        self.assertRegex(CLIENT_LED_TASK, r"status: (pending|in-progress|done)")
        for item in [
            "depends-on: [client-usb-002, firmware-led-001]",
            "matching current-epoch `source:\"applied\"` response",
        ]:
            self.assertIn(item, CLIENT_LED_TASK)
        self.assertIn("Replacement LED calibration is explicitly outside this vendor input/audio task", CLIENT_HARDWARE_E2E_TASK)
        self.assertIn("firmware-led-ram-harness-001", DELIVERY_DECOMPOSITION)
        self.assertIn("firmware-led-calibration-001", DELIVERY_DECOMPOSITION)
        self.assertIn("replacement-led-e2e-001", DELIVERY_DECOMPOSITION)
        self.assertIn("LED before calibration/unavailable", CLIENT_UI_CONTRACT)
        self.assertIn("hide enable/brightness controls", CLIENT_UI_CONTRACT)


class PartitionContractTests(unittest.TestCase):
    EXPECTED = [
        ("nvs", "data", "nvs", 0x009000, 0x004000, ""),
        ("nvs_keys", "data", "nvs", 0x00D000, 0x003000, ""),
        ("phy_init", "data", "phy", 0x010000, 0x001000, ""),
        ("otadata", "data", "ota", 0x011000, 0x002000, ""),
        ("ota_0", "app", "ota_0", 0x020000, 0x500000, ""),
        ("ota_1", "app", "ota_1", 0x520000, 0x500000, ""),
        ("storage", "data", "spiffs", 0xA20000, 0x5E0000, ""),
    ]

    def test_partition_metadata_boundaries_and_golden_binary(self):
        rows = []
        with (ROOT / "partitions.csv").open(newline="") as handle:
            for row in csv.reader(line for line in handle if not line.startswith("#")):
                fields = [field.strip() for field in row]
                rows.append((fields[0], fields[1], fields[2], int(fields[3], 0), int(fields[4], 0), fields[5]))
        self.assertEqual(rows, self.EXPECTED)
        for (_, _, _, offset, size, _), (_, _, _, next_offset, _, _) in zip(rows, rows[1:]):
            self.assertLessEqual(offset + size, next_offset)
        self.assertEqual(rows[-1][3] + rows[-1][4], 16 * 1024 * 1024)

        idf_path = os.environ.get("IDF_PATH")
        if not idf_path:
            self.skipTest("IDF_PATH is required for generated partition golden verification")
        generator = pathlib.Path(idf_path) / "components/partition_table/gen_esp32part.py"
        with tempfile.TemporaryDirectory(prefix="vk-partitions-") as directory:
            output = pathlib.Path(directory) / "partition-table.bin"
            subprocess.run([sys.executable, str(generator), str(ROOT / "partitions.csv"), str(output)], check=True)
            self.assertEqual(output.read_bytes()[: 7 * 32], bytes.fromhex(EXPECTED_PARTITION_ENTRIES_HEX))


if __name__ == "__main__":
    unittest.main()
