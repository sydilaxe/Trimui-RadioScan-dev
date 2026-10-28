#include "net.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define USER_AGENT "RadioScan/1.0 (TrimUI Brick)"

/* ------------------------------------------------------------------ TLS */

struct TlsSession {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
};

static mbedtls_x509_crt g_ca;
static int g_ca_loaded;
static pthread_once_t g_psa_once = PTHREAD_ONCE_INIT;

static void psa_init_once(void) { psa_crypto_init(); }

int net_init(const char *ca_bundle_path)
{
    pthread_once(&g_psa_once, psa_init_once);
    mbedtls_x509_crt_init(&g_ca);
    int rc = mbedtls_x509_crt_parse_file(&g_ca, ca_bundle_path);
    g_ca_loaded = rc >= 0 && g_ca.version != 0;
    if (!g_ca_loaded) fprintf(stderr, "net: could not load CA bundle %s (HTTPS will be unverified)\n", ca_bundle_path);
    return g_ca_loaded ? 0 : -1;
}

/* Handhelds often boot with a wrong clock, so ignore certificate validity dates. */
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx; (void)crt; (void)depth;
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

static int raw_recv(HttpConn *c, unsigned char *dst, size_t n);

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    HttpConn *c = ctx;
    ssize_t r = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (r < 0) return (errno == EINTR || errno == EAGAIN) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int r = raw_recv(ctx, buf, len);
    if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    return r; /* 0 = EOF */
}

static void tls_free(HttpConn *c)
{
    if (!c->tls) return;
    mbedtls_ssl_free(&c->tls->ssl);
    mbedtls_ssl_config_free(&c->tls->conf);
    mbedtls_ctr_drbg_free(&c->tls->drbg);
    mbedtls_entropy_free(&c->tls->entropy);
    free(c->tls);
    c->tls = NULL;
}

static int tls_start(HttpConn *c, const char *host, char *err, size_t errlen)
{
    pthread_once(&g_psa_once, psa_init_once);
    struct TlsSession *t = calloc(1, sizeof *t);
    if (!t) return -1;
    c->tls = t;
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ssl_init(&t->ssl);
    int rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, (const unsigned char *)"radioscan", 9);
    if (!rc) rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) { snprintf(err, errlen, "TLS init failed (-0x%04x)", -rc); return -1; }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (g_ca_loaded) {
        mbedtls_ssl_conf_ca_chain(&t->conf, &g_ca, NULL);
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_verify(&t->conf, verify_cb, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    rc = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (!rc) rc = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (rc) { snprintf(err, errlen, "TLS setup failed (-0x%04x)", -rc); return -1; }
    mbedtls_ssl_set_bio(&t->ssl, c, bio_send, bio_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) snprintf(err, errlen, "TLS: certificate not trusted");
        else snprintf(err, errlen, "TLS handshake failed (-0x%04x)", -rc);
        return -1;
    }
    return 0;
}

static int conn_send(HttpConn *c, const char *buf, int len)
{
    if (!c->tls) return (int)send(c->fd, buf, len, MSG_NOSIGNAL);
    int off = 0;
    while (off < len) {
        int r = mbedtls_ssl_write(&c->tls->ssl, (const unsigned char *)buf + off, len - off);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r < 0) return -1;
        off += r;
    }
    return len;
}

/* ------------------------------------------------------------------ HTTP */

static int parse_url(const char *url, char *host, size_t hl, int *port, char *path, size_t pl, int *tls)
{
    const char *p = url;
    *port = 80;
    *tls = 0;
    if (!strncasecmp(p, "http://", 7)) p += 7;
    else if (!strncasecmp(p, "https://", 8)) { p += 8; *port = 443; *tls = 1; }
    else if (strstr(p, "://")) return -1;
    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostlen == 0 || hostlen >= hl) return -1;
    memcpy(host, p, hostlen);
    host[hostlen] = 0;
    char *at = strrchr(host, '@'); /* strip user:pass@ */
    if (at) memmove(host, at + 1, strlen(at));
    char *colon = strrchr(host, ':');
    if (colon && !strchr(host, ']')) { *port = atoi(colon + 1); *colon = 0; }
    snprintf(path, pl, "%s", slash ? slash : "/");
    return 0;
}

