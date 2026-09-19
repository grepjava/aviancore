/* TLS, wrapped so that OpenSSL headers never reach the Swift side.
 *
 * The Swift module map imports these headers, and
 * anything with a packed struct, a macro-heavy API or a feature-test macro of
 * its own would leak into every target that imports CAvian. So the types
 * here are opaque and the API is the eight operations the server needs.
 *
 * The read and write wrappers deliberately look like read(2) and write(2):
 * they return a byte count, or -1 with errno set to EAGAIN when OpenSSL needs
 * more of the socket. That lets the connection loop keep one code path for
 * plaintext and TLS instead of two.
 */
#ifndef AVIAN_TLS_H
#define AVIAN_TLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct av_tls_ctx av_tls_ctx;
typedef struct av_tls av_tls;

/* Whether this binary was built against OpenSSL at all. */
int av_tls_available(void);

/* Creates a server context. `alpn` is a comma-separated preference list, most
 * preferred first, e.g. "h2,http/1.1"; NULL or "" disables ALPN. On failure
 * returns NULL and writes a human-readable reason into `err`. */
av_tls_ctx *av_tls_ctx_new(const char *cert_path, const char *key_path,
                           const char *alpn, const char *ciphers,
                           char *err, size_t err_len);
/* Adds another certificate, for SNI. The first one given to av_tls_ctx_new is
 * the default; these are chosen by the name the client asks for, matched
 * against the DNS names inside each certificate. Returns 0 and fills `err` on
 * failure. */
int av_tls_ctx_add(av_tls_ctx *ctx, const char *cert_path, const char *key_path,
                   const char *ciphers, char *err, size_t err_len);

/* How many certificates are loaded, and the names of each -- for the start-up
 * log, so an operator can see what the server believes it can serve. Returns 0
 * when the index is past the end. */
int av_tls_ctx_host_count(av_tls_ctx *ctx);
int av_tls_ctx_names(av_tls_ctx *ctx, int host_index, int name_index,
                     char *out, size_t out_len);

void av_tls_ctx_free(av_tls_ctx *ctx);

/* A context for connections this process makes, rather than accepts.
 *
 * Verification is the whole point of it: the peer's chain is checked against
 * `ca_file`, or against the system trust store when that is NULL. `alpn` is
 * the same comma-separated list `av_tls_ctx_new` takes, offered in order.
 * Returns NULL and fills `err` on failure. */
av_tls_ctx *av_tls_client_ctx_new(const char *ca_file, const char *alpn,
                                  char *err, size_t err_len);

/* A client session on `fd`, for `hostname`.
 *
 * The name is used twice, and both uses matter: as SNI, saying which
 * certificate to send, and as the name the certificate is checked against.
 * Without the second, verification proves only that some CA somewhere signed
 * some certificate -- which is the quiet hole that makes TLS look like it is
 * working while it protects nothing. */
av_tls *av_tls_client_new(av_tls_ctx *ctx, int fd, const char *hostname);

av_tls *av_tls_new(av_tls_ctx *ctx, int fd);
void av_tls_free(av_tls *tls);

/* Handshake progress: 1 done, 0 needs more input, -1 needs to write, -2 failed.
 * A failure reason, when there is one, is written into `err`. */
int av_tls_handshake(av_tls *tls, char *err, size_t err_len);

/* read(2)/write(2) shaped. -1 with errno EAGAIN means "not yet"; errno EPIPE
 * means the session is over. A read of 0 is a clean close_notify. */
long av_tls_read(av_tls *tls, void *buf, long n);
long av_tls_write(av_tls *tls, const void *buf, long n);

/* --ktls: ask for kernel TLS on every context built after this call. Returns
 * 1 when this OpenSSL can do it at all, 0 when the request is ignored. */
int av_tls_enable_ktls(int on);

/* 1 when the kernel's tls module is loaded, without which no session gets
 * kernel TLS whatever OpenSSL is asked. */
int av_tls_kernel_ready(void);

/* 1 when the kernel encrypts what this session sends (kernel TLS), which is
 * what lets a file go from the page cache to the socket without being copied
 * through here. */
int av_tls_ktls_send(av_tls *tls);

/* 1 when the kernel also decrypts what this session receives. */
int av_tls_ktls_recv(av_tls *tls);

/* Gives the session up to the kernel, for a connection about to be handed to
 * another worker process: an OpenSSL session is memory in this one and cannot
 * go with it, but once the kernel encrypts and decrypts both ways the socket
 * carries the whole of TLS. Succeeds, freeing `tls` without a word on the
 * wire, only when both directions are the kernel's and OpenSSL holds nothing
 * read or half-written; 0 leaves `tls` as it was. After it, the descriptor is
 * read and written with read(2) and write(2). A record that is not
 * application data -- an alert, or a key update the kernel cannot apply --
 * fails the read with EIO, and ends the connection. */
int av_tls_release_to_kernel(av_tls *tls);

/* Sends close_notify on a connection whose TLS is the kernel's alone. 0, or
 * -1 with errno. */
int av_ktls_close_notify(int fd);

/* SSL_sendfile: `n` bytes of `fd` starting at `offset`, encrypted by the
 * kernel. Shaped like av_tls_write. Only when av_tls_ktls_send says 1. */
long av_tls_sendfile(av_tls *tls, int fd, long offset, long n);

/* Decrypted bytes OpenSSL is holding that the socket no longer has. A
 * level-triggered poller will not mention these, so anything that reads has to
 * keep asking until this is zero. */
int av_tls_pending(av_tls *tls);

/* An idle pooled connection had something to say. 1 when it was only
 * post-handshake bookkeeping -- a session ticket, a key update -- and the
 * connection is still good to hand to the next caller; 0 when application data
 * is waiting or the session is over, neither of which belongs in a pool.
 *
 * A TLS 1.3 server sends a ticket straight after the handshake, so without
 * this every pooled session looks like a peer hanging up and no encrypted
 * connection is ever reused: a fresh handshake for every request. */
int av_tls_idle_ok(av_tls *tls);

/* Set when the last call could not proceed until the socket is writable,
 * which is how a renegotiation or a key update surfaces mid-read. */
int av_tls_wants_write(av_tls *tls);

/* 1 when ALPN settled on HTTP/2. */
int av_tls_is_h2(av_tls *tls);

/* The SNI name a client sent, on a server session after its handshake, or
 * NULL when it sent none. Owned by the session. */
const char *av_tls_server_name(av_tls *tls);

/* The connection negotiated acme-tls/1: an ACME CA validating a tls-alpn-01
 * challenge, which wants the handshake and nothing after it. */
int av_tls_is_acme(av_tls *tls);

/* Turns on tls-alpn-01 answering, with challenge certificates read from
 * <dir>/alpn/NAME.crt and .key as connections ask for them. 0 on success. */
int av_tls_ctx_set_acme_dir(av_tls_ctx *ctx, const char *dir);

/* Best-effort close_notify. Never blocks. */
void av_tls_shutdown(av_tls *tls);

#ifdef __cplusplus
}
#endif

#endif /* AVIAN_TLS_H */
