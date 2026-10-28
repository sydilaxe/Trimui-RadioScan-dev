#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stream.h"
#include "interp.h"
#include "net.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define RING_N (1 << 18)              /* stereo frames, ~5.9 s @ 44.1 kHz */
#define PREBUFFER_SEC 1.5f

struct Stream {
    int16_t *ring;                    /* RING_N stereo frames */
    volatile unsigned ring_w, ring_r; /* SPSC indices (free-running) */
    volatile int flush_req;
    volatile int underrun;            /* set by audio thread, cleared by worker */

    pthread_t worker;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    char req_url[1024];
    volatile int generation;          /* bumps on every play/stop; aborts the current connection */
    volatile int want_play, quit;

    volatile StreamState state;
    char status_msg[160];
    char title[256];
    volatile int src_rate, bitrate;
    int out_rate;

    /* decoder (worker thread only) */
    mp3dec_t dec;
    unsigned char in[32768];
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    /* resampler (audio thread only) */
    double rs_pos;
    int playing_gate;
    /* declicker (audio thread only): the stream fades in and out over a few ms when it starts, changes station or
     * runs dry, holding its last sample while it fades out, instead of jumping to or from silence */
    float gain, last[2];
    float hist[2];                    /* the frame just before ring_r, for the cubic resampler */
};


#define DECLICK_SEC 0.005f

/* fade out[valid..frames) away from the last sample and out[0..valid) in, by the running gain */
static void declick(Stream *s, float *out, int valid, int frames)
{
    float step = 1.0f / (DECLICK_SEC * s->out_rate);
    for (int i = 0; i < frames; i++) {
        if (i < valid) {
            if (s->gain < 1) s->gain = s->gain + step > 1 ? 1 : s->gain + step;
            s->last[0] = out[i * 2];
            s->last[1] = out[i * 2 + 1];
        } else {
            s->gain = s->gain - step < 0 ? 0 : s->gain - step;
        }
        out[i * 2] = s->last[0] * s->gain;
        out[i * 2 + 1] = s->last[1] * s->gain;
    }
}

static void set_status(Stream *s, StreamState st, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&s->mtx);
    s->state = st;
    va_start(ap, fmt);
    vsnprintf(s->status_msg, sizeof s->status_msg, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&s->mtx);
}

static unsigned ring_avail(Stream *s)
{
    return __atomic_load_n(&s->ring_w, __ATOMIC_ACQUIRE) - __atomic_load_n(&s->ring_r, __ATOMIC_ACQUIRE);
}

