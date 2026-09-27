// swift-tools-version: 5.9
// win32swiftui.so: the unix half of Win32-to-SwiftUI. A wine unix library (the
// C target exports __wine_unix_call_funcs) whose calls land in SwiftUI (W2SKit).
import PackageDescription

let package = Package(
    name: "Win32ToSwiftUI",
    platforms: [.macOS(.v12)],
    products: [
        .library(name: "win32swiftui", type: .dynamic, targets: ["W2SUnix", "W2SKit"]),
    ],
    targets: [
        .target(
            name: "W2SUnix",
            path: "Sources/W2SUnix",
            cSettings: [.headerSearchPath("../../include")]
        ),
        .target(
            name: "W2SKit",
            path: "Sources/W2SKit",
            swiftSettings: [.unsafeFlags(["-swift-version", "5"])]
        ),
    ]
)
