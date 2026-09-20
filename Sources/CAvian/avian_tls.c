/* TLS over the existing non-blocking socket loop.
 *
 * OpenSSL is handed the descriptor directly (SSL_set_fd) rather than driven
 * through memory BIOs. With a non-blocking socket that gives exactly the
 * behaviour the rest of the server already handles: a short read or write, or
 * EAGAIN. The wrappers below translate SSL_ERROR_WANT_* into errno so the
 * connection loop needs no TLS-specific error handling.
 *
 * Reads go ahead (SSL_CTX_set_read_ahead): OpenSSL reads whatever the socket
 * has, up to a whole record and more, in one call, where without it every
 * record costs two -- its 5-byte header, then the rest. What it reads beyond
 * the record it returns stays in its buffer, which av_tls_pending counts: the
 * socket will not say it is readable for bytes it no longer has. Not under
 * --ktls, where records OpenSSL has already read when the handshake ends
 * would keep the kernel from taking over the receiving side.
 *
 * The error queue is cleared on the way out of a failure, not on the way in
 * to every call. SSL_get_error is only reliable with an empty queue, and the
 * usual way to get one is ERR_clear_error() before every SSL_read and
 * SSL_write -- which measured 1.37% of this server's whole CPU under load,
 * more than three times what encrypting the data cost. Every path below that
 * can leave entries drains them before it returns, so the queue is already
 * empty when the next call starts and the reading and writing that succeed
 * pay nothing.
 *
 * Partial writes are enabled deliberately. Without SSL_MODE_ENABLE_PARTIAL_WRITE
 * a write that cannot be completed must be retried with the identical buffer,
 * which a ring of connection buffers cannot promise; with it, and with
 * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, SSL_write behaves like write(2).
 */

#define _GNU_SOURCE 1

#include "avian_tls.h"
#include "avian_crypto.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(__has_include)
#  if !__has_include(<openssl/ssl.h>)
#    define AV_NO_OPENSSL 1
#  endif
#endif

#ifdef AV_NO_OPENSSL

int av_tls_available(void) { return 0; }
av_tls_ctx *av_tls_ctx_new(const char *cert_path, const char *key_path,
                           const char *alpn, const char *ciphers,
                           char *err, size_t err_len) {
    (void)cert_path; (void)key_path; (void)alpn; (void)ciphers;
    if (err && err_len) {
        snprintf(err, err_len, "this binary was built without OpenSSL");
    }
    return NULL;
}
int av_tls_ctx_add(av_tls_ctx *ctx, const char *cert_path, const char *key_path,
                   const char *ciphers, char *err, size_t err_len) {
    (void)ctx; (void)cert_path; (void)key_path; (void)ciphers;
    if (err && err_len) snprintf(err, err_len, "this binary was built without OpenSSL");
    return 0;
}
int av_tls_ctx_host_count(av_tls_ctx *ctx) { (void)ctx; return 0; }
int av_tls_ctx_names(av_tls_ctx *ctx, int host_index, int name_index,
                     char *out, size_t out_len) {
    (void)ctx; (void)host_index; (void)name_index; (void)out; (void)out_len;
    return 0;
}
void av_tls_ctx_free(av_tls_ctx *ctx) { (void)ctx; }
av_tls *av_tls_new(av_tls_ctx *ctx, int fd) { (void)ctx; (void)fd; return NULL; }
av_tls_ctx *av_tls_client_ctx_new(const char *ca_file, const char *alpn,
                                  char *err, size_t err_len) {
    (void)ca_file; (void)alpn; (void)err; (void)err_len; return NULL;
}
av_tls *av_tls_client_new(av_tls_ctx *ctx, int fd, const char *hostname) {
    (void)ctx; (void)fd; (void)hostname; return NULL;
}
void av_tls_free(av_tls *tls) { (void)tls; }
int av_tls_handshake(av_tls *tls, char *err, size_t err_len) {
    (void)tls; (void)err; (void)err_len; return -2;
}
long av_tls_read(av_tls *tls, void *buf, long n) {
    (void)tls; (void)buf; (void)n; errno = EPIPE; return -1;
}
long av_tls_write(av_tls *tls, const void *buf, long n) {
    (void)tls; (void)buf; (void)n; errno = EPIPE; return -1;
}
int av_tls_enable_ktls(int on) { (void)on; return 0; }
int av_tls_kernel_ready(void) { return 0; }
int av_tls_ktls_send(av_tls *tls) { (void)tls; return 0; }
int av_tls_ktls_recv(av_tls *tls) { (void)tls; return 0; }
int av_tls_release_to_kernel(av_tls *tls) { (void)tls; return 0; }
long av_tls_sendfile(av_tls *tls, int fd, long offset, long n) {
    (void)tls; (void)fd; (void)offset; (void)n; errno = EPIPE; return -1;
}
int av_tls_pending(av_tls *tls) { (void)tls; return 0; }
int av_tls_flush_control(av_tls *tls) { (void)tls; errno = EPIPE; return -1; }
int av_tls_idle_ok(av_tls *tls) { (void)tls; return 0; }
int av_tls_wants_write(av_tls *tls) { (void)tls; return 0; }
int av_tls_is_h2(av_tls *tls) { (void)tls; return 0; }
const char *av_tls_server_name(av_tls *tls) { (void)tls; return NULL; }
int av_tls_is_acme(av_tls *tls) { (void)tls; return 0; }
int av_tls_ctx_set_acme_dir(av_tls_ctx *ctx, const char *dir) { (void)ctx; (void)dir; return -1; }
void av_tls_shutdown(av_tls *tls) { (void)tls; }

#else

#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>

