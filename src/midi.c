/* MIDI over USB (see midi.h). The parser and the ports run in the MIDI
 * thread; what it hears is queued for the main thread, which plays it as the
 * buttons do. */
#define _GNU_SOURCE
#include "midi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------- notices */

static pthread_mutex_t nmx = PTHREAD_MUTEX_INITIALIZER;
static char notices[8][48];
static int n_head, n_tail;

static void notice(const char *fmt, const char *arg)
{
    pthread_mutex_lock(&nmx);
    if (n_head - n_tail < 8) {
        snprintf(notices[n_head % 8], sizeof notices[0], fmt, arg);
        n_head++;
    }
    pthread_mutex_unlock(&nmx);
}

int midi_take_notice(char *buf, int n)
{
    int got = 0;
    pthread_mutex_lock(&nmx);
    if (n_tail < n_head) {
        snprintf(buf, (size_t)n, "%s", notices[n_tail % 8]);
        n_tail++;
        got = 1;
    }
    pthread_mutex_unlock(&nmx);
    return got;
}

/* --------------------------------------------------- the queue, the parser */

/* one writer (the MIDI thread), one reader (the main thread) */
#define MQ 512
static volatile uint32_t mq[MQ];
static volatile unsigned mq_w, mq_r;

static void put(int kind, int ch, int a, int b)
{
    if (mq_w - mq_r >= MQ)
        return; /* full: the newest are dropped */
    mq[mq_w % MQ] = (uint32_t)kind << 24 | (uint32_t)(ch & 15) << 16 | (uint32_t)(a & 127) << 8 | (uint32_t)(b & 127);
    __sync_synchronize();
    mq_w++;
}

int midi_take(int *kind, int *ch, int *a, int *b)
{
    if (mq_r == mq_w)
        return 0;
    __sync_synchronize();
    uint32_t m = mq[mq_r % MQ];
    __sync_synchronize();
    mq_r++;
    *kind = (int)(m >> 24), *ch = (int)(m >> 16 & 15), *a = (int)(m >> 8 & 127), *b = (int)(m & 127);
    return 1;
}

static uint8_t status, data[2];
static int ndata, in_sysex;

static int data_len(uint8_t st)
{
    switch (st & 0xf0) {
    case 0xc0:
    case 0xd0: return 1;
    case 0xf0: return st == 0xf2 ? 2 : st == 0xf1 || st == 0xf3 ? 1 : 0;
    default: return 2;
    }
}

static void message(void)
{
    switch (status & 0xf0) {
    case 0x90: put(data[1] ? 0x90 : 0x80, status & 15, data[0], data[1]); break; /* velocity 0: a note off */
    case 0x80: put(0x80, status & 15, data[0], 0); break;
    case 0xb0: put(0xb0, status & 15, data[0], data[1]); break;
    }
}

void midi_feed(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) /* real time (clock and the like): not used, and never breaks a message */
            continue;
        if (c == 0xf0) {
            in_sysex = 1;
            continue;
        }
        if (c == 0xf7) {
            in_sysex = 0;
            continue;
        }
        if (c & 0x80) {
            in_sysex = 0;
            status = c;
            ndata = 0;
            if (data_len(status) == 0)
                status = 0;
            continue;
        }
        if (in_sysex || !status)
            continue;
        data[ndata++] = c;
        if (ndata >= data_len(status)) {
            message();
            ndata = 0; /* running status: the same kind again */
            if (status >= 0xf0)
                status = 0;
        }
    }
}

void midi_reset(void)
{
    status = 0, ndata = 0, in_sysex = 0;
    mq_r = mq_w;
}

/* --------------------------------------------------------------- ports */

#define MAXPORT 8
typedef struct {
    int fd;
    char path[40], name[32];
} Port;
static Port ports[MAXPORT];
static int nports;

static void port_name(int card, char *b, int n)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    FILE *f = fopen(p, "r");
    b[0] = 0;
    if (f) {
        if (fgets(b, n, f))
            b[strcspn(b, "\n")] = 0;
        fclose(f);
    }
    if (!b[0])
        snprintf(b, (size_t)n, "card %d", card);
}

static void scan(void)
{
    DIR *d = opendir("/dev/snd");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nports < MAXPORT) {
        int card, devn;
        if (sscanf(e->d_name, "midiC%dD%d", &card, &devn) != 2)
            continue;
        char path[40];
        snprintf(path, sizeof path, "/dev/snd/%.30s", e->d_name);
        int known = 0;
        for (int i = 0; i < nports; i++)
            known |= !strcmp(ports[i].path, path);
        if (known)
            continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        Port *p = &ports[nports++];
        p->fd = fd;
        snprintf(p->path, sizeof p->path, "%s", path);
        port_name(card, p->name, sizeof p->name);
        notice("MIDI: %.40s", p->name);
    }
    closedir(d);
}

static void drop(int i)
{
    notice("MIDI: %.34s gone", ports[i].name);
    close(ports[i].fd);
    ports[i] = ports[--nports];
}

/* ------------------------------------------------------------- thread */

static pthread_t th;
static volatile int running;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static void *loop(void *arg)
{
    (void)arg;
    uint64_t next_scan = 0;
    while (running) {
        uint64_t t = now_ms();
        if (t >= next_scan) {
            scan();
            next_scan = t + 1000;
        }
        struct pollfd pf[MAXPORT];
        for (int i = 0; i < nports; i++)
            pf[i].fd = ports[i].fd, pf[i].events = POLLIN, pf[i].revents = 0;
        poll(pf, (nfds_t)nports, nports ? 20 : 200); /* no port: only the scan to do */
        for (int i = nports - 1; i >= 0; i--) {
            if (!(pf[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            uint8_t buf[256];
            ssize_t r = read(ports[i].fd, buf, sizeof buf);
            if (r > 0)
                midi_feed(buf, (int)r);
            else if (r == 0 || (errno != EAGAIN && errno != EINTR))
                drop(i);
        }
    }
    for (int i = 0; i < nports; i++)
        close(ports[i].fd);
    nports = 0;
    return NULL;
}

void midi_start(void)
{
    const char *v = getenv("RS_MIDI");
    if (v && *v == '0')
        return;
    midi_reset();
    running = 1;
    if (pthread_create(&th, NULL, loop, NULL) != 0)
        running = 0;
}

void midi_stop(void)
{
    if (!running)
        return;
    running = 0;
    pthread_join(th, NULL);
}
