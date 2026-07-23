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
    targets: [
        .target(
            name: "VibeBoardKit",
            linkerSettings: [.linkedFramework("IOKit")]
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