/* AVIAN_TLS_BORINGSSL builds the record layer and the handshake against
 * BoringSSL instead of OpenSSL, and Package.swift defines it: BoringSSL is
 * what this package uses for TLS unless someone deliberately turns it off.
 * It spends much less on a handshake and on a small record, never having
 * adopted OpenSSL 3.x's provider architecture and so none of the EVP object
 * churn profiling finds there.
 *
 * The BoringSSL is the copy vendored at Sources/CAvianSSL, whose symbols
 * carry a CAvianSSL prefix. That prefix is what makes this a choice one file
 * can make: avian_crypto.c and avian_acme.c go on calling OpenSSL, under its
 * own unprefixed symbols, in the same binary. So this moves the TLS record
 * layer and nothing else -- not JWT, not ACME's own client, not QUIC's
 * primitives, which use APIs BoringSSL has no equivalent for.
 *
 * Building without it falls back to OpenSSL for TLS as well, which is worth
 * keeping working: it is the only way to compare the two, and the only way to
 * build where the vendored copy will not.
 *
 * What BoringSSL gives up is kernel TLS, which it does not have. Every use of
 * it here is already behind SSL_OP_ENABLE_KTLS, which BoringSSL does not
 * define, so it compiles out on its own -- and kTLS measures as a regression
 * on hardware without NIC offload anyway. */
#ifdef AVIAN_TLS_BORINGSSL
#include "CAvianSSL_ssl.h"
#include "CAvianSSL_err.h"
#include "CAvianSSL_x509v3.h"
#else
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

/* One certificate, with the names it is valid for.
 *
 * The names come out of the certificate rather than from configuration: a
 * certificate already carries the list of hosts it is good for, in its subject
 * alternative names, and asking the operator to repeat it is asking them to
 * get it wrong. */
struct av_tls_host {
    SSL_CTX *ctx;
    char **names;
    int name_count;
};

#define AV_TLS_MAX_HOSTS 64

struct av_tls_ctx {
    struct av_tls_host hosts[AV_TLS_MAX_HOSTS];
    int host_count;
    /* hosts[0]: what a client with no SNI, or an unrecognised one, is served.
     * Answering with the first certificate rather than refusing is what every
     * other server does, and it leaves the decision with the client, which can
     * see the name mismatch and say so in terms its user understands. */
    SSL_CTX *ctx;
    /* ALPN preference list in wire format: length-prefixed, most preferred
     * first. Held here because the callback runs per connection. */
    unsigned char *alpn;
    unsigned int alpn_len;
    /* Where --acme-domain keeps its files, when it is on. A tls-alpn-01
     * challenge certificate for NAME is at <acme_dir>/alpn/NAME.crt. */
    char *acme_dir;
};

#ifdef AVIAN_TLS_BORINGSSL
/* BoringSSL has no read ahead. SSL_CTX_set_read_ahead is one of the calls it
 * keeps for compatibility and does nothing with -- its header says the
 * function "returns one", and that is the whole of it -- so its record layer
 * asks the socket for a record's 5-byte header, learns the body's length from
 * it, and asks again for the body. Two reads where OpenSSL, told to read
 * ahead, costs one: measured at exactly 1.00 against 2.00 reads a request.
 *
 * So the socket BIO is replaced by one that reads greedily. Asked for the
 * header, it reads whatever the socket has into a buffer and answers out of
 * it; the body is then already in hand, and so is any record queued behind
 * it. That is what OpenSSL's read ahead does internally, and BoringSSL has no
 * BIO_f_buffer to compose it from, so it is written out here.
 *
 * The buffer is why av_tls_pending has to count it. Bytes held here are bytes
 * the socket no longer holds, so a poller waiting for the socket to become
 * readable would be waiting for what has already arrived. */
#define AV_GREEDY_BUF 16384

struct av_greedy {
    int fd;
    int start, end;                     /* the filled part of buf */
    unsigned char buf[AV_GREEDY_BUF];
};
#endif

struct av_tls {
    SSL *ssl;
    int wants_write;
    int h2;
    /* The connection negotiated acme-tls/1: a CA validating a challenge, to
     * be closed as soon as the handshake is done. */
    int acme;
#ifdef AVIAN_TLS_BORINGSSL
    /* Owned by the BIO, which frees it; held here only so that
     * av_tls_pending can ask what is still buffered. */
    struct av_greedy *greedy;
#endif
};

/* Binds a TLS object to a socket; defined with the BIO it may install. */
static int tls_attach(struct av_tls *tls, int fd);

/* Marks a connection that is being served a challenge certificate, so that the
 * SNI and ALPN callbacks leave it alone. Allocated once per process. */
static int acme_ex_index = -1;

#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
/* --ktls. Set by the supervisor before any context exists, and inherited by
 * every worker. */
static int g_ktls = 0;
#endif

int av_tls_enable_ktls(int on) {
#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
    g_ktls = on ? 1 : 0;
    return 1;
#else
    (void)on;
    return 0;
#endif
}

int av_tls_kernel_ready(void) {
#if defined(__linux__)
    /* The module creates this when it loads. */
    FILE *f = fopen("/proc/net/tls_stat", "r");
    if (!f) return 0;
    fclose(f);
    return 1;
#else
    return 0;
#endif
}

static void last_error(char *err, size_t err_len, const char *what) {
    if (!err || err_len == 0) return;
    unsigned long code = ERR_get_error();
    if (code == 0) {
        snprintf(err, err_len, "%s", what);
        return;
    }
    char buf[256];
    ERR_error_string_n(code, buf, sizeof buf);
    snprintf(err, err_len, "%s: %s", what, buf);
    /* Drain the rest so a later failure does not report this one. */
    while (ERR_get_error() != 0) { }
}

/* Turns "h2,http/1.1" into the length-prefixed wire form ALPN uses. */
static unsigned char *encode_alpn(const char *list, unsigned int *out_len) {
    size_t n = strlen(list);
    unsigned char *out = malloc(n + 2);
    if (!out) return NULL;
    unsigned int w = 0;
    size_t i = 0;
    while (i <= n) {
        size_t start = i;
        while (i < n && list[i] != ',') i++;
        size_t len = i - start;
        if (len > 0 && len < 256) {
            out[w++] = (unsigned char)len;
            memcpy(out + w, list + start, len);
            w += (unsigned int)len;
        }
        if (i >= n) break;
        i++;
    }
    *out_len = w;
    return out;
}

