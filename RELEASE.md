# Releases

A change someone building on aviancore would notice gets a line under
Unreleased in the commit that makes it. When a version is tagged, that section
is renamed to the version and its date.

`.github/workflows/ci.yml` runs `swift test` on Linux and macOS and both C
unit suites on every push to `main`, every pull request, and nightly. Before
tagging, run the unit and end-to-end suites of Garuda and Peregrine against
the new version as well: what they exercise here is only what this package
tests of itself.

## Unreleased

- `AVIAN_NO_GREEDY=1` in the environment puts the TLS record layer back on a
  plain socket BIO, giving up the read-ahead and returning to two reads a
  record. It exists so the greedy BIO can be measured against itself in one
  binary, where two builds would differ in more than the one branch, and it
  doubles as a way out if the BIO ever misbehaves against a particular peer.
  Measured with it: the BIO is worth about 1% of throughput on a keep-alive
  workload and about 4% of CPU a request over HTTP/2, and it is **not** the
  cause of the HTTP/2 deficit recorded in Garuda's BENCHMARKS.md -- switching
  it off makes HTTP/2 slower, not faster.

## 0.7.1 — 2026-09-20

- **`av_tls_flush_control` puts pending TLS control messages on the wire
  without an application write.** BoringSSL does not send its session ticket
  at the end of the handshake the way OpenSSL does; it holds the
  NewSessionTicket back until the first application write, so the ticket
  rides out with the response instead of costing a write of its own. That is
  a good trade for a server answering a request, and a problem for a caller
  that wants the ticket on the wire before it has anything to say -- a test
  that checks resumption, or a connection that will sit idle before its first
  request. The new call flushes them (`SSL_write` with a zero length, which
  BoringSSL treats as exactly that), and answers 0 on success. Under OpenSSL
  it succeeds without doing anything, since there is nothing held back.

- The kernel-TLS costs quoted under 0.7.0 are now marked provisional there.
  They came from a run whose arm order was not rotated, and a re-measurement
  that rotates it is outstanding. The direction is not in doubt; the
  magnitudes are not settled.

## 0.7.0 — 2026-09-20

- **TLS is built against BoringSSL now, and a copy of it ships here.** The
  record layer and the handshake are BoringSSL's; `avian_crypto.c`,
  `avian_acme.c` and QUIC's primitives go on calling OpenSSL, which is still
  linked and still required. Only the record layer and the handshake move.

  The copy lives at `Sources/CAvianSSL`, taken from swift-nio-ssl's already
  pre-generated one so that it needs neither CMake nor Go, and re-prefixed
  `CAvianSSL`. That re-prefix is load-bearing: keeping `CNIOBoringSSL` would
  mean an application using both this and anything built on swift-nio-ssl
  linking two different BoringSSL versions that each define
  `CNIOBoringSSL_SSL_new`. `Sources/CAvianSSL/VENDORING.md` has the
  provenance, the BoringSSL revision, and how to take a new release.

  **Vendoring makes this package responsible for a security dependency**:
  when BoringSSL fixes something, it reaches anyone building on aviancore
  only when the copy here is updated.

  Building without `AVIAN_TLS_BORINGSSL` puts TLS back on OpenSSL. That path
  is kept working, and is the only way to compare the two.

  Two libraries can live in one binary only because BoringSSL's symbols carry
  a prefix where OpenSSL's do not.

  The reason to want it is cost. Measured on one pinned core, the same source
  compiled against each library: a handshake **752.6 us against 447.7**, and
  a small record round trip, less the socketpair floor both pay, **5.19 us
  against 3.66**. BoringSSL never adopted OpenSSL 3.x's provider
  architecture, so it has none of the EVP object churn profiling finds in the
  handshake here. Measured again through a server over HTTPS, across
  fourteen workloads: **CPU a request falls on all fourteen and throughput
  rises on thirteen**, `churn` -- a handshake a request -- gaining **40.5%**,
  which is what the probe predicted to a tenth of a point. Garuda's
  BENCHMARKS.md has the table.

  What it gives up is kernel TLS, which BoringSSL does not have. Every use of
  it was already behind `SSL_OP_ENABLE_KTLS`, which BoringSSL does not
  define, so it compiles out on its own: `av_tls_ktls_send`,
  `av_tls_ktls_recv` and `av_tls_release_to_kernel` answer 0, and
  `av_tls_sendfile` encrypts in process. A server that passes `--ktls` still
  starts; the request is accepted and has no effect.

  **What that costs is being measured and the first answer is provisional.**
  Peregrine, one worker, static files: HTTPS/1.1 files go out about **8%
  slower at 1 MiB and 14% slower at 16 MiB**, for about 15% more CPU a
  gibibyte, with nothing lost below about 64 KiB -- BoringSSL is 19% ahead
  there despite having no kernel TLS -- and nothing lost over HTTP/2, whose
  framed bytes could never take `sendfile` anyway. Those rows came from two
  rounds that both ran the baseline first, an order that has demonstrably
  invented an effect of this size elsewhere, so they are held pending a
  four-round run that rotates the leading arm. The direction is what the
  mechanism predicts; the magnitudes are not settled. Anyone serving large
  files over HTTP/1.1 should weigh that against a handshake 40 to 45%
  cheaper, and can build without `AVIAN_TLS_BORINGSSL` to keep OpenSSL and
  kernel TLS.
