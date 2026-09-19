# Releases

A change someone building on aviancore would notice gets a line under
Unreleased in the commit that makes it. When a version is tagged, that section
is renamed to the version and its date.

`.github/workflows/ci.yml` runs `swift test` on Linux and macOS and both C
unit suites on every push to `main`, every pull request, and nightly. Before
tagging, run the unit and end-to-end suites of Garuda and Peregrine against
the new version as well: what they exercise here is only what this package
tests of itself.

## 0.6.3 — 2026-09-19

- `av_load_awake` records when a worker began its current turn of work, and
  `av_load_view.stalled` says a worker has been on one turn for 20 ms or more,
  with its busy reading forced to 1000. A worker whose handler holds its loop
  publishes nothing meanwhile, and its last reading could be that it was idle
  and accepting: everyone else would keep leaving it connections it was not
  going to take.

## 0.6.2 — 2026-09-19

Two more readings on the load page, which spreading connections turned out
to need.

- `av_load_accepting` publishes whether a worker is watching a listener it
  shares, and `av_load_view.accepting` reads it. A worker that leaves a new
  connection to a less loaded one needs to know that one is there to take it:
  without it, workers each deferring to another that had just done the same
  left connections queued with every worker idle.
- `av_load_publish_wait` publishes how long a request arriving now would
  wait at a worker, and `av_load_view.wait_us` reads it. A worker moving
  connections needs somewhere they will be served sooner, and busyness alone
  does not say where: half busy with two-millisecond requests keeps a quick
  one waiting longer than nearly flat out with quick ones.

## 0.6.0 — 2026-09-19

What a server needs to spread connections evenly over worker processes.

- `avian_load.h`, a page mapped before the fork where each worker publishes
  how busy its loop is and how many connections it holds, and reads the
  others'. A worker that is waiting says since when, and a reader discounts
  its reading to nothing over 20 ms, since an idle loop does not turn to
  refresh it. `av_load_reap` clears whatever slot a pid held, for a
  supervisor whose worker crashed without saying so.
- `av_poll_add_exclusive` registers a descriptor several pollers share -- a
  listener every worker accepts from -- with `EPOLLEXCLUSIVE`, so that the
  kernel wakes one waiting poller for it rather than all of them. It is a
  plain add on kqueue. `Poller.addExclusive` is the Swift side.
- `av_handoff_pair`, `av_send_fd` and `av_recv_fd` pass a connection's
  descriptor and a short note from one process to another over a datagram
  unix socket (`SCM_RIGHTS`).
- `av_tls_ktls_recv` says whether the kernel decrypts what a session
  receives. `av_tls_release_to_kernel` gives a session whose TLS the kernel
  carries both ways up to it, freeing the OpenSSL side without a word on the
  wire, so that the descriptor can go to another process with all of TLS in
  it. `av_ktls_close_notify` then sends `close_notify` on such a socket.
- Three metrics indices: connections handed off, connections taken over, and
  accepts deferred. They come before the duration counters and shift their
  numbers; code that uses the names needs only a rebuild.

## 0.5.0 — 2026-09-19

- `LoopExecutor`, a task executor for a thread that runs its own event loop.
  A task that prefers it runs only on the loop's thread, inline, when the
  loop calls `drain()`; a task resumed from any other thread is handed over
  under a lock and wakes the loop through `wakeFD`, a pipe for its poller.
  It was Garuda's per-worker executor, and is here so that every server on
  this loop runs its async code the same way rather than keeping a copy.
- The broadcast ring's tests compile on macOS. One line used `fork()`, which
  Swift marks unavailable on Darwin, and it failed the whole file: no test in
  it had ever run there. It only ever wanted the ID of a process that has
  finished, which `posix_spawn` gives portably. CI, which now runs without
  being asked, is what found it.

## 0.4.0 — 2026-09-18

- `QUICServerConfig` takes several certificates and keys, and a QUIC
  handshake serves the one whose names cover the name in the client's SNI
  extension, or the first when none does. HTTP/3 served the default
  certificate whatever a client asked for: the handshake here is written from
  the primitives rather than driven by OpenSSL, so it had no selection of its
  own, where TCP had OpenSSL's callback doing it.
- `av_certkey_matches` says whether a certificate is one to serve for a name,
  by its subject alternative names or, when it carries none, its common name.
  The name is passed as bytes and a length, so it can come straight out of a
  ClientHello.
- `av_host_matches` is the RFC 6125 name rule, and both certificate paths now
  call it rather than the TCP path keeping its own copy: a wildcard covers
  exactly one label, matching is case-insensitive, and `*.example.com` is
  neither `a.b.example.com` nor `example.com`. A pattern of `*.` alone now
  matches nothing, where it used to match any one-label name ending in a dot.

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