/* Server preference: walk our list in order and take the first the client
 * offered. OpenSSL's own helper prefers the client's order, which is not what
 * a server that would rather speak HTTP/2 wants. */
static int alpn_select(SSL *ssl, const unsigned char **out, unsigned char *out_len,
                       const unsigned char *in, unsigned int in_len, void *arg) {
    struct av_tls_ctx *ctx = (struct av_tls_ctx *)arg;
    /* A challenge connection speaks acme-tls/1 and nothing else (RFC 8737). */
    if (acme_ex_index >= 0 && SSL_get_ex_data(ssl, acme_ex_index)) {
        for (unsigned int j = 0; j + 1 <= in_len && in[j];) {
            unsigned char have_len = in[j];
            if (have_len == 10 && j + 1u + 10u <= in_len
                && memcmp(in + j + 1, "acme-tls/1", 10) == 0) {
                *out = in + j + 1;
                *out_len = 10;
                return SSL_TLSEXT_ERR_OK;
            }
            j += 1u + have_len;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    for (unsigned int i = 0; i + 1 <= ctx->alpn_len && ctx->alpn[i];) {
        unsigned char want_len = ctx->alpn[i];
        const unsigned char *want = ctx->alpn + i + 1;
        for (unsigned int j = 0; j + 1 <= in_len && in[j];) {
            unsigned char have_len = in[j];
            const unsigned char *have = in + j + 1;
            if (have_len == want_len && memcmp(have, want, have_len) == 0) {
                *out = have;
                *out_len = have_len;
                return SSL_TLSEXT_ERR_OK;
            }
            j += 1u + have_len;
        }
        i += 1u + want_len;
    }
    /* No overlap. Refusing is correct for a client that asked for something
     * specific and got nothing. */
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* Remembers one name a certificate is valid for. Names arrive as ASN.1
 * strings, which are counted rather than terminated and may legally contain an
 * embedded NUL -- a name like that is a forgery attempt, so it is dropped. */
static void add_name(struct av_tls_host *host, const char *name, int len) {
    if (len <= 0 || len > 255) return;
    if (memchr(name, 0, (size_t)len) != NULL) return;
    char **grown = realloc(host->names, (size_t)(host->name_count + 1) * sizeof *grown);
    if (!grown) return;
    host->names = grown;
    char *copy = malloc((size_t)len + 1);
    if (!copy) return;
    memcpy(copy, name, (size_t)len);
    copy[len] = 0;
    host->names[host->name_count++] = copy;
}

/* The DNS names in a certificate: its subject alternative names, or its common
 * name when it has none. CN is deprecated for this and still turns up in
 * certificates people generate by hand for a private service. */
static void collect_names(struct av_tls_host *host) {
    X509 *cert = SSL_CTX_get0_certificate(host->ctx);
    if (!cert) return;

    GENERAL_NAMES *sans = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (sans) {
        int n = sk_GENERAL_NAME_num(sans);
        for (int i = 0; i < n; i++) {
            const GENERAL_NAME *entry = sk_GENERAL_NAME_value(sans, i);
            if (!entry || entry->type != GEN_DNS) continue;
            add_name(host, (const char *)ASN1_STRING_get0_data(entry->d.dNSName),
                     ASN1_STRING_length(entry->d.dNSName));
        }
        GENERAL_NAMES_free(sans);
    }

    if (host->name_count == 0) {
        char common[256];
        int len = X509_NAME_get_text_by_NID(X509_get_subject_name(cert),
                                            NID_commonName, common, sizeof common);
        if (len > 0) add_name(host, common, len);
    }
}

/* Picks the certificate for the name the client asked for. */
static int sni_select(SSL *ssl, int *unused_alert, void *arg) {
    (void)unused_alert;
    struct av_tls_ctx *wrapper = (struct av_tls_ctx *)arg;
    /* Swapping the context would swap out the challenge certificate. */
    if (acme_ex_index >= 0 && SSL_get_ex_data(ssl, acme_ex_index)) return SSL_TLSEXT_ERR_OK;
    const char *asked = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!asked || !*asked) return SSL_TLSEXT_ERR_OK;

    for (int i = 0; i < wrapper->host_count; i++) {
        for (int j = 0; j < wrapper->hosts[i].name_count; j++) {
            if (!av_host_matches(wrapper->hosts[i].names[j], asked)) continue;
            SSL_set_SSL_CTX(ssl, wrapper->hosts[i].ctx);
            return SSL_TLSEXT_ERR_OK;
        }
    }
    /* Unrecognised: the default certificate, and the client decides. */
    return SSL_TLSEXT_ERR_OK;
}

/* Everything that is the same for every certificate. SSL_set_SSL_CTX swaps the
 * certificate but carries almost nothing else over, so each context has to be
 * able to stand on its own. */
static int configure_common(SSL_CTX *ctx, struct av_tls_ctx *wrapper,
                            const char *ciphers, char *err, size_t err_len) {
    /* TLS 1.2 is the floor; everything below it is broken in public. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION
                             | SSL_OP_CIPHER_SERVER_PREFERENCE
                             | SSL_OP_NO_RENEGOTIATION);
#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
    /* --ktls: kernel TLS, wherever the kernel and the negotiated cipher allow
     * it. OpenSSL falls back to encrypting in-process on its own when not. */
    if (g_ktls) SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
    if (!g_ktls) SSL_CTX_set_read_ahead(ctx, 1);
#else
    SSL_CTX_set_read_ahead(ctx, 1);
#endif
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE
                          | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
                          | SSL_MODE_RELEASE_BUFFERS);
    if (ciphers && *ciphers) {
        if (SSL_CTX_set_cipher_list(ctx, ciphers) != 1) {
            last_error(err, err_len, "no usable ciphers in the list given");
            return 0;
        }
    }
    if (wrapper->alpn) SSL_CTX_set_alpn_select_cb(ctx, alpn_select, wrapper);
    return 1;
}

/* Loads a certificate and key into a fresh context and records its names. */
static int add_host(struct av_tls_ctx *wrapper, const char *cert_path,
                    const char *key_path, const char *ciphers,
                    char *err, size_t err_len) {
    if (wrapper->host_count >= AV_TLS_MAX_HOSTS) {
        if (err && err_len) snprintf(err, err_len, "too many certificates");
        return 0;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        last_error(err, err_len, "cannot create a TLS context");
        return 0;
    }
    if (!configure_common(ctx, wrapper, ciphers, err, err_len)) {
        SSL_CTX_free(ctx);
        return 0;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        last_error(err, err_len, "cannot load the certificate");
        SSL_CTX_free(ctx);
        return 0;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        last_error(err, err_len, "cannot load the private key");
        SSL_CTX_free(ctx);
        return 0;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        last_error(err, err_len, "the private key does not match the certificate");
        SSL_CTX_free(ctx);
        return 0;
    }

    struct av_tls_host *host = &wrapper->hosts[wrapper->host_count++];
    host->ctx = ctx;
    host->names = NULL;
    host->name_count = 0;
    collect_names(host);
    return 1;
}

/* --- tls-alpn-01 (RFC 8737) ------------------------------------------------
 *
 * A CA validating a challenge opens a TLS connection offering exactly one
 * protocol, acme-tls/1, and expects a self-signed certificate carrying the
 * digest of the key authorization. The ACME helper process writes that
 * certificate into the cache directory; any worker the connection lands on
 * finds it there by the name in the SNI.
 *
 * It has to be decided in the ClientHello callback. The SNI callback runs
 * before ALPN is known, and the ALPN callback runs after the certificate has
 * been chosen, so neither alone can serve a certificate that depends on both. */

static pthread_once_t acme_index_once = PTHREAD_ONCE_INIT;

static void make_acme_index(void) {
    acme_ex_index = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

static int offers_acme(const unsigned char *ext, size_t len) {
    if (len < 2) return 0;
    size_t list = (size_t)ext[0] << 8 | ext[1];
    if (list + 2 > len) return 0;
    size_t i = 2;
    while (i < 2 + list) {
        size_t n = ext[i];
        if (i + 1 + n > 2 + list) return 0;
        if (n == 10 && memcmp(ext + i + 1, "acme-tls/1", 10) == 0) return 1;
        i += 1 + n;
    }
    return 0;
}

/* The host name from a raw server_name extension, lower-cased, restricted to
 * the characters a DNS name has -- it becomes part of a file path. */
static int sni_host(const unsigned char *ext, size_t len, char *out, size_t cap) {
    if (len < 5) return 0;
    size_t list = (size_t)ext[0] << 8 | ext[1];
    if (list + 2 > len || list < 3 || ext[2] != 0) return 0;
    size_t n = (size_t)ext[3] << 8 | ext[4];
    if (n == 0 || n + 5 > len || n >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = ext[5 + i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        int allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!allowed) return 0;
        out[i] = (char)c;
    }
    out[n] = 0;
    return out[0] != '.';
}

/* The tls-alpn-01 decision itself. The two libraries hand a ClientHello
 * callback different things -- OpenSSL an SSL and an argument of the caller's
 * choosing, BoringSSL an SSL_CLIENT_HELLO and no argument at all -- but what
 * is decided is the same, so it is written once here and each callback below
 * only adapts its own shape to it.
 *
 * `alpn` and `sni` are the extension bodies, or NULL where the ClientHello
 * did not carry them. */
static void acme_pick(SSL *ssl, struct av_tls_ctx *wrapper,
                      const unsigned char *alpn, size_t alpn_len,
                      const unsigned char *sni, size_t sni_len) {
    if (!wrapper || !wrapper->acme_dir) return;
    if (!alpn || !offers_acme(alpn, alpn_len)) return;
    char host[256];
    if (!sni || !sni_host(sni, sni_len, host, sizeof host)) return;

    char cert[4200], key[4200];
    if (snprintf(cert, sizeof cert, "%s/alpn/%s.crt", wrapper->acme_dir, host) >= (int)sizeof cert
        || snprintf(key, sizeof key, "%s/alpn/%s.key", wrapper->acme_dir, host) >= (int)sizeof key) {
        return;
    }
    /* No challenge pending for that name is not an error here: the handshake
     * carries on as usual, and a client that offered nothing but acme-tls/1
     * is refused by the ALPN callback for want of a protocol in common. */
    if (SSL_use_certificate_file(ssl, cert, SSL_FILETYPE_PEM) != 1
        || SSL_use_PrivateKey_file(ssl, key, SSL_FILETYPE_PEM) != 1) {
        ERR_clear_error();
        return;
    }
    SSL_set_ex_data(ssl, acme_ex_index, (void *)1);
}

#ifdef AVIAN_TLS_BORINGSSL

/* BoringSSL's callback carries no argument of its own, so the wrapper rides
 * on the context and is fetched back out here. */
static int acme_ctx_index = -1;
static pthread_once_t acme_ctx_index_once = PTHREAD_ONCE_INIT;

static void make_acme_ctx_index(void) {
    acme_ctx_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

static enum ssl_select_cert_result_t acme_client_hello(const SSL_CLIENT_HELLO *hello) {
    const unsigned char *alpn = NULL, *sni = NULL;
    size_t alpn_len = 0, sni_len = 0;
    if (!SSL_early_callback_ctx_extension_get(
            hello, TLSEXT_TYPE_application_layer_protocol_negotiation, &alpn, &alpn_len)) {
        alpn = NULL;
    }
    if (!SSL_early_callback_ctx_extension_get(hello, TLSEXT_TYPE_server_name, &sni, &sni_len)) {
        sni = NULL;
    }
    struct av_tls_ctx *wrapper =
        acme_ctx_index < 0 ? NULL
                           : (struct av_tls_ctx *)SSL_CTX_get_ex_data(
                                 SSL_get_SSL_CTX(hello->ssl), acme_ctx_index);
    acme_pick(hello->ssl, wrapper, alpn, alpn_len, sni, sni_len);
    return ssl_select_cert_success;
}

#else

static int acme_client_hello(SSL *ssl, int *alert, void *arg) {
    (void)alert;
    const unsigned char *alpn = NULL, *sni = NULL;
    size_t alpn_len = 0, sni_len = 0;
    if (!SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation,
                                   &alpn, &alpn_len)) {
        alpn = NULL;
    }
    if (!SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &sni, &sni_len)) {
        sni = NULL;
    }
    acme_pick(ssl, (struct av_tls_ctx *)arg, alpn, alpn_len, sni, sni_len);
    return SSL_CLIENT_HELLO_SUCCESS;
}

#endif

int av_tls_ctx_set_acme_dir(av_tls_ctx *wrapper, const char *dir) {
    if (!wrapper || !dir) return -1;
    pthread_once(&acme_index_once, make_acme_index);
    if (acme_ex_index < 0) return -1;
#ifdef AVIAN_TLS_BORINGSSL
    pthread_once(&acme_ctx_index_once, make_acme_ctx_index);
    if (acme_ctx_index < 0) return -1;
#endif
    char *copy = strdup(dir);
    if (!copy) return -1;
    free(wrapper->acme_dir);
    wrapper->acme_dir = copy;
    /* On the context every connection starts on: the ClientHello callback
     * runs before SNI could move a connection to another one. */
#ifdef AVIAN_TLS_BORINGSSL
    SSL_CTX_set_ex_data(wrapper->ctx, acme_ctx_index, wrapper);
    SSL_CTX_set_select_certificate_cb(wrapper->ctx, acme_client_hello);
#else
    SSL_CTX_set_client_hello_cb(wrapper->ctx, acme_client_hello, wrapper);
#endif
    return 0;
}

int av_tls_available(void) { return 1; }

int av_tls_ctx_add(av_tls_ctx *wrapper, const char *cert_path, const char *key_path,
                   const char *ciphers, char *err, size_t err_len) {
    if (!wrapper) return 0;
    return add_host(wrapper, cert_path, key_path, ciphers, err, err_len);
}

int av_tls_ctx_names(av_tls_ctx *wrapper, int host_index, int name_index,
                     char *out, size_t out_len) {
    if (!wrapper || host_index < 0 || host_index >= wrapper->host_count) return 0;
    struct av_tls_host *host = &wrapper->hosts[host_index];
    if (name_index < 0 || name_index >= host->name_count) return 0;
    if (out && out_len) snprintf(out, out_len, "%s", host->names[name_index]);
    return 1;
}

int av_tls_ctx_host_count(av_tls_ctx *wrapper) {
    return wrapper ? wrapper->host_count : 0;
}

av_tls_ctx *av_tls_ctx_new(const char *cert_path, const char *key_path,
                           const char *alpn, const char *ciphers,
                           char *err, size_t err_len) {
    struct av_tls_ctx *wrapper = calloc(1, sizeof *wrapper);
    if (!wrapper) {
        if (err && err_len) snprintf(err, err_len, "out of memory");
        return NULL;
    }

    /* Before the first context, so that `configure_common` can install the
     * callback on every one of them. */
    if (alpn && *alpn) {
        wrapper->alpn = encode_alpn(alpn, &wrapper->alpn_len);
        if (!wrapper->alpn) {
            if (err && err_len) snprintf(err, err_len, "out of memory");
            free(wrapper);
            return NULL;
        }
    }

    if (!add_host(wrapper, cert_path, key_path, ciphers, err, err_len)) {
        free(wrapper->alpn);
        free(wrapper);
        return NULL;
    }

    /* The first certificate is the default, and the one the SNI callback hangs
     * off: the callback runs before the context is swapped, so it has to be
     * installed on whichever context the connection starts on. */
    wrapper->ctx = wrapper->hosts[0].ctx;
    SSL_CTX_set_tlsext_servername_callback(wrapper->ctx, sni_select);
    SSL_CTX_set_tlsext_servername_arg(wrapper->ctx, wrapper);
    return wrapper;
}

void av_tls_ctx_free(av_tls_ctx *wrapper) {
    if (!wrapper) return;
    for (int i = 0; i < wrapper->host_count; i++) {
        for (int j = 0; j < wrapper->hosts[i].name_count; j++) {
            free(wrapper->hosts[i].names[j]);
        }
        free(wrapper->hosts[i].names);
        if (wrapper->hosts[i].ctx) SSL_CTX_free(wrapper->hosts[i].ctx);
    }
    free(wrapper->alpn);
    free(wrapper->acme_dir);
    free(wrapper);
}

av_tls_ctx *av_tls_client_ctx_new(const char *ca_file, const char *alpn,
                                  char *err, size_t err_len) {
    struct av_tls_ctx *wrapper = calloc(1, sizeof *wrapper);
    if (!wrapper) {
        if (err && err_len) snprintf(err, err_len, "out of memory");
        return NULL;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        last_error(err, err_len, "cannot create a TLS client context");
        free(wrapper);
        return NULL;
    }
    /* The same floor and the same modes as a served connection. Not
     * SSL_OP_CIPHER_SERVER_PREFERENCE, which means nothing to a client. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_read_ahead(ctx, 1);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE
                          | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
                          | SSL_MODE_RELEASE_BUFFERS);

    /* Refuse a chain that does not check out, rather than reporting it and
     * carrying on: a client that continues past a verification failure is
     * not doing TLS, it is doing encryption against nobody in particular. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (ca_file && *ca_file) {
        if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
            last_error(err, err_len, "cannot load the CA file");
            SSL_CTX_free(ctx);
            free(wrapper);
            return NULL;
        }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        last_error(err, err_len, "cannot load the system trust store");
        SSL_CTX_free(ctx);
        free(wrapper);
        return NULL;
    }

    if (alpn && *alpn) {
        unsigned int len = 0;
        unsigned char *wire = encode_alpn(alpn, &len);
        if (!wire) {
            if (err && err_len) snprintf(err, err_len, "out of memory");
            SSL_CTX_free(ctx);
            free(wrapper);
            return NULL;
        }
        /* Inverted, unlike almost everything else here: 0 is success. */
        if (SSL_CTX_set_alpn_protos(ctx, wire, len) != 0) {
            last_error(err, err_len, "cannot set the ALPN list");
            free(wire);
            SSL_CTX_free(ctx);
            free(wrapper);
            return NULL;
        }
        wrapper->alpn = wire;
        wrapper->alpn_len = len;
    }

    /* Kept in hosts[0] rather than only in `ctx`: av_tls_ctx_free releases
     * contexts through the hosts array, so a context parked anywhere else
     * would leak. There is no certificate and no name list -- a client sends
     * neither -- so host_count is 1 with names NULL. */
    wrapper->hosts[0].ctx = ctx;
    wrapper->hosts[0].names = NULL;
    wrapper->hosts[0].name_count = 0;
    wrapper->host_count = 1;
    wrapper->ctx = ctx;
    return wrapper;
}