- Under BoringSSL the socket BIO is replaced by one that reads ahead, because
  BoringSSL will not. `SSL_CTX_set_read_ahead` is one of the calls it keeps
  for compatibility and does nothing with, so its record layer takes a
  record's 5-byte header and its body in two reads where OpenSSL takes one:
  **exactly 2.00 reads a request against 1.00**, counted off a worker while
  it served. The replacement reads whatever the socket has into a buffer and
  answers the header out of it, so the body -- and any record queued behind
  it -- is already in hand, and reads a request are 1.00 again. `av_tls_pending`
  counts what the BIO holds, since those bytes are no longer in the socket and
  a poller would otherwise wait for what has already arrived. If the BIO
  cannot be made, the plain socket BIO is used instead.
- The ACME `tls-alpn-01` ClientHello callback is written once and adapted to
  each library rather than assuming OpenSSL's shape. OpenSSL passes the `SSL`
  and an argument of the caller's choosing; BoringSSL passes an
  `SSL_CLIENT_HELLO` and no argument, so the context carries the wrapper
  itself. Nothing changes under OpenSSL.

## 0.6.7 — 2026-09-20

- The TLS error queue is cleared on the way out of a failure rather than on
  the way in to every call. `SSL_get_error` is only reliable with an empty
  queue, and the usual way to get one is `ERR_clear_error()` before every
  `SSL_read` and `SSL_write`; profiling a server under load put that at 1.37%
  of its whole CPU, more than three times what encrypting the data cost.
  Every path that can leave entries now drains them before it returns, so the
  queue is already empty when the next call starts and the reads and writes
  that succeed pay nothing. Measured on one pinned worker serving HTTPS,
  three runs against three: **76,600 requests a second to 79,100**, and every
  pair favoured it.

## 0.6.6 — 2026-09-19

- TLS reads go ahead: OpenSSL reads what the socket has in one call rather
  than a record's 5-byte header and then its body in two, halving the reads
  an HTTPS request costs. Not under `--ktls`. `av_tls_pending` now also
  counts bytes read ahead and not yet decrypted, so a caller that reads until
  it is zero still finds a pipelined record the socket no longer announces.

## 0.6.5 — 2026-09-19

- `av_sched_set_slice` asks the scheduler to run the calling thread in
  shorter slices (EEVDF's custom slice, Linux 6.12 and later), and
  `av_sched_slice` reads back what it has. A worker that owns its
  connections and loses its CPU to another thread keeps every one of them
  waiting for up to a whole slice, 2.8 ms by default on an 8-CPU machine.
  Elsewhere the call fails with `ENOSYS`, and an older Linux ignores the
  slice.

## 0.6.4 — 2026-09-19

- `av_load_publish_heavy` and `av_load_view.heavy`: how many of a worker's
  connections make requests that hold its loop for a long time. A server can
  then gather those on fewer workers, so that quick requests are not all
  waiting behind one of them. Uses the slot's spare word; the page's size is
  unchanged.

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
