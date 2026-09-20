# CAvianSSL — a vendored BoringSSL

This directory is BoringSSL, and none of it is ours. It is here because
aviancore's TLS record layer and handshake are built against BoringSSL rather
than OpenSSL: profiling put 22% of a keep-alive HTTPS request's CPU and 72% of
a handshake's in OpenSSL 3.x's provider bookkeeping — algorithm fetching,
`OSSL_PARAM` lookups, allocate-and-cleanse — which BoringSSL never adopted.
Measured through a server, CPU a request falls on every workload and a
handshake costs about 40% less. Garuda's BENCHMARKS.md has the figures.

## Where it came from

Not from BoringSSL directly, but from **swift-nio-ssl's** vendored copy, which
is already pre-generated and patched for SwiftPM: no CMake, no Go, no build
step of its own. `hash.txt` records the BoringSSL revision, and `LICENSE.txt`
and `NOTICE.txt` are carried with it.

    swift-nio-ssl 2.37.4
    BoringSSL     817ab07ebb53da35afea409ab9328f578492832d

## Why the symbols say CAvianSSL

BoringSSL's public names are prefixed so they cannot collide with a system
OpenSSL, which is what lets `avian_crypto.c` and `avian_acme.c` go on calling
OpenSSL under its own unprefixed symbols in the same binary.

swift-nio-ssl prefixes its copy `CNIOBoringSSL`. **Keeping that prefix would
have been a bug**: an application using both Garuda and something built on
swift-nio-ssl — Vapor, say — would link two different BoringSSL versions both
defining `CNIOBoringSSL_SSL_new`, and get whichever the linker picked. So this
copy is re-prefixed `CAvianSSL` and the two can coexist.

The prefix is not baked into the sources. They are stock BoringSSL, and every
public name is redirected by `BORINGSSL_ADD_PREFIX` macros in
`include/CAvianSSL_boringssl_prefix_symbols.h` against one line in
`include/CAvianSSL_base.h`:

    #define BORINGSSL_PREFIX CAvianSSL

## Updating it

Re-prefixing is textual, because the sources are unmodified. Take
swift-nio-ssl's `Sources/CNIOBoringSSL` at the release wanted, then rename the
files and rewrite the references:

```bash
cp -a <swift-nio-ssl>/Sources/CNIOBoringSSL Sources/CAvianSSL
find Sources/CAvianSSL -depth -name 'CNIOBoringSSL*' | while read f; do
    mv "$f" "$(dirname "$f")/$(basename "$f" | sed 's/CNIOBoringSSL/CAvianSSL/')"
done
grep -rlI CNIOBoringSSL Sources/CAvianSSL | xargs sed -i 's/CNIOBoringSSL/CAvianSSL/g'
```

Then check that nothing is left (`grep -rI CNIOBoringSSL Sources/CAvianSSL`),
that `CAvianSSL_base.h` still defines `BORINGSSL_PREFIX` as `CAvianSSL`, and
copy `hash.txt`, `LICENSE.txt` and `NOTICE.txt` across so this file can be
brought up to date.

**This directory is a security dependency.** Vendoring means aviancore, not a
distribution, decides when a BoringSSL fix reaches anyone building on it. Track
upstream and take releases promptly.