av_tls *av_tls_client_new(av_tls_ctx *ctx, int fd, const char *hostname) {
    if (!ctx) return NULL;
    struct av_tls *tls = calloc(1, sizeof *tls);
    if (!tls) return NULL;
    tls->ssl = SSL_new(ctx->ctx);
    if (!tls->ssl) { free(tls); return NULL; }
    if (!tls_attach(tls, fd)) {
        SSL_free(tls->ssl);
        free(tls);
        return NULL;
    }
    if (hostname && *hostname) {
        unsigned char literal[16];
        int is_address = inet_pton(AF_INET, hostname, literal) == 1
            || inet_pton(AF_INET6, hostname, literal) == 1;
        if (is_address) {
            /* An address is checked against the certificate's IP addresses,
             * which is not what SSL_set1_host does in every OpenSSL this may
             * link: that one matches DNS names, and a literal matched as a
             * DNS name matches nothing, so https://127.0.0.1/ would be refused
             * however correct its certificate. Said explicitly rather than
             * left to whichever version is installed.
             *
             * And no SNI: RFC 6066 section 3 allows only a DNS name there,
             * never a literal address. */
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(tls->ssl), hostname) != 1) {
                SSL_free(tls->ssl);
                free(tls);
                return NULL;
            }
        } else {
            /* Which certificate to send... */
            SSL_set_tlsext_host_name(tls->ssl, hostname);
            /* ...and the name that certificate has to be for. Verification
             * without this checks that the chain is trusted, not that it
             * belongs to whoever we meant to talk to. */
            if (SSL_set1_host(tls->ssl, hostname) != 1) {
                SSL_free(tls->ssl);
                free(tls);
                return NULL;
            }
        }
    }
    SSL_set_connect_state(tls->ssl);
    return tls;
}

