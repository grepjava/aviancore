# Releases

A change someone building on aviancore would notice gets a line under
Unreleased in the commit that makes it. When a version is tagged, that section
is renamed to the version and its date.

Before tagging, run `swift test`, `bash scripts/cache-unit-test.sh` and
`bash scripts/bus-unit-test.sh` here, and
the unit and end-to-end suites of Garuda and Peregrine against the new version.

## 0.3.0 — 2026-09-17

- `av_bus_*`, the broadcast ring: messages published by any process that maps
  it before the fork, read back by number by every other, with no lock held
  across processes. Numbers start at the time the ring was mapped, a message
  stays readable until newer ones write over it, a writer that died holding the
  ring is taken over, and each process has a wake descriptor that a publisher
  writes to only when that process has armed it.

## 0.2.0 — 2026-09-17

- `av_dec_*` and `ContentDecoder` decode response bodies: gzip (several
  members too), deflate as zlib or raw, and brotli and zstd when their
  libraries load. The decoded size is held to a limit as it grows, and
  `ContentDecoder.acceptEncoding` names the codings this process can decode.

## 0.1.1 — 2026-09-17

- `WebSocketCodec.isSendableCloseCode` refuses 1004, which RFC 6455 reserves,
  so a close frame carrying it is a protocol error. Autobahn's case 7.9.3
  failed on it.
- `scripts/gen-hpack-tables.py` generates `HPACKTables.swift` here, beside the
  QPACK generator.

## 0.1.0 — 2026-09-17

The first release. These layers were in Garuda and Peregrine, and each repository
had its own copy. This package merges the two copies into one:

- From Garuda: the outbound TLS client and connect, the executable probe and
  exec used by `--reload`, HPACK request pseudo-headers, the HTTP/1.1 response
  parser's `badStatusLine`, and a fix to QUIC streams. A stream reset before it
  had sent anything could be forgotten before its RESET_STREAM went out.
- From Peregrine: threads, mutexes and condition variables, `av_local_addr`,
  `Log.raw`, a raw status line for the response writer, and response caching
  that varies on Accept-Encoding and honours a request's `max-age` and
  `min-fresh`.
- `HTTPResponseWriter.writeError` takes the `Server` header's value as
  `serverName`.
- C names are `av_` and `AV_`, headers `avian_*.h`, and the environment
  variables `AVIAN_UDP_GSO` and `AVIAN_NO_OPENAT2`.
