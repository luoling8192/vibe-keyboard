#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vk-native-") as directory:
    binary = pathlib.Path(directory) / "test_lifecycle"
    command = [
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DVK_BOARD_RUNTIME_NATIVE=1",
        "-I",
        str(ROOT / "tests/native/include"),
        "-I",
        str(ROOT / "components/vk_board/include"),
        str(ROOT / "tests/native/test_lifecycle.c"),
        str(ROOT / "components/vk_board/vk_board_lifecycle.c"),
        str(ROOT / "components/vk_board/vk_board_runtime.c"),
        str(ROOT / "components/vk_board/vk_nv3007_policy.c"),
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

    json_binary = pathlib.Path(directory) / "test_vk_usb_json"
    json_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_vk_usb_json.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        "-o", str(json_binary),
    ]
    subprocess.run(json_command, check=True)
    subprocess.run([str(json_binary)], check=True)

    capability_binary = pathlib.Path(directory) / "test_usb_capabilities"
    capability_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_capabilities.c"),
        str(ROOT / "components/vk_usb/vk_usb_capabilities.c"),
        "-o", str(capability_binary),
    ]
    subprocess.run(capability_command, check=True)
    subprocess.run([str(capability_binary)], check=True)

    asset_protocol_binary = pathlib.Path(directory) / "test_usb_asset_protocol"
    asset_protocol_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        "-o", str(asset_protocol_binary),
    ]
    subprocess.run(asset_protocol_command, check=True)
    subprocess.run([str(asset_protocol_binary)], check=True)

    vka1_binary = pathlib.Path(directory) / "test_vka1"
    vka1_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "components/vk_vka1/include"),
        str(ROOT / "tests/native/test_vka1.c"),
        str(ROOT / "components/vk_vka1/vk_vka1.c"),
        "-o", str(vka1_binary),
    ]
    subprocess.run(vka1_command, check=True)
    subprocess.run(
        [str(vka1_binary), str(ROOT.parent / "mac/Tests/VibeBoardKitTests/Fixtures/VKA1")],
        check=True,
    )

    asset_store_binary = pathlib.Path(directory) / "test_asset_store"
    asset_store_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        str(ROOT / "tests/native/test_asset_store.c"),
        str(ROOT / "components/vk_assets/vk_asset_store.c"),
        "-o", str(asset_store_binary),
    ]
    subprocess.run(asset_store_command, check=True)
    subprocess.run([str(asset_store_binary)], check=True)

    screen_binary = pathlib.Path(directory) / "test_screen"
    screen_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_screen/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        "-I", str(ROOT / "components/vk_vka1/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        str(ROOT / "tests/native/test_screen.c"),
        str(ROOT / "components/vk_screen/vk_screen.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_assets/vk_asset_store.c"),
        "-o", str(screen_binary),
    ]
    subprocess.run(screen_command, check=True)
    subprocess.run([str(screen_binary)], check=True)

    screen_service_object = pathlib.Path(directory) / "vk_screen_service.o"
    screen_service_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_screen/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        "-I", str(ROOT / "components/vk_vka1/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        "-I", str(ROOT / "components/vk_asset_transfer/include"),
        "-I", str(ROOT / "components/vk_display/include"),
        "-c", str(ROOT / "components/vk_screen/vk_screen_service.c"),
        "-o", str(screen_service_object),
    ]
    subprocess.run(screen_service_command, check=True)

    screen_recovery_binary = pathlib.Path(directory) / "test_screen_recovery"
    screen_recovery_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_screen/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        "-I", str(ROOT / "components/vk_vka1/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        "-I", str(ROOT / "components/vk_asset_transfer/include"),
        "-I", str(ROOT / "components/vk_display/include"),
        str(ROOT / "tests/native/test_screen_recovery.c"),
        str(ROOT / "components/vk_screen/vk_screen.c"),
        str(ROOT / "components/vk_screen/vk_screen_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_assets/vk_asset_store.c"),
        str(ROOT / "components/vk_asset_transfer/vk_asset_transfer.c"),
        str(ROOT / "components/vk_display/vk_display.c"),
        "-o", str(screen_recovery_binary),
    ]
    subprocess.run(screen_recovery_command, check=True)
    subprocess.run([str(screen_recovery_binary)], check=True)

    chain_binary = pathlib.Path(directory) / "test_asset_screen_chain"
    chain_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_screen/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        "-I", str(ROOT / "components/vk_vka1/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        "-I", str(ROOT / "components/vk_asset_transfer/include"),
        "-I", str(ROOT / "components/vk_display/include"),
        str(ROOT / "tests/native/test_asset_screen_chain.c"),
        str(ROOT / "components/vk_screen/vk_screen.c"),
        str(ROOT / "components/vk_screen/vk_screen_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_usb/vk_usb_capabilities.c"),
        str(ROOT / "components/vk_usb/vk_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_led_protocol.c"),
        str(ROOT / "components/vk_assets/vk_asset_store.c"),
        str(ROOT / "components/vk_asset_transfer/vk_asset_transfer.c"),
        str(ROOT / "components/vk_vka1/vk_vka1.c"),
        str(ROOT / "components/vk_display/vk_display.c"),
        "-DVK_USB_NATIVE_TEST=1",
        "-o", str(chain_binary),
    ]
    subprocess.run(chain_command, check=True)
    subprocess.run(
        [
            str(chain_binary),
            str(ROOT.parent / "mac/Tests/VibeBoardKitTests/Fixtures/VKA1/one-pixel.vka1"),
        ],
        check=True,
    )

    screen_concurrency_binary = pathlib.Path(directory) / "test_screen_concurrency"
    screen_concurrency_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_screen/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        "-I", str(ROOT / "components/vk_vka1/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        str(ROOT / "tests/native/test_screen_concurrency.c"),
        str(ROOT / "components/vk_screen/vk_screen.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_assets/vk_asset_store.c"),
        "-o", str(screen_concurrency_binary),
    ]
    subprocess.run(screen_concurrency_command, check=True)
    subprocess.run([str(screen_concurrency_binary)], check=True)

    display_binary = pathlib.Path(directory) / "test_display"
    display_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_display/include"),
        str(ROOT / "tests/native/test_display.c"),
        str(ROOT / "components/vk_display/vk_display.c"),
        "-o", str(display_binary),
    ]
    subprocess.run(display_command, check=True)
    subprocess.run([
        str(display_binary),
        str(ROOT.parent / "docs/product/fixtures/preview-geometry-color-font-v1.json"),
    ], check=True)

    asset_transfer_binary = pathlib.Path(directory) / "test_asset_transfer"
    asset_transfer_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        "-I", str(ROOT / "components/vk_assets/include"),
        "-I", str(ROOT / "components/vk_asset_transfer/include"),
        str(ROOT / "tests/native/test_asset_transfer.c"),
        str(ROOT / "components/vk_asset_transfer/vk_asset_transfer.c"),
        str(ROOT / "components/vk_assets/vk_asset_store.c"),
        "-o", str(asset_transfer_binary),
    ]
    subprocess.run(asset_transfer_command, check=True)
    subprocess.run([str(asset_transfer_binary)], check=True)

    led_binary = pathlib.Path(directory) / "test_led"
    led_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_led/include"),
        str(ROOT / "tests/native/test_led.c"),
        str(ROOT / "components/vk_led/vk_led.c"),
        str(ROOT / "components/vk_led/vk_led_board_adapter.c"),
        "-o", str(led_binary),
    ]
    subprocess.run(led_command, check=True)
    subprocess.run([str(led_binary)], check=True)

    usb_led_binary = pathlib.Path(directory) / "test_usb_led_protocol"
    usb_led_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_led_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_led_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        "-o", str(usb_led_binary),
    ]
    subprocess.run(usb_led_command, check=True)
    subprocess.run([str(usb_led_binary)], check=True)

    update_binary = pathlib.Path(directory) / "test_update"
    update_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "components/vk_update/include"),
        str(ROOT / "tests/native/test_update.c"),
        str(ROOT / "components/vk_update/vk_update.c"),
        "-o", str(update_binary),
    ]
    subprocess.run(update_command, check=True)
    subprocess.run([str(update_binary)], check=True)

    input_binary = pathlib.Path(directory) / "test_input_debounce"
    input_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_input/include"),
        str(ROOT / "tests/native/test_input_debounce.c"),
        str(ROOT / "components/vk_input/vk_input_debounce.c"),
        "-o", str(input_binary),
    ]
    subprocess.run(input_command, check=True)
    subprocess.run([str(input_binary)], check=True)

    input_production_binary = pathlib.Path(directory) / "test_input_production"
    input_production_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DVK_INPUT_NATIVE_TEST=1", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_input/include"),
        "-I", str(ROOT / "components/vk_audio/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_input_production.c"),
        str(ROOT / "components/vk_input/vk_input.c"),
        str(ROOT / "components/vk_input/vk_input_debounce.c"),
        "-o", str(input_production_binary),
    ]
    subprocess.run(input_production_command, check=True)
    subprocess.run([str(input_production_binary)], check=True)

    standard_microphone_input_binary = pathlib.Path(directory) / "test_input_standard_microphone"
    standard_microphone_input_command = input_production_command.copy()
    standard_microphone_input_command.insert(5, "-DVK_INPUT_STANDARD_MICROPHONE_TEST=1")
    standard_microphone_input_command[-1] = str(standard_microphone_input_binary)
    subprocess.run(standard_microphone_input_command, check=True)
    subprocess.run([str(standard_microphone_input_binary)], check=True)

    usb_input_binary = pathlib.Path(directory) / "test_usb_input_integration"
    usb_input_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DVK_INPUT_NATIVE_TEST=1", "-DVK_USB_NATIVE_TEST=1", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_input/include"),
        "-I", str(ROOT / "components/vk_audio/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_input_integration.c"),
        str(ROOT / "components/vk_input/vk_input.c"),
        str(ROOT / "components/vk_input/vk_input_debounce.c"),
        str(ROOT / "components/vk_usb/vk_usb_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_usb/vk_usb_capabilities.c"),
        str(ROOT / "components/vk_usb/vk_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_led_protocol.c"),
        "-o", str(usb_input_binary),
    ]
    subprocess.run(usb_input_command, check=True)
    subprocess.run([str(usb_input_binary)], check=True)

    usb_binary = pathlib.Path(directory) / "test_usb"
    usb_command = [
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DVK_USB_NATIVE_TEST=1",
        "-pthread",
        "-I",
        str(ROOT / "tests/native/include"),
        "-I",
        str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb.c"),
        str(ROOT / "components/vk_usb/vk_usb_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_usb/vk_usb_capabilities.c"),
        str(ROOT / "components/vk_usb/vk_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_led_protocol.c"),
        "-o",
        str(usb_binary),
    ]
    subprocess.run(usb_command, check=True)
    subprocess.run([str(usb_binary)], check=True)

    usb_runtime_binary = pathlib.Path(directory) / "test_usb_runtime"
    usb_runtime_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_runtime.c"),
        str(ROOT / "components/vk_usb/vk_usb_runtime.c"),
        "-o", str(usb_runtime_binary),
    ]
    subprocess.run(usb_runtime_command, check=True)
    subprocess.run([str(usb_runtime_binary)], check=True)

    usb_facade_binary = pathlib.Path(directory) / "test_usb_facade"
    usb_facade_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_facade.c"),
        str(ROOT / "components/vk_usb/vk_usb_facade.c"),
        "-o", str(usb_facade_binary),
    ]
    subprocess.run(usb_facade_command, check=True)
    subprocess.run([str(usb_facade_binary)], check=True)

    usb_owner_binary = pathlib.Path(directory) / "test_usb_owner"
    usb_owner_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_owner.c"),
        str(ROOT / "components/vk_usb/vk_usb_owner.c"),
        "-o", str(usb_owner_binary),
    ]
    subprocess.run(usb_owner_command, check=True)
    subprocess.run([str(usb_owner_binary)], check=True)

    usb_production_binary = pathlib.Path(directory) / "test_usb_production"
    usb_production_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DVK_USB_PRODUCTION_NATIVE_TEST=1", "-DVK_USB_NATIVE_TEST=1", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_usb_production.c"),
        str(ROOT / "components/vk_usb/vk_usb.c"),
        str(ROOT / "components/vk_usb/vk_usb_runtime.c"),
        str(ROOT / "components/vk_usb/vk_usb_owner.c"),
        str(ROOT / "components/vk_usb/vk_usb_facade.c"),
        str(ROOT / "components/vk_usb/vk_usb_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_usb/vk_usb_capabilities.c"),
        str(ROOT / "components/vk_usb/vk_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_led_protocol.c"),
        "-o", str(usb_production_binary),
    ]
    subprocess.run(usb_production_command, check=True)
    subprocess.run([str(usb_production_binary)], check=True)
    subprocess.run([str(usb_production_binary), "concurrent-stop"], check=True)
    subprocess.run([str(usb_production_binary), "precommit-stop"], check=True)
    subprocess.run([str(usb_production_binary), "precommit-replace"], check=True)
    subprocess.run([str(usb_production_binary), "timeout"], check=True)
    subprocess.run([str(usb_production_binary), "cleanup-failure"], check=True)

    audio_production_binary = pathlib.Path(directory) / "test_audio_production"
    audio_production_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DVK_AUDIO_NATIVE_TEST=1", "-pthread",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_audio/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/test_audio_production.c"),
        str(ROOT / "components/vk_audio/vk_audio.c"),
        str(ROOT / "components/vk_audio/vk_audio_backend.c"),
        str(ROOT / "components/vk_audio/vk_audio_pipeline.c"),
        str(ROOT / "components/vk_audio/vk_audio_runtime.c"),
        "-o", str(audio_production_binary),
    ]
    subprocess.run(audio_production_command, check=True)
    subprocess.run([str(audio_production_binary)], check=True)
    subprocess.run([str(audio_production_binary), "startup-timeout"], check=True)
    subprocess.run([str(audio_production_binary), "stop-timeout"], check=True)
    subprocess.run([str(audio_production_binary), "runtime-failure"], check=True)
    subprocess.run([str(audio_production_binary), "abort-no-eos"], check=True)
    subprocess.run([str(audio_production_binary), "system-mic"], check=True)
    subprocess.run([str(audio_production_binary), "cleanup-disable"], check=True)
    subprocess.run([str(audio_production_binary), "cleanup-destroy"], check=True)
    subprocess.run([str(audio_production_binary), "cleanup-persistent"], check=True)
    subprocess.run([str(audio_production_binary), "start-stop"], check=True)
    subprocess.run([str(audio_production_binary), "concurrency"], check=True)

    audio_pipeline_binary = pathlib.Path(directory) / "test_audio_pipeline"
    audio_pipeline_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_audio/include"),
        str(ROOT / "tests/native/test_audio_pipeline.c"),
        str(ROOT / "components/vk_audio/vk_audio_pipeline.c"),
        "-o", str(audio_pipeline_binary),
    ]
    subprocess.run(audio_pipeline_command, check=True)
    subprocess.run([str(audio_pipeline_binary)], check=True)

    audio_backend_binary = pathlib.Path(directory) / "test_audio_backend"
    audio_backend_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_audio/include"),
        str(ROOT / "tests/native/test_audio_backend.c"),
        str(ROOT / "components/vk_audio/vk_audio_backend.c"),
        str(ROOT / "components/vk_audio/vk_audio_pipeline.c"),
        "-o", str(audio_backend_binary),
    ]
    subprocess.run(audio_backend_command, check=True)
    subprocess.run([str(audio_backend_binary)], check=True)

    audio_runtime_binary = pathlib.Path(directory) / "test_audio_runtime"
    audio_runtime_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_audio/include"),
        str(ROOT / "tests/native/test_audio_runtime.c"),
        str(ROOT / "components/vk_audio/vk_audio_runtime.c"),
        "-o", str(audio_runtime_binary),
    ]
    subprocess.run(audio_runtime_command, check=True)
    subprocess.run([str(audio_runtime_binary)], check=True)

    golden_generator = pathlib.Path(directory) / "generate_usb_goldens"
    golden_command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DVK_USB_NATIVE_TEST=1",
        "-I", str(ROOT / "tests/native/include"),
        "-I", str(ROOT / "components/vk_usb/include"),
        str(ROOT / "tests/native/generate_usb_goldens.c"),
        str(ROOT / "components/vk_usb/vk_usb_service.c"),
        str(ROOT / "components/vk_usb/vk_usb_json.c"),
        str(ROOT / "components/vk_usb/vk_usb_capabilities.c"),
        str(ROOT / "components/vk_usb/vk_usb_asset_protocol.c"),
        str(ROOT / "components/vk_usb/vk_usb_led_protocol.c"),
        "-o", str(golden_generator),
    ]
    subprocess.run(golden_command, check=True)
    generated_golden = pathlib.Path(directory) / "replacement-handshake.bin"
    subprocess.run([str(golden_generator), str(generated_golden)], check=True)
    checked_golden = ROOT.parent / "mac/Tests/VibeBoardKitTests/Fixtures/replacement-handshake.bin"
    if generated_golden.read_bytes() != checked_golden.read_bytes():
        raise SystemExit("checked-in replacement handshake golden is stale")