#ifdef AVIAN_TLS_BORINGSSL

static int greedy_read(BIO *bio, char *out, int len) {
    struct av_greedy *g = (struct av_greedy *)BIO_get_data(bio);
    if (!g || !out || len <= 0) return 0;
    BIO_clear_retry_flags(bio);
    if (g->start == g->end) {
        ssize_t n;
        do { n = read(g->fd, g->buf, sizeof g->buf); } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            /* A short read is the socket being empty, not the end of it. Say
             * so, or the record layer reports a broken connection. */
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) BIO_set_retry_read(bio);
            return (int)n;
        }
        g->start = 0;
        g->end = (int)n;
    }
    int have = g->end - g->start;
    if (have > len) have = len;
    memcpy(out, g->buf + g->start, (size_t)have);
    g->start += have;
    return have;
}

static int greedy_write(BIO *bio, const char *in, int len) {
    struct av_greedy *g = (struct av_greedy *)BIO_get_data(bio);
    if (!g || !in || len <= 0) return 0;
    BIO_clear_retry_flags(bio);
    ssize_t n;
    do { n = write(g->fd, in, (size_t)len); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) BIO_set_retry_write(bio);
    return (int)n;
}

static long greedy_ctrl(BIO *bio, int cmd, long larg, void *parg) {
    struct av_greedy *g = (struct av_greedy *)BIO_get_data(bio);
    (void)larg; (void)parg;
    switch (cmd) {
    /* Nothing is held on the way out: writes go straight to the socket. */
    case BIO_CTRL_FLUSH:   return 1;
    case BIO_CTRL_PENDING: return g ? g->end - g->start : 0;
    case BIO_CTRL_EOF:     return 0;
    default:               return 0;
    }
}

