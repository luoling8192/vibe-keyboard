// swift-tools-version: 6.2

import PackageDescription

let package = Package(
    name: "VibeKeyboard",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .library(name: "VibeBoardKit", targets: ["VibeBoardKit"]),
        .executable(name: "VibeBoardDiagnostic", targets: ["VibeBoardDiagnostic"]),
        .executable(name: "VibeKeyboardApp", targets: ["VibeKeyboardApp"])
    ],
    dependencies: [
    ],
    targets: [
        .target(
            name: "COpus",
            dependencies: [],
            path: "Vendor/opus-c",
            exclude: [
                "AUTHORS",
                "autogen.sh",
                "autogen.bat",
                "celt_headers.mk",
                "celt_sources.mk",
                "celt/arm",
                "celt/dump_modes",
                "celt/meson.build",
                "celt/tests",
                "celt/opus_custom_demo.c",
                "celt/x86",
                "ChangeLog",
                "cmake",
                "CMakeLists.txt",
                "configure.ac",
                "COPYING",
                "create_opus_data.sh",
                "dnn",
                "doc",
                "LICENSE_PLEASE_READ.txt",
                "lpcnet_headers.mk",
                "lpcnet_sources.mk",
                "m4",
                "Makefile.am",
                "Makefile.unix",
                "meson",
                "meson_options.txt",
                "meson.build",
                "NEWS",
                "opus_headers.mk",
                "opus_sources.mk",
                "opus-uninstalled.pc.in",
                "opus.m4",
                "opus.pc.in",
                "README",
                "README.draft",
                "releases.sha2",
                "scripts",
                "silk_headers.mk",
                "silk_sources.mk",
                "silk/arm",
                "silk/fixed",
                "silk/float/x86",
                "silk/mips",
                "silk/tests",
                "silk/x86",
                "silk/meson.build",
                "src/meson.build",
                "src/opus_compare.c",
                "src/opus_demo.c",
                "src/repacketizer_demo.c",
                "src/qext_compare.c",
                "tests",
                "training",
                "update_version",
                "win32",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("."),
                .headerSearchPath("celt"),
                .headerSearchPath("silk"),
                .headerSearchPath("silk/float"),
                .define("OPUS_BUILD"),
                .define("VAR_ARRAYS", to: "1"),
                .define("FLOATING_POINT"),
                .define("HAVE_DLFCN_H", to: "1"),
                .define("HAVE_INTTYPES_H", to: "1"),
                .define("HAVE_LRINT", to: "1"),
                .define("HAVE_LRINTF", to: "1"),
                .define("HAVE_MEMORY_H", to: "1"),
                .define("HAVE_STDINT_H", to: "1"),
                .define("HAVE_STDLIB_H", to: "1"),
                .define("HAVE_STRING_H", to: "1"),
                .define("HAVE_STRINGS_H", to: "1"),
                .define("HAVE_SYS_STAT_H", to: "1"),
                .define("HAVE_SYS_TYPES_H", to: "1"),
                .define("HAVE_UNISTD_H", to: "1"),
            ]
        ),
        .target(
            name: "VibeBoardKit",
            dependencies: ["COpus"],
            linkerSettings: [
                .linkedFramework("IOKit"),
                .linkedFramework("CoreAudio")
            ]
        ),
        .executableTarget(
            name: "VibeBoardDiagnostic",
            dependencies: ["VibeBoardKit"]
        ),
        .executableTarget(
            name: "VibeKeyboardApp",
            dependencies: ["VibeBoardKit"]
        ),
        .testTarget(
            name: "VibeBoardKitTests",
            dependencies: ["VibeBoardKit"],
            resources: [.copy("Fixtures")]
        ),
        .testTarget(
            name: "VibeKeyboardAppTests",
            dependencies: ["VibeKeyboardApp", "VibeBoardKit"]
        )
    ]
)
