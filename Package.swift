// swift-tools-version: 6.1
import PackageDescription

// AvianCore — the protocol and systems layers shared by Swift servers:
// syscalls, TLS and compression in C; buffers and the poller; HTTP/1.1, HTTP/2,
// HTTP/3, WebSocket framing and QUIC. No Foundation.
//
// No target sets unsafe flags: SwiftPM refuses them in a package that is
// depended on by version. A server that wants exclusivity checking off in
// release builds passes it on the command line:
//   swift build -c release -Xswiftc -enforce-exclusivity=unchecked

let swiftSettings: [SwiftSetting] = [
    .swiftLanguageMode(.v6),
]

let package = Package(
    name: "aviancore",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "CAvian", targets: ["CAvian"]),
        .library(name: "CAvianSSL", targets: ["CAvianSSL"]),
        .library(name: "AvianCore", targets: ["AvianCore"]),
        .library(name: "AvianHTTP", targets: ["AvianHTTP"]),
        .library(name: "AvianQUIC", targets: ["AvianQUIC"]),
    ],
    targets: [
        // Syscall wrappers (epoll/kqueue, sockets, sendfile, clock), threads,
        // TLS, compression, caches. Feature-test macros are set inside the .c
        // files, never here: a define that reaches the module build would change
        // glibc struct layouts relative to SwiftGlibc.
        // BoringSSL, vendored. Sources/CAvianSSL/VENDORING.md says where it
        // came from, why its symbols are prefixed CAvianSSL rather than
        // swift-nio-ssl's CNIOBoringSSL, and how to take a new release.
        .target(
            name: "CAvianSSL",
            cSettings: [
                .define("_GNU_SOURCE"),
                .define("_POSIX_C_SOURCE", to: "200112L"),
                .define("_DARWIN_C_SOURCE"),
                // Recent Windows SDKs pull in headers whose symbols collide
                // with BoringSSL's: the legacy <winsock.h>, the min()/max()
                // macros, and <wincrypt.h>. Suppress just those.
                .define("_WINSOCKAPI_", .when(platforms: [.windows])),
                .define("NOMINMAX", .when(platforms: [.windows])),
                .define("NOCRYPT", .when(platforms: [.windows])),
            ],
            cxxSettings: [
                .define("_WINSOCKAPI_", .when(platforms: [.windows])),
                .define("NOMINMAX", .when(platforms: [.windows])),
                .define("NOCRYPT", .when(platforms: [.windows])),
            ]
        ),

        .target(
            name: "CAvian",
            dependencies: ["CAvianSSL"],
            cSettings: [
                .headerSearchPath("include"),
                // The TLS record layer and the handshake are BoringSSL's: it
                // spends much less on a handshake and on a small record,
                // never having adopted OpenSSL 3.x's provider architecture.
                // Everything else that wants a primitive -- avian_crypto.c,
                // avian_acme.c, QUIC -- goes on calling OpenSSL, which is
                // possible in one binary only because BoringSSL's symbols
                // carry a prefix where OpenSSL's do not.
                .define("AVIAN_TLS_BORINGSSL"),
            ],
            // OpenSSL still supplies the primitives BoringSSL has no
            // equivalent for. gzip links; brotli and zstd are opened at run
            // time, so a machine without them still runs.
            linkerSettings: [
                .linkedLibrary("ssl"),
                .linkedLibrary("crypto"),
                .linkedLibrary("z"),
            ]
        ),

        .target(name: "AvianCore", dependencies: ["CAvian"],
                swiftSettings: swiftSettings),

        .target(name: "AvianHTTP", dependencies: ["AvianCore"],
                swiftSettings: swiftSettings),

        // QUIC and its TLS 1.3 handshake. QUIC replaces the TLS record layer,
        // so OpenSSL is used here only for primitives -- hash, HKDF, AEAD, key
        // agreement, signature -- and the protocol above them is ours.
        .target(name: "AvianQUIC", dependencies: ["AvianCore", "AvianHTTP"],
                swiftSettings: swiftSettings),

        .testTarget(name: "AvianTests",
                    dependencies: ["CAvian", "AvianCore", "AvianHTTP", "AvianQUIC"],
                    swiftSettings: swiftSettings),
    ],
    cLanguageStandard: .gnu11,
    // BoringSSL is C++.
    cxxLanguageStandard: .cxx17
)