static int greedy_destroy(BIO *bio) {
    if (!bio) return 0;
    free(BIO_get_data(bio));
    BIO_set_data(bio, NULL);
    return 1;
}

/* One method for the process, made once. The socket is not closed here: the
 * worker owns the descriptor and closes it itself. */
static BIO_METHOD *greedy_meth = NULL;
static pthread_once_t greedy_once = PTHREAD_ONCE_INIT;

static void make_greedy_meth(void) {
    BIO_METHOD *m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "avian greedy");
    if (!m) return;
    if (!BIO_meth_set_read(m, greedy_read) || !BIO_meth_set_write(m, greedy_write)
        || !BIO_meth_set_ctrl(m, greedy_ctrl) || !BIO_meth_set_destroy(m, greedy_destroy)) {
        BIO_meth_free(m);
        return;
    }
    greedy_meth = m;
}

/* A BIO over `fd` that reads ahead. NULL if it cannot be made, and the caller
 * falls back to the plain socket BIO: an extra read a request is worth far
 * more than a connection refused. */
static BIO *greedy_bio(int fd, struct av_greedy **out) {
    pthread_once(&greedy_once, make_greedy_meth);
    if (!greedy_meth) return NULL;
    struct av_greedy *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->fd = fd;
    BIO *bio = BIO_new(greedy_meth);
    if (!bio) { free(g); return NULL; }
    BIO_set_data(bio, g);
    BIO_set_init(bio, 1);
    *out = g;
    return bio;
}

