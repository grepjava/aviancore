# Releases

A change someone building on aviancore would notice gets a line under
Unreleased in the commit that makes it. When a version is tagged, that section
is renamed to the version and its date.

Before tagging, run `swift test` and `bash scripts/cache-unit-test.sh` here, and
the unit and end-to-end suites of Garuda and Peregrine against the new version.

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