static void msleep(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Push decoded PCM; waits while the ring is full. Returns -1 if aborted. */
static int ring_push(Stream *s, const int16_t *pcm, int frames, int channels, int gen)
{
    int i = 0;
    while (i < frames) {
        if (gen != s->generation || s->quit) return -1;
        unsigned free_frames = RING_N - ring_avail(s);
        if (free_frames < 64) { msleep(10); continue; }
        unsigned w = s->ring_w;
        int n = frames - i;
        if ((unsigned)n > free_frames) n = (int)free_frames;
        for (int k = 0; k < n; k++, i++) {
            unsigned idx = (w + k) & (RING_N - 1);
            int16_t l = pcm[i * channels];
            s->ring[idx * 2] = l;
            s->ring[idx * 2 + 1] = channels > 1 ? pcm[i * channels + 1] : l;
        }
        __atomic_store_n(&s->ring_w, w + n, __ATOMIC_RELEASE);
    }
    return 0;
}

static void parse_icy_meta(Stream *s, const char *m)
{
    const char *p = strstr(m, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = p + strlen(p);
    int n = (int)(e - p);
    pthread_mutex_lock(&s->mtx);
    if (n >= (int)sizeof s->title) n = sizeof s->title - 1;
    for (int i = 0; i < n; i++) s->title[i] = (p[i] >= 32 && p[i] < 127) ? p[i] : '?';
    s->title[n] = 0;
    pthread_mutex_unlock(&s->mtx);
}

/* Find the first stream URL in a .pls / .m3u body. */
static int playlist_first_url(const char *body, char *out, int n)
{
    const char *p = body;
    while (*p) {
        const char *line = p;
        const char *e = strpbrk(p, "\r\n");
        int len = e ? (int)(e - line) : (int)strlen(line);
        const char *h = NULL;
        for (int i = 0; i + 7 <= len; i++)
            if (!strncasecmp(line + i, "http://", 7) || !strncasecmp(line + i, "https://", 8)) { h = line + i; break; }
        if (h) {
            int k = len - (int)(h - line);
            while (k > 0 && (h[k - 1] == ' ' || h[k - 1] == '\t')) k--;
            if (k >= n) k = n - 1;
            memcpy(out, h, k);
            out[k] = 0;
            return 0;
        }
        p = e ? e + 1 : line + len;
    }
    return -1;
}

static int is_playlist(const char *url, const char *ctype)
{
    const char *q = strchr(url, '?');
    int len = q ? (int)(q - url) : (int)strlen(url);
    if (len > 5 && !strncasecmp(url + len - 5, ".m3u8", 5)) return 2; /* HLS */
    if (strcasestr(ctype, "apple.mpegurl")) return 2;
    if (len > 4 && (!strncasecmp(url + len - 4, ".pls", 4) || !strncasecmp(url + len - 4, ".m3u", 4))) return 1;
    if (strcasestr(ctype, "scpls") || strcasestr(ctype, "mpegurl") || strcasestr(ctype, "text/")) return 1;
    return 0;
}

static void play_url(Stream *s, const char *start_url, int gen)
{
    char url[1024], err[160];
    snprintf(url, sizeof url, "%s", start_url);
    HttpConn c;

    for (int depth = 0;; depth++) {
        set_status(s, ST_CONNECTING, "Connecting...");
        memset(&c, 0, sizeof c);
        c.abort_flag = &s->generation;
        c.abort_value = gen;
        if (http_open(&c, url, 1, err, sizeof err)) {
            if (gen == s->generation) set_status(s, ST_ERROR, "%s", err);
            return;
        }
        int pl = is_playlist(url, c.content_type);
        if (!pl) break;
        if (pl == 2) {
            http_close(&c);
            set_status(s, ST_ERROR, "HLS (.m3u8) streams not supported");
            return;
        }
        char body[16384];
        int len = 0, r;
        while (len < (int)sizeof body - 1 && (r = http_read(&c, body + len, sizeof body - 1 - len)) > 0) len += r;
        body[len] = 0;
        http_close(&c);
        char next[1024];
        if (depth >= 3 || playlist_first_url(body, next, sizeof next)) {
            set_status(s, ST_ERROR, "Playlist has no stream URL");
            return;
        }
        snprintf(url, sizeof url, "%s", next);
    }

    if (strcasestr(c.content_type, "aac") || strcasestr(c.content_type, "ogg") ||
        strcasestr(c.content_type, "flac") || strcasestr(c.content_type, "opus")) {
        set_status(s, ST_ERROR, "Unsupported format %s (MP3 only)", c.content_type);
        http_close(&c);
        return;
    }
    pthread_mutex_lock(&s->mtx);
    s->title[0] = 0;
    pthread_mutex_unlock(&s->mtx);
    s->bitrate = c.icy_br;

    /* drop audio from the previous station */
    s->flush_req = 1;
    for (int i = 0; i < 50 && s->flush_req; i++) msleep(5);
    set_status(s, ST_BUFFERING, "Buffering...");

    mp3dec_init(&s->dec);
    int in_len = 0;
    int metaint = c.icy_metaint, until_meta = metaint;
    int frames_ok = 0, bad_bytes = 0;
    unsigned char tmp[4096];

    while (gen == s->generation && !s->quit) {
        int want = sizeof tmp;
        if (metaint && want > until_meta) want = until_meta;
        int r = http_read(&c, tmp, want);
        if (r <= 0) {
            if (gen == s->generation) set_status(s, ST_ERROR, r == 0 ? "Stream ended" : "Connection lost");
            break;
        }
        if (metaint) {
            until_meta -= r;
            if (until_meta == 0) { /* ICY metadata block follows every metaint audio bytes */
                unsigned char lb;
                if (http_read(&c, &lb, 1) != 1) break;
                int mlen = lb * 16, got = 0;
                char meta[4081];
                while (got < mlen) {
                    int k = http_read(&c, meta + got, mlen - got);
                    if (k <= 0) break;
                    got += k;
                }
                meta[got] = 0;
                if (got) parse_icy_meta(s, meta);
                until_meta = metaint;
            }
        }
        if (r > (int)sizeof tmp) r = sizeof tmp;
        if (in_len + r > (int)sizeof s->in) in_len = 0; /* resync on garbage */
        memcpy(s->in + in_len, tmp, r);
        in_len += r;

        /* decode whole frames, keeping a tail for a partial frame */
        int pos = 0;
        while (in_len - pos > 2048) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&s->dec, s->in + pos, in_len - pos, s->pcm, &info);
            if (info.frame_bytes == 0) { pos = in_len; break; }
            pos += info.frame_bytes;
            if (samples > 0) {
                frames_ok++;
                bad_bytes = 0;
                if (info.hz && info.hz != s->src_rate) s->src_rate = info.hz;
                if (!s->bitrate) s->bitrate = info.bitrate_kbps;
                if (ring_push(s, s->pcm, samples, info.channels, gen)) { http_close(&c); return; }
            } else {
                bad_bytes += info.frame_bytes;
            }
        }
        memmove(s->in, s->in + pos, in_len - pos);
        in_len -= pos;
        if (frames_ok == 0 && bad_bytes > 200000) {
            set_status(s, ST_ERROR, "No MP3 audio found in stream");
            break;
        }
        if (s->underrun) {
            s->underrun = 0;
            if (s->state == ST_PLAYING) set_status(s, ST_BUFFERING, "Buffering...");
        }
        if (s->state == ST_BUFFERING && ring_avail(s) > (unsigned)(s->src_rate * PREBUFFER_SEC))
            set_status(s, ST_PLAYING, "Playing");
    }
    http_close(&c);
}