#endif /* AVIAN_TLS_BORINGSSL */

/* Binds the TLS object to a socket. Under BoringSSL that is a BIO of our own
 * that reads ahead, since BoringSSL will not; under OpenSSL, told to read
 * ahead in configure_common, the library's own socket BIO already does. */
/* The greedy BIO can be switched off with AVIAN_NO_GREEDY=1, which puts the
 * record layer back on a plain socket BIO and so back to two reads a record.
 * It exists to measure the BIO against itself in one binary, where two builds
 * would differ in more than the one branch. */
static int greedy_disabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("AVIAN_NO_GREEDY");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

static int tls_attach(struct av_tls *tls, int fd) {
#ifdef AVIAN_TLS_BORINGSSL
    BIO *bio = greedy_disabled() ? NULL : greedy_bio(fd, &tls->greedy);
    if (bio) {
        /* One BIO for both directions takes one reference, and SSL_free
         * releases it. */
        SSL_set_bio(tls->ssl, bio, bio);
        return 1;
    }
    tls->greedy = NULL;
#endif
    return SSL_set_fd(tls->ssl, fd) == 1;
}

av_tls *av_tls_new(av_tls_ctx *ctx, int fd) {
    if (!ctx) return NULL;
    struct av_tls *tls = calloc(1, sizeof *tls);
    if (!tls) return NULL;
    tls->ssl = SSL_new(ctx->ctx);
    if (!tls->ssl) { free(tls); return NULL; }
    if (!tls_attach(tls, fd)) {
        SSL_free(tls->ssl);
        free(tls);
        return NULL;
    }
    SSL_set_accept_state(tls->ssl);
    return tls;
}

void av_tls_free(av_tls *tls) {
    if (!tls) return;
    if (tls->ssl) SSL_free(tls->ssl);
    free(tls);
}

int av_tls_handshake(av_tls *tls, char *err, size_t err_len) {
    if (!tls || !tls->ssl) return -2;
    int rc = SSL_do_handshake(tls->ssl);
    if (rc == 1) {
        const unsigned char *proto = NULL;
        unsigned int len = 0;
        SSL_get0_alpn_selected(tls->ssl, &proto, &len);
        tls->h2 = (len == 2 && proto && proto[0] == 'h' && proto[1] == '2');
        tls->acme = (len == 10 && proto && memcmp(proto, "acme-tls/1", 10) == 0);
        tls->wants_write = 0;
        return 1;
    }
    int reason = SSL_get_error(tls->ssl, rc);
    switch (reason) {
    case SSL_ERROR_WANT_READ:
        tls->wants_write = 0;
        ERR_clear_error();
        return 0;
    case SSL_ERROR_WANT_WRITE:
        tls->wants_write = 1;
        ERR_clear_error();
        return -1;
    default:
        /* last_error drains the queue itself as it reads the message out. */
        last_error(err, err_len, "handshake failed");
        return -2;
    }
}

long av_tls_read(av_tls *tls, void *buf, long n) {
    if (!tls || !tls->ssl) { errno = EPIPE; return -1; }
    if (n <= 0) return 0;
    int rc = SSL_read(tls->ssl, buf, (int)(n > 0x7fffffff ? 0x7fffffff : n));
    if (rc > 0) {
        tls->wants_write = 0;
        return rc;
    }
    /* Read first, while the queue still says why, then empty it for whoever
     * calls next. */
    int reason = SSL_get_error(tls->ssl, rc);
    ERR_clear_error();
    switch (reason) {
    case SSL_ERROR_ZERO_RETURN:
        return 0;                      /* close_notify: a clean end of stream */
    case SSL_ERROR_WANT_READ:
        tls->wants_write = 0;
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_WANT_WRITE:
        /* A key update or renegotiation needs the socket writable before this
         * read can finish. */
        tls->wants_write = 1;
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_SYSCALL:
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
        errno = EPIPE;
        return -1;
    default:
        errno = EPIPE;
        return -1;
    }
}

long av_tls_write(av_tls *tls, const void *buf, long n) {
    if (!tls || !tls->ssl) { errno = EPIPE; return -1; }
    if (n <= 0) return 0;
    int rc = SSL_write(tls->ssl, buf, (int)(n > 0x7fffffff ? 0x7fffffff : n));
    if (rc > 0) {
        tls->wants_write = 0;
        return rc;
    }
    int reason = SSL_get_error(tls->ssl, rc);
    ERR_clear_error();
    switch (reason) {
    case SSL_ERROR_WANT_READ:
        /* Rare, but a write can need input first. The caller polls for both. */
        tls->wants_write = 0;
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_WANT_WRITE:
        tls->wants_write = 1;
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_SYSCALL:
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
        errno = EPIPE;
        return -1;
    default:
        errno = EPIPE;
        return -1;
    }
}

int av_tls_ktls_send(av_tls *tls) {
#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
    if (!tls || !tls->ssl) return 0;
    BIO *wbio = SSL_get_wbio(tls->ssl);
    return wbio && BIO_get_ktls_send(wbio) ? 1 : 0;
#else
    (void)tls;
    return 0;
#endif
}

int av_tls_ktls_recv(av_tls *tls) {
#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
    if (!tls || !tls->ssl) return 0;
    BIO *rbio = SSL_get_rbio(tls->ssl);
    return rbio && BIO_get_ktls_recv(rbio) ? 1 : 0;
#else
    (void)tls;
    return 0;
#endif
}

int av_tls_release_to_kernel(av_tls *tls) {
#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
    if (!tls || !tls->ssl || tls->h2 || tls->acme || tls->wants_write) return 0;
    if (!av_tls_ktls_send(tls) || !av_tls_ktls_recv(tls)) return 0;
    /* Anything OpenSSL has read and not handed over, or has half-sent, would
     * be lost with it. */
    if (SSL_has_pending(tls->ssl) || SSL_pending(tls->ssl) > 0) return 0;
    if (SSL_get_shutdown(tls->ssl) != 0) return 0;
    if (!SSL_is_init_finished(tls->ssl)) return 0;
    /* SSL_free sends nothing, and the socket BIO does not own the descriptor,
     * so the connection goes on exactly as the kernel has it. */
    SSL_free(tls->ssl);
    tls->ssl = NULL;
    free(tls);
    return 1;
#else
    (void)tls;
    return 0;
#endif
}