static int tcp_connect(const char *host, int port, char *err, size_t errlen)
{
    struct addrinfo hints = {0}, *res = NULL, *ai;
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, ports, &hints, &res);
    if (rc) { snprintf(err, errlen, "DNS failed: %s", host); return -1; }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) snprintf(err, errlen, "Connect failed: %s:%d", host, port);
    return fd;
}

/* socket read with abort polling and a 15 s silence timeout */
static int raw_recv(HttpConn *c, unsigned char *dst, size_t n)
{
    int waited = 0;
    for (;;) {
        if (c->abort_flag && *c->abort_flag != c->abort_value) return -1;
        struct pollfd pfd = {c->fd, POLLIN, 0};
        int r = poll(&pfd, 1, 200);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) {
            waited += 200;
            if (waited >= 15000) return -1;
            continue;
        }
        int got = (int)recv(c->fd, dst, n, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return got;
    }
}

static int raw_read(HttpConn *c, void *dst, int n)
{
    if (!c->tls) return raw_recv(c, dst, (size_t)n);
    for (;;) {
        int r = mbedtls_ssl_read(&c->tls->ssl, dst, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r < 0 ? -1 : r;
    }
}

static int buf_getc(HttpConn *c)
{
    if (c->buf_pos >= c->buf_len) {
        int r = raw_read(c, c->buf, sizeof c->buf);
        if (r <= 0) return -1;
        c->buf_len = r;
        c->buf_pos = 0;
    }
    return (unsigned char)c->buf[c->buf_pos++];
}

static int read_line(HttpConn *c, char *line, int n)
{
    int len = 0, ch;
    while ((ch = buf_getc(c)) >= 0) {
        if (ch == '\n') break;
        if (ch != '\r' && len < n - 1) line[len++] = (char)ch;
    }
    line[len] = 0;
    return ch < 0 && len == 0 ? -1 : len;
}

static int unbuffered_read(HttpConn *c, void *dst, int n)
{
    if (c->buf_pos < c->buf_len) {
        int k = c->buf_len - c->buf_pos;
        if (k > n) k = n;
        memcpy(dst, c->buf + c->buf_pos, k);
        c->buf_pos += k;
        return k;
    }
    return raw_read(c, dst, n);
}

int http_read(HttpConn *c, void *dst, int n)
{
    if (c->eof) return 0;
    if (!c->chunked) {
        int r = unbuffered_read(c, dst, n);
        if (r == 0) c->eof = 1;
        return r;
    }
    if (c->chunk_left <= 0) {
        char line[64];
        if (c->chunk_left == 0 && read_line(c, line, sizeof line) < 0) return -1; /* CRLF after chunk */
        if (read_line(c, line, sizeof line) < 0) return -1;
        c->chunk_left = strtol(line, NULL, 16);
        if (c->chunk_left <= 0) { c->eof = 1; return 0; }
    }
    if (n > c->chunk_left) n = (int)c->chunk_left;
    int r = unbuffered_read(c, dst, n);
    if (r > 0) c->chunk_left -= r;
    return r;
}

static int open_once(HttpConn *c, const char *url, int want_icy, char *err, size_t errlen)
{
    char host[256], path[4096];
    int port, tls;
    c->content_length = -1;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path, &tls)) {
        snprintf(err, errlen, "Bad URL");
        return -1;
    }
    c->fd = tcp_connect(host, port, err, errlen);
    if (c->fd < 0) return -1;
    if (tls && tls_start(c, host, err, errlen)) return -1;

    char req[4600];
    int len = snprintf(req, sizeof req,
                       "GET %s HTTP/1.1\r\nHost: %s%s%.0d\r\nUser-Agent: " USER_AGENT "\r\n"
                       "Accept: */*\r\n%sConnection: close\r\n\r\n",
                       path, host, port != (tls ? 443 : 80) ? ":" : "", port != (tls ? 443 : 80) ? port : 0,
                       want_icy ? "Icy-MetaData: 1\r\n" : "");
    if (conn_send(c, req, len) != len) {
        snprintf(err, errlen, "Send failed");
        return -1;
    }
    char line[4200];
    if (read_line(c, line, sizeof line) < 0) { snprintf(err, errlen, "No response"); return -1; }
    /* "HTTP/1.1 200 OK" or Shoutcast v1 "ICY 200 OK" */
    char *sp = strchr(line, ' ');
    c->status = sp ? atoi(sp + 1) : 0;
    while (read_line(c, line, sizeof line) > 0) {
        char *v = strchr(line, ':');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ') v++;
        if (!strcasecmp(line, "content-type")) snprintf(c->content_type, sizeof c->content_type, "%s", v);
        else if (!strcasecmp(line, "icy-metaint")) c->icy_metaint = atoi(v);
        else if (!strcasecmp(line, "icy-name")) snprintf(c->icy_name, sizeof c->icy_name, "%s", v);
        else if (!strcasecmp(line, "icy-br")) c->icy_br = atoi(v);
        else if (!strcasecmp(line, "location")) snprintf(c->location, sizeof c->location, "%s", v);
        else if (!strcasecmp(line, "content-length")) c->content_length = atol(v);
        else if (!strcasecmp(line, "transfer-encoding") && strstr(v, "chunked")) { c->chunked = 1; c->chunk_left = -1; }
    }
    return 0;
}