/* ------------------------------------------------------------ local .wav files */

static char file_root[PATH_MAX] = ".";

void stream_set_file_root(const char *dir) { snprintf(file_root, sizeof file_root, "%s", dir && dir[0] ? dir : "."); }

int stream_is_file(const char *url) { return !strncasecmp(url, "file://", 7); }

void stream_file_path(const char *url, char *out, int n)
{
    const char *p = stream_is_file(url) ? url + 7 : url;
    if (p[0] == '/') snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", file_root, p);
}

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static inline int16_t to16(double v)
{
    v *= 32768.0;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* one sample of any supported encoding, as 16-bit */
static inline int16_t wav_sample(const unsigned char *p, int fmt, int bits)
{
    if (fmt == 3) {
        if (bits == 32) { float f; memcpy(&f, p, 4); return to16(f); }
        double d; memcpy(&d, p, 8); return to16(d);
    }
    switch (bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 16: return (int16_t)rd16(p);
    case 24: return (int16_t)(p[1] | p[2] << 8);
    default: return (int16_t)(p[2] | p[3] << 8); /* 32 */
    }
}

/* Play a .wav file as if it were a station: decoded into the ring at its own rate, looping forever. */
static void play_wav(Stream *s, const char *url, int gen)
{
    char path[PATH_MAX + 512];
    stream_file_path(url, path, sizeof path);
    set_status(s, ST_CONNECTING, "Opening file...");
    FILE *f = fopen(path, "rb");
    if (!f) { set_status(s, ST_ERROR, "WAV file not found"); return; }

    unsigned char h[40];
    int fmt = 0, ch = 0, bits = 0, rate = 0;
    long data_at = -1, data_len = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        set_status(s, ST_ERROR, "Not a WAV file");
        return;
    }
    while (fread(h, 1, 8, f) == 8) { /* walk the chunks for "fmt " and "data" */
        long len = rd32(h + 4), at = ftell(f);
        if (!memcmp(h, "fmt ", 4) && len >= 16) {
            unsigned char fb[40] = {0};
            if (fread(fb, 1, len < 40 ? len : 40, f) < 16) break;
            fmt = rd16(fb);
            ch = rd16(fb + 2);
            rate = (int)rd32(fb + 4);
            bits = rd16(fb + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(fb + 24); /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
        } else if (!memcmp(h, "data", 4)) {
            data_at = at;
            data_len = len;
            break;
        }
        if (fseek(f, at + len + (len & 1), SEEK_SET)) break; /* chunks are padded to even sizes */
    }
    int ok = (fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && (bits == 32 || bits == 64));
    if (data_at < 0 || !ok || ch < 1 || ch > 32 || rate < 1000 || rate > 384000) {
        fclose(f);
        if (data_at < 0) set_status(s, ST_ERROR, "WAV error: no audio data");
        else set_status(s, ST_ERROR, "Unsupported WAV format (%d-bit, type %d)", bits, fmt);
        return;
    }
    int frame_bytes = ch * bits / 8;
    fseek(f, 0, SEEK_END);
    long avail = ftell(f) - data_at; /* a header may lie about the length (recorders that never closed the file) */
    if (data_len <= 0 || data_len > avail) data_len = avail;
    long frames = data_len / frame_bytes;
    if (frames < 1) { fclose(f); set_status(s, ST_ERROR, "WAV error: no audio data"); return; }

    const char *name = strrchr(path, '/');
    pthread_mutex_lock(&s->mtx);
    snprintf(s->title, sizeof s->title, "%s", name ? name + 1 : path);
    pthread_mutex_unlock(&s->mtx);
    s->bitrate = (int)((long long)rate * ch * bits / 1000);
    s->src_rate = rate;

    s->flush_req = 1; /* drop audio from the previous station */
    for (int i = 0; i < 50 && s->flush_req; i++) msleep(5);
    set_status(s, ST_BUFFERING, "Buffering...");

    enum { BLOCK = 1024 };
    unsigned char *raw = malloc((size_t)BLOCK * frame_bytes);
    int16_t pcm[BLOCK * 2];
    if (!raw) { fclose(f); set_status(s, ST_ERROR, "Out of memory"); return; }
    long pos = frames; /* seek on the first pass */
    while (gen == s->generation && !s->quit) {
        if (pos >= frames) { /* loop: back to the start of the audio */
            fseek(f, data_at, SEEK_SET);
            pos = 0;
        }
        long want = frames - pos < BLOCK ? frames - pos : BLOCK;
        long got = (long)fread(raw, frame_bytes, want, f);
        if (got <= 0) { set_status(s, ST_ERROR, "WAV error: read failed"); break; }
        pos += got;
        for (long i = 0; i < got; i++) {
            const unsigned char *fr = raw + i * frame_bytes;
            pcm[i * 2] = wav_sample(fr, fmt, bits);
            pcm[i * 2 + 1] = ch > 1 ? wav_sample(fr + bits / 8, fmt, bits) : pcm[i * 2];
        }
        if (ring_push(s, pcm, (int)got, 2, gen)) break;
        if (s->underrun) {
            s->underrun = 0;
            if (s->state == ST_PLAYING) set_status(s, ST_BUFFERING, "Buffering...");
        }
        if (s->state == ST_BUFFERING && ring_avail(s) > (unsigned)(s->src_rate * PREBUFFER_SEC))
            set_status(s, ST_PLAYING, "Playing file");
    }
    free(raw);
    fclose(f);
}

static void *worker_main(void *arg)
{
    Stream *s = arg;
    pthread_mutex_lock(&s->mtx);
    while (!s->quit) {
        while (!s->quit && !s->want_play) pthread_cond_wait(&s->cond, &s->mtx);
        if (s->quit) break;
        char url[1024];
        snprintf(url, sizeof url, "%s", s->req_url);
        int gen = s->generation;
        pthread_mutex_unlock(&s->mtx);

        if (stream_is_file(url)) play_wav(s, url, gen);
        else play_url(s, url, gen);

        /* retry after drop-outs while the same request is still wanted */
        for (int i = 0; i < 30 && gen == s->generation && !s->quit; i++) msleep(100);
        pthread_mutex_lock(&s->mtx);
        if (gen != s->generation) continue; /* a new request arrived meanwhile */
        if (s->state == ST_ERROR && (strstr(s->status_msg, "not supported") || strstr(s->status_msg, "Unsupported") ||
                                     strstr(s->status_msg, "No MP3") ||
                                     strstr(s->status_msg, "WAV")))
            s->want_play = 0; /* permanent failure: don't hammer the server (or re-read a bad file) */
    }
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

Stream *stream_create(int out_rate)
{
    Stream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ring = calloc(RING_N * 2, sizeof(int16_t));
    if (!s->ring) { free(s); return NULL; }
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->out_rate = out_rate;
    s->src_rate = 44100;
    snprintf(s->status_msg, sizeof s->status_msg, "No station");
    pthread_create(&s->worker, NULL, worker_main, s);
    return s;
}

void stream_destroy(Stream *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->mtx);
    s->quit = 1;
    s->generation++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    pthread_join(s->worker, NULL);
    free(s->ring);
    free(s);
}

void stream_play(Stream *s, const char *url)
{
    pthread_mutex_lock(&s->mtx);
    snprintf(s->req_url, sizeof s->req_url, "%s", url);
    s->generation++;
    s->want_play = 1;
    s->state = ST_CONNECTING;
    snprintf(s->status_msg, sizeof s->status_msg, "Connecting...");
    s->title[0] = 0;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mtx);
}