long av_tls_sendfile(av_tls *tls, int fd, long offset, long n) {
#if defined(SSL_OP_ENABLE_KTLS) && !defined(OPENSSL_NO_KTLS)
    if (!tls || !tls->ssl) { errno = EPIPE; return -1; }
    if (n <= 0) return 0;
    ossl_ssize_t rc = SSL_sendfile(tls->ssl, fd, (off_t)offset, (size_t)n, 0);
    if (rc > 0) {
        tls->wants_write = 0;
        return (long)rc;
    }
    if (rc == 0) {
        /* Nothing left at that offset: the file shrank after its length was
         * promised. */
        errno = EPIPE;
        return -1;
    }
    int reason = SSL_get_error(tls->ssl, (int)rc);
    ERR_clear_error();
    switch (reason) {
    case SSL_ERROR_WANT_WRITE:
        tls->wants_write = 1;
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_WANT_READ:
        tls->wants_write = 0;
        errno = EAGAIN;
        return -1;
    case SSL_ERROR_SYSCALL:
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
        errno = EPIPE;
        return -1;
    default:
        errno = EPIPE;
        return -1;
    }
#else
    (void)tls; (void)fd; (void)offset; (void)n;
    errno = EPIPE;
    return -1;
#endif
}

int av_tls_flush_control(av_tls *tls) {
    if (!tls || !tls->ssl) { errno = EPIPE; return -1; }
#ifdef AVIAN_TLS_BORINGSSL
    /* A write of length zero is BoringSSL's documented way to push the control
     * messages it is holding -- a session ticket, a key update -- without
     * sending any application data with them. */
    int rc = SSL_write(tls->ssl, "", 0);
    if (rc >= 0) return 0;
    int reason = SSL_get_error(tls->ssl, rc);
    ERR_clear_error();
    if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE) {
        if (reason == SSL_ERROR_WANT_WRITE) tls->wants_write = 1;
        errno = EAGAIN;
        return -1;
    }
    errno = EPIPE;
    return -1;
#else
    /* OpenSSL sent the ticket as the handshake finished. */
    return 0;
#endif
}

int av_tls_pending(av_tls *tls) {
    if (!tls || !tls->ssl) return 0;
    int decrypted = SSL_pending(tls->ssl);
    if (decrypted > 0) return decrypted;
#ifdef AVIAN_TLS_BORINGSSL
    /* Bytes the greedy BIO read ahead and the record layer has not taken yet.
     * SSL_has_pending does not know about them -- they are behind the BIO,
     * not inside the SSL -- and they are no longer in the socket either, so
     * without this a poller would wait for what has already arrived. */
    if (tls->greedy && tls->greedy->end > tls->greedy->start) return 1;
#endif
    /* Read ahead of the record just returned, and not decrypted yet. */
    return SSL_has_pending(tls->ssl) ? 1 : 0;
}

int av_tls_idle_ok(av_tls *tls) {
    if (!tls || !tls->ssl) return 0;
    unsigned char byte;
    /* SSL_read is what drives post-handshake messages; OpenSSL offers no way
     * to process them without offering to read. Application data arriving on
     * a connection nobody is using means the peer spoke out of turn, and the
     * byte consumed here does not matter: that connection is being discarded
     * either way. */
    int rc = SSL_read(tls->ssl, &byte, 1);
    if (rc > 0) return 0;
    int reason = SSL_get_error(tls->ssl, rc);
    ERR_clear_error();
    switch (reason) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        /* Nothing to report: whatever arrived was bookkeeping, and OpenSSL
         * has dealt with it. */
        return 1;
    case SSL_ERROR_SYSCALL:
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 1 : 0;
    default:
        /* close_notify included: a clean end is still an end. */
        return 0;
    }
}

int av_tls_wants_write(av_tls *tls) { return tls ? tls->wants_write : 0; }

int av_tls_is_h2(av_tls *tls) { return tls ? tls->h2 : 0; }

const char *av_tls_server_name(av_tls *tls) {
    if (!tls || !tls->ssl) return NULL;
    return SSL_get_servername(tls->ssl, TLSEXT_NAMETYPE_host_name);
}

int av_tls_is_acme(av_tls *tls) { return tls ? tls->acme : 0; }

void av_tls_shutdown(av_tls *tls) {
    if (!tls || !tls->ssl) return;
    /* One try. If the socket will not take the close_notify we are closing
     * anyway, and blocking here would hold up the whole loop. */
    SSL_shutdown(tls->ssl);
    ERR_clear_error();
}

#endif /* AV_NO_OPENSSL */

/* ------------------------------------------------------------------------ */
/* A connection whose TLS is the kernel's alone                             */
/* ------------------------------------------------------------------------ */

#if defined(__linux__)
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#ifndef SOL_TLS
#define SOL_TLS 282
#endif
#ifndef TLS_SET_RECORD_TYPE
#define TLS_SET_RECORD_TYPE 1
#endif

int av_ktls_close_notify(int fd) {
    /* An alert record: warning (1), close_notify (0). The kernel frames and
     * encrypts it like any other record once told its type. */
    unsigned char alert[2] = { 1, 0 };
    struct iovec iov = { alert, sizeof alert };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(unsigned char))];
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof control.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_TLS;
    c->cmsg_type = TLS_SET_RECORD_TYPE;
    c->cmsg_len = CMSG_LEN(sizeof(unsigned char));
    *CMSG_DATA(c) = 21; /* alert */
    return sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof alert ? 0 : -1;
}
#else
int av_ktls_close_notify(int fd) { (void)fd; errno = ENOTSUP; return -1; }
#endif