int http_open(HttpConn *c, const char *url, int want_icy, char *err, size_t errlen)
{
    char cur[4096];
    volatile int *af = c->abort_flag;
    int av = c->abort_value;
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 6; hop++) {
        memset(c, 0, sizeof *c);
        c->fd = -1;
        c->tls = NULL;
        c->abort_flag = af;
        c->abort_value = av;
        if (open_once(c, cur, want_icy, err, errlen)) { http_close(c); return -1; }
        if (c->status >= 300 && c->status < 400 && c->location[0]) {
            char next[4096];
            if (c->location[0] == '/') { /* relative redirect */
                char host[256], path[4096];
                int port, tls;
                parse_url(cur, host, sizeof host, &port, path, sizeof path, &tls);
                snprintf(next, sizeof next, "%s://%s:%d%s", tls ? "https" : "http", host, port, c->location);
            } else {
                snprintf(next, sizeof next, "%s", c->location);
            }
            snprintf(cur, sizeof cur, "%s", next);
            http_close(c);
            continue;
        }
        if (c->status < 200 || c->status >= 300) {
            snprintf(err, errlen, "HTTP error %d", c->status);
            http_close(c);
            return -1;
        }
        return 0;
    }
    snprintf(err, errlen, "Too many redirects");
    return -1;
}

void http_close(HttpConn *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->tls->ssl);
        tls_free(c);
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

long http_fetch(const char *url, char **out, long max_bytes, char *err, size_t errlen)
{
    HttpConn c;
    memset(&c, 0, sizeof c);
    *out = NULL;
    if (http_open(&c, url, 0, err, errlen)) return -1;
    long cap = 65536, len = 0;
    char *b = malloc(cap + 1);
    if (!b) { http_close(&c); return -1; }
    for (;;) {
        if (len == cap) {
            if (cap >= max_bytes) break;
            cap *= 2;
            char *nb = realloc(b, cap + 1);
            if (!nb) break;
            b = nb;
        }
        int r = http_read(&c, b + len, (int)(cap - len));
        if (r < 0) { if (len == 0) { snprintf(err, errlen, "Read failed"); free(b); http_close(&c); return -1; } break; }
        if (r == 0) break;
        len += r;
    }
    http_close(&c);
    b[len] = 0;
    *out = b;
    return len;
}

void url_encode(const char *s, char *dst, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (; *s && j + 4 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') dst[j++] = (char)ch;
        else { dst[j++] = '%'; dst[j++] = hex[ch >> 4]; dst[j++] = hex[ch & 15]; }
    }
    dst[j] = 0;
}