void stream_stop(Stream *s)
{
    pthread_mutex_lock(&s->mtx);
    s->generation++;
    s->want_play = 0;
    s->state = ST_IDLE;
    snprintf(s->status_msg, sizeof s->status_msg, "No station");
    s->title[0] = 0;
    pthread_mutex_unlock(&s->mtx);
    s->flush_req = 1;
}

void stream_read(Stream *s, float *out, int frames)
{
    if (s->flush_req) {
        __atomic_store_n(&s->ring_r, __atomic_load_n(&s->ring_w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        s->rs_pos = 0;
        s->playing_gate = 0;
        s->flush_req = 0;
        s->hist[0] = s->hist[1] = 0;
    }
    unsigned avail = ring_avail(s);
    if (!s->playing_gate && avail > (unsigned)(s->src_rate * PREBUFFER_SEC)) s->playing_gate = 1;
    if (!s->playing_gate) {
        declick(s, out, 0, frames);
        return;
    }
    const double step = (double)s->src_rate / s->out_rate;
    const float sc = 1.0f / 32768.0f;
    const int16_t *ring = s->ring;
    unsigned r = s->ring_r;
    int i;
    for (i = 0; i < frames; i++) { /* cubic resampling from the station's rate to ours: clean highs, no grit */
        unsigned ip = (unsigned)s->rs_pos;
        if (ip + 3 > avail) break; /* starved */
        float fr = (float)(s->rs_pos - ip);
        unsigned a = (r + ip) & (RING_N - 1), b = (a + 1) & (RING_N - 1), c = (a + 2) & (RING_N - 1);
        unsigned m = (a - 1) & (RING_N - 1);
        for (int ch = 0; ch < 2; ch++) {
            float xm1 = ip ? ring[m * 2 + ch] * sc : s->hist[ch];
            out[i * 2 + ch] = hermite(xm1, ring[a * 2 + ch] * sc, ring[b * 2 + ch] * sc, ring[c * 2 + ch] * sc, fr);
        }
        s->rs_pos += step;
    }
    unsigned adv = (unsigned)s->rs_pos;
    if (adv > avail) adv = avail;
    if (adv) { /* keep the last consumed frame: the writer may reuse its slot */
        unsigned h = (r + adv - 1) & (RING_N - 1);
        s->hist[0] = ring[h * 2] * sc;
        s->hist[1] = ring[h * 2 + 1] * sc;
    }
    s->rs_pos -= adv;
    __atomic_store_n(&s->ring_r, r + adv, __ATOMIC_RELEASE);
    if (i < frames) {
        s->playing_gate = 0; /* underrun: rebuffer (no locking in the audio thread) */
        s->underrun = 1;
    }
    declick(s, out, i, frames);
}

StreamState stream_state(Stream *s) { return s->state; }

void stream_status(Stream *s, char *buf, int n)
{
    pthread_mutex_lock(&s->mtx);
    snprintf(buf, n, "%s", s->status_msg);
    pthread_mutex_unlock(&s->mtx);
}

void stream_title(Stream *s, char *buf, int n)
{
    pthread_mutex_lock(&s->mtx);
    snprintf(buf, n, "%s", s->title);
    pthread_mutex_unlock(&s->mtx);
}

int stream_src_rate(Stream *s) { return s->src_rate; }
int stream_bitrate(Stream *s) { return s->bitrate; }
float stream_buffer_seconds(Stream *s) { return (float)ring_avail(s) / (float)s->src_rate; }
