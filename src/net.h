/* Minimal blocking HTTP/1.1 client with HTTPS (mbedTLS), redirects,
 * chunked transfer decoding and abortable reads. */
#ifndef NET_H
#define NET_H

#include <stddef.h>

struct TlsSession;

typedef struct {
    int fd;
    struct TlsSession *tls; /* NULL for plain HTTP */
    int chunked;
    long chunk_left;     /* bytes left in current chunk, -1 = need header */
    int eof;
    char buf[8192];      /* read-ahead */
    int buf_len, buf_pos;
    int status;
    char content_type[128];
    int icy_metaint;
    char icy_name[128];
    int icy_br;
    char location[4096];   /* redirects: GitHub's signed download links run to ~1000 characters */
    long content_length;   /* -1 if the server didn't say */
    volatile int *abort_flag; /* optional: read() returns -1 once *abort_flag != abort_value */
    int abort_value;
} HttpConn;

/* Load CA certificates (PEM bundle). Call once at startup; safe to skip (HTTPS then unverified). */
int net_init(const char *ca_bundle_path);

/* Open url (follows up to 5 redirects). Returns 0 on success (2xx), -1 on error with err filled. */
int http_open(HttpConn *c, const char *url, int want_icy, char *err, size_t errlen);
/* Read up to n body bytes. Returns >0 bytes, 0 at EOF, -1 on error/abort. Timeouts after 15 s of silence. */
int http_read(HttpConn *c, void *dst, int n);
void http_close(HttpConn *c);
/* Fetch a whole (small) body into a malloc'd NUL-terminated buffer. Returns length or -1. */
long http_fetch(const char *url, char **out, long max_bytes, char *err, size_t errlen);
/* Percent-encode s into dst. */
void url_encode(const char *s, char *dst, size_t n);

#endif
