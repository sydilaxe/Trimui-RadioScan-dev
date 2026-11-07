/* MIDI over USB (see midi.h). The parser and the ports run in the MIDI
 * thread; notes and CCs are queued for the main thread, which plays them as
 * the buttons do. The clock (in, out, and the Brick link) is handled here,
 * as it comes, through the main thread's clock (midi_set_clock). */
#define _GNU_SOURCE
#include "midi.h"
#include "aclock.h"
#include "link.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------- notices */

static pthread_mutex_t nmx = PTHREAD_MUTEX_INITIALIZER;
static char notices[8][48];
static int n_head, n_tail;

void midi_notice(const char *fmt, const char *arg)
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

/* ------------------------------------------------------------- the clock */

static const MidiClock *clk;
static int clock_out = 1, follow = 1; /* RS_MIDI_CLOCK=0, RS_MIDI_FOLLOW=0 */
static volatile float latency_s = 0.03f;
/* the clock is driven from the MIDI thread, and from midi_feed in the tests */
static pthread_mutex_t clk_mx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

void midi_set_clock(const MidiClock *c) { clk = c; }
void midi_set_latency(float seconds) { latency_s = seconds < 0.f ? 0.f : seconds; }

static int mode(void) { return clk ? clk->mode() : MIDI_OFF; }
static int can_follow(void) { return clk && follow && (mode() == MIDI_AUTO || mode() == MIDI_FOLLOW); }
static int can_send(void) { return clk && clock_out && (mode() == MIDI_AUTO || mode() == MIDI_SEND); }
/* START, STOP, CONTINUE and song position too, unless set to the clock only */
static int can_transport(void) { return can_follow() && !clk->clock_only(); }

static uint64_t tick_us, ext_seen_us;
static double tick_avg_us;
static int64_t ticks;          /* since START (or the song position) */
static volatile int following; /* an outside clock leads */
static int ext_running;
static int out_running; /* the clock going out */
static uint64_t precise_us; /* the last position from a leading Brick: its tempo and phase win over the ticks' */
static double pll_i;

int midi_following(void) { return following; }

/* where the sound is now: the audio clock, or the clock less the time to the speaker */
static double heard_now(uint64_t t)
{
    double h;
    return aclock_position(t, &h) ? h : clk->position() - latency_s * clk->bpm() / 15.0;
}

/* the last ticks' times, for the tempo: a straight line through four beats
 * of them, which averages out the jitter of USB and the thread waking */
#define TK 96
static uint64_t tk_t[TK];
static int tk_n, tk_w;

static void clock_tick(uint64_t t)
{
    if (tick_us && (t <= tick_us || t - tick_us >= 250000))
        tk_n = 0; /* a gap: the fit starts afresh */
    tk_t[tk_w] = t;
    tk_w = (tk_w + 1) % TK;
    if (tk_n < TK)
        tk_n++;
    if (tk_n >= 2) {
        uint64_t t0 = tk_t[(tk_w - tk_n + TK) % TK];
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = tk_n;
        for (int i = 0; i < tk_n; i++) {
            double y = (double)(tk_t[(tk_w - tk_n + i + TK) % TK] - t0);
            sx += i, sy += y, sxx += (double)i * i, sxy += i * y;
        }
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        if (slope > 0.0)
            tick_avg_us = slope;
    }
    tick_us = t;
    ext_seen_us = t;
    if (!can_follow() || tick_avg_us <= 0.0)
        return;
    following = 1;
    if (precise_us && t - precise_us < 1000000) { /* a leading Brick's positions hold the tempo and phase */
        ticks++;
        return;
    }
    /* the tempo: set when it has really changed (the phase lock below takes
     * up what is left), quickly while the fit is young */
    float bpm = (float)(60e6 / (tick_avg_us * 24.0));
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    if (fabsf(bpm - clk->bpm()) > (tk_n < 24 ? 0.05f : 0.15f))
        clk->set_bpm(roundf(bpm * 100.f) / 100.f);
    if (!ext_running || !clk->playing())
        return;
    ticks++;
    /* the phase: what is heard (the audio clock) should be where the clock
     * is; trimmed gently, jumped only when far off, and a little integral
     * takes up the tempo's last difference */
    double err = ticks / 6.0 - heard_now(t);
    if (fabs(err) > 2.0) {
        clk->set_position(clk->position() + err);
        clk->set_rate(1.0);
        pll_i = 0.0;
    }
    else {
        pll_i = fmax(-0.004, fmin(0.004, pll_i + err * 0.002));
        clk->set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.05 + pll_i)));
    }
}

static void realtime(uint8_t b, uint64_t t)
{
    switch (b) {
    case 0xf8: clock_tick(t); break;
    case 0xfa: /* START: from the top */
        if (!can_transport())
            break;
        ticks = 0;
        ext_running = 1;
        clk->set_position(latency_s * clk->bpm() / 15.0);
        clk->set_rate(1.0);
        pll_i = 0.0;
        clk->set_playing(1);
        break;
    case 0xfb: /* CONTINUE: from the song position */
        if (!can_transport())
            break;
        ext_running = 1;
        clk->set_playing(1);
        break;
    case 0xfc: /* STOP */
        if (!can_transport())
            break;
        ext_running = 0;
        clk->set_rate(1.0);
        clk->set_playing(0);
        break;
    }
}

/* A leading Brick's position over the USB clock link (link.h), about 50 times
 * a second: F0 7D 'T' 'L', where its sound is (the 16ths heard at the moment
 * it was sent, x 65536, five 7-bit bytes, low first), its exact tempo (BPM x
 * 1000, three bytes) and whether it plays, F7. The same in Columbia Street,
 * HI5 and Jaffedrone, so any of them can lead this one. 7D is MIDI's id for
 * non-commercial use, which other gear passes over. The follower turns its
 * own heard position (the audio clock, aclock) toward it: a jump when more
 * than half a 16th off, else a slight change of speed, so the two sound
 * together within a fraction of a millisecond. Set to the clock only, it
 * lines up the bar but keeps its own count, and doesn't start or stop with
 * the leader. */
#define LINK_TRANSIT_S 0.0003 /* a bulk transfer through a hub and the follower's thread waking */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing)
{
    if (max < 14)
        return 0;
    uint64_t p = heard > 0.0 ? (uint64_t)llround(heard * 65536.0) : 0;
    uint32_t b = (uint32_t)lround(bpm * 1000.0);
    int n = 0;
    out[n++] = 0xf0, out[n++] = 0x7d, out[n++] = 'T', out[n++] = 'L';
    for (int k = 0; k < 5; k++)
        out[n++] = (uint8_t)(p >> (7 * k) & 127);
    for (int k = 0; k < 3; k++)
        out[n++] = (uint8_t)(b >> (7 * k) & 127);
    out[n++] = playing ? 1 : 0;
    out[n++] = 0xf7;
    return n;
}

static void link_position(uint64_t t, double lead, double bpm, int lead_playing)
{
    if (!can_follow())
        return;
    ext_seen_us = precise_us = t;
    following = 1;
    if (bpm >= 40.0 && bpm <= 250.0 && fabs(bpm - clk->bpm()) > 1e-4)
        clk->set_bpm((float)bpm);
    if (!clk->playing()) {
        if (lead_playing && can_transport()) { /* joined mid-song: in at the leader's place */
            clk->set_playing(1);
            clk->set_position(lead + (LINK_TRANSIT_S + latency_s) * bpm / 15.0);
            ext_running = 1;
            pll_i = 0.0;
        }
        return;
    }
    if (!lead_playing) { /* the leader stopped (a leader passing on another's clock sends no STOP) */
        if (can_transport()) {
            ext_running = 0;
            clk->set_rate(1.0);
            clk->set_playing(0);
        }
        return;
    }
    double err = lead + LINK_TRANSIT_S * bpm / 15.0 - heard_now(t);
    if (!can_transport()) /* the bar only: the nearest way round */
        err -= 16.0 * floor((err + 8.0) / 16.0);
    if (fabs(err) > 0.5) {
        clk->set_position(clk->position() + err);
        clk->set_rate(1.0);
        pll_i = 0.0;
        return;
    }
    pll_i = fmax(-0.003, fmin(0.003, pll_i + err * 0.02));
    clk->set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.4 + pll_i)));
}

static uint8_t status, data[2];
static int ndata, in_sysex;
static uint8_t sx[16]; /* a SysEx being read: only the Brick link's position is wanted */
static int nsx;

static void sysex_end(uint64_t t)
{
    if (nsx != 12 || sx[0] != 0x7d || sx[1] != 'T' || sx[2] != 'L')
        return;
    uint64_t p = 0;
    uint32_t b = 0;
    for (int k = 0; k < 5; k++)
        p |= (uint64_t)sx[3 + k] << (7 * k);
    for (int k = 0; k < 3; k++)
        b |= (uint32_t)sx[8 + k] << (7 * k);
    link_position(t, p / 65536.0, b / 1000.0, sx[11] & 1);
}

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
    if (status == 0xf2 && can_transport()) { /* song position, in 16ths */
        ticks = (int64_t)((data[1] << 7) | data[0]) * 6;
        if (clk->playing())
            clk->set_position(ticks / 6.0 + latency_s * clk->bpm() / 15.0);
    }
}

void midi_feed_at(const uint8_t *b, int n, uint64_t t)
{
    pthread_mutex_lock(&clk_mx);
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) { /* real time: anywhere, even inside another message */
            if (clk)
                realtime(c, t);
            continue;
        }
        if (c == 0xf0) {
            in_sysex = 1;
            nsx = 0;
            continue;
        }
        if (c == 0xf7) {
            if (in_sysex && clk)
                sysex_end(t);
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
        if (in_sysex) {
            if (nsx < (int)sizeof sx)
                sx[nsx++] = c;
            else
                nsx = (int)sizeof sx + 1; /* too long: not ours */
            continue;
        }
        if (!status)
            continue;
        data[ndata++] = c;
        if (ndata >= data_len(status)) {
            message();
            ndata = 0; /* running status: the same kind again */
            if (status >= 0xf0)
                status = 0;
        }
    }
    pthread_mutex_unlock(&clk_mx);
}

void midi_feed(const uint8_t *b, int n) { midi_feed_at(b, n, now_us()); }

void midi_reset(void)
{
    pthread_mutex_lock(&clk_mx);
    status = 0, ndata = 0, in_sysex = 0, nsx = 0;
    mq_r = mq_w;
    tick_us = ext_seen_us = 0;
    tick_avg_us = 0.0;
    ticks = 0;
    following = ext_running = 0;
    precise_us = 0, pll_i = 0.0;
    tk_n = tk_w = 0;
    out_running = 0;
    pthread_mutex_unlock(&clk_mx);
}

/* ------------------------------------------------------------ clock out */

static double out_est;
static int64_t out_sent;
static uint64_t out_t;

int midi_clock_step(uint64_t t, uint8_t *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&clk_mx);
    int play = clk && clk->playing() && can_send() && !following;
    if (!play) {
        if (out_running && n < max)
            out[n++] = 0xfc;
        out_running = 0;
        pthread_mutex_unlock(&clk_mx);
        return n;
    }
    double bpm = clk->bpm();
    /* where the sound is, through whichever output plays it (a USB audio
     * interface's own buffering included): the audio clock */
    double heard = heard_now(t);
    if (!out_running) { /* START now: the next tick is the first */
        out_running = 1;
        out_est = heard;
        out_sent = (int64_t)floor(heard * 6.0);
        out_t = t;
        if (n < max)
            out[n++] = 0xfa;
        pthread_mutex_unlock(&clk_mx);
        return n;
    }
    /* the clock's position moves in blocks: a smooth estimate between them,
     * held to it */
    out_est += (double)(t - out_t) * 1e-6 * bpm / 15.0;
    out_t = t;
    double d = heard - out_est;
    out_est += fabs(d) > 1.0 ? d : d * 0.02;
    int64_t want = (int64_t)floor(out_est * 6.0);
    if (want - out_sent > 4) /* a jump (a new position): no burst of ticks */
        out_sent = want - 1;
    while (out_sent < want && n < max) {
        out[n++] = 0xf8;
        out_sent++;
    }
    if (want < out_sent)
        out_sent = want; /* moved back: wait for it */
    pthread_mutex_unlock(&clk_mx);
    return n;
}

/* the position for the Bricks following over the link, every 20 ms while the
 * clock goes out, or while this one follows another clock (passing it on):
 * the bytes to send now, or 0 */
static uint64_t link_sent_us;
int midi_link_step(uint64_t t, uint8_t *out, int max)
{
    pthread_mutex_lock(&clk_mx);
    int n = 0;
    if (!clk || (!out_running && !following))
        link_sent_us = 0;
    else if (!link_sent_us || t - link_sent_us >= 20000) {
        link_sent_us = t;
        n = midi_link_pos(out, max, heard_now(t), clk->bpm(), clk->playing());
    }
    pthread_mutex_unlock(&clk_mx);
    return n;
}

/* --------------------------------------------------------------- ports */

#define MAXPORT 8
/* a raw MIDI port, or a Brick following this one over the USB clock link
 * (link.h): its usbfs device, written with bulk transfers to its endpoint */
typedef struct {
    int fd;
    int link_ep, dead; /* a link: its bulk OUT endpoint (0 for a MIDI port); it went */
    char path[300], name[32];
} Port;
static Port ports[MAXPORT];
static volatile int nports, nlinks;
int midi_ports(void) { return nports; }
int midi_links(void) { return nlinks; }
static void count_links(void)
{
    int n = 0;
    for (int i = 0; i < nports; i++)
        n += ports[i].link_ep != 0;
    nlinks = n;
}

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

static void drop(int i);

/* Bricks following this one over the USB clock link: new ones opened, gone ones dropped */
static void scan_links(void)
{
    LinkDev ld[MAXPORT];
    int n = link_find("/sys/bus/usb/devices", ld, MAXPORT);
    for (int i = nports - 1; i >= 0; i--) {
        if (!ports[i].link_ep)
            continue;
        int still = 0;
        for (int k = 0; k < n && !ports[i].dead; k++)
            still |= !strcmp(ld[k].path, ports[i].path);
        if (!still)
            drop(i);
    }
    for (int k = 0; k < n && nports < MAXPORT; k++) {
        int known = 0;
        for (int i = 0; i < nports; i++)
            known |= ports[i].link_ep && !strcmp(ports[i].path, ld[k].path);
        if (known)
            continue;
        char dev[64];
        snprintf(dev, sizeof dev, "/dev/bus/usb/%03d/%03d", ld[k].bus, ld[k].dev);
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        unsigned iface = 0;
        if (fd < 0 || ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
            if (fd >= 0)
                close(fd);
            continue;
        }
        Port *p = &ports[nports++];
        memset(p, 0, sizeof *p);
        p->fd = fd;
        p->link_ep = ld[k].ep;
        snprintf(p->path, sizeof p->path, "%s", ld[k].path);
        snprintf(p->name, sizeof p->name, "BRICK LINK");
        midi_notice("LINK: %.40s", "a Brick follows this one");
    }
    count_links();
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
        int fd = open(path, O_RDWR | O_NONBLOCK); /* to send clock; a port that only sends to us opens to read */
        if (fd < 0)
            fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        Port *p = &ports[nports++];
        memset(p, 0, sizeof *p);
        p->fd = fd;
        snprintf(p->path, sizeof p->path, "%s", path);
        port_name(card, p->name, sizeof p->name);
        midi_notice("MIDI: %.40s", p->name);
    }
    closedir(d);
    scan_links();
}

static void drop(int i)
{
    if (ports[i].link_ep)
        midi_notice("LINK: %.40s", "a following Brick went");
    else
        midi_notice("MIDI: %.34s gone", ports[i].name);
    close(ports[i].fd);
    ports[i] = ports[--nports];
    count_links();
}

/* to one Brick following over the link: a bulk transfer, given up after 20 ms */
static void send_link(Port *p, const uint8_t *b, int n)
{
    if (p->dead)
        return;
    struct usbdevfs_bulktransfer bt = {.ep = (unsigned)p->link_ep, .len = (unsigned)n, .timeout = 20, .data = (void *)b};
    if (ioctl(p->fd, USBDEVFS_BULK, &bt) < 0 && (errno == ENODEV || errno == ESHUTDOWN))
        p->dead = 1; /* dropped at the next scan */
}

static void send_all(const uint8_t *b, int n)
{
    for (int i = 0; i < nports; i++) {
        if (ports[i].link_ep) {
            send_link(&ports[i], b, n);
            continue;
        }
        if (write(ports[i].fd, b, (size_t)n) < 0) {
            /* a full or vanished port, or one opened only to read: a port that went away is dropped when read */
        }
    }
}

/* to the Bricks following over the link only */
static void send_links(const uint8_t *b, int n)
{
    for (int i = 0; i < nports; i++)
        if (ports[i].link_ep)
            send_link(&ports[i], b, n);
}

/* ------------------------------------------------------------- thread */

static pthread_t th;
static volatile int running;

static void *loop(void *arg)
{
    (void)arg;
    uint64_t next_scan = 0;
    while (running) {
        uint64_t t = now_us();
        if (t >= next_scan) {
            scan();
            next_scan = t + 1000000;
        }
        struct pollfd pf[MAXPORT];
        for (int i = 0; i < nports; i++)
            pf[i].fd = ports[i].link_ep ? -1 : ports[i].fd, pf[i].events = POLLIN, pf[i].revents = 0; /* links only send */
        /* every millisecond while a clock comes in or could go out; else only the scan to do */
        int busy = nports && (following || out_running || (clk && clk->playing() && can_send()));
        poll(pf, (nfds_t)nports, busy ? 1 : nports ? 20 : 200);
        t = now_us();
        for (int i = nports - 1; i >= 0; i--) {
            if (!(pf[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            uint8_t buf[256];
            ssize_t r = read(ports[i].fd, buf, sizeof buf);
            if (r > 0)
                midi_feed_at(buf, (int)r, t);
            else if (r == 0 || (errno != EAGAIN && errno != EINTR))
                drop(i);
        }
        pthread_mutex_lock(&clk_mx);
        if (following && (t - ext_seen_us > 1000000 || !can_follow())) { /* the outside clock has stopped, or MIDI is set not to follow it */
            following = ext_running = 0;
            if (clk)
                clk->set_rate(1.0);
        }
        pthread_mutex_unlock(&clk_mx);
        uint8_t out[16];
        int n = midi_clock_step(t, out, sizeof out);
        if (n)
            send_all(out, n);
        uint8_t pos[16];
        if (nlinks && (n = midi_link_step(t, pos, sizeof pos)) > 0)
            send_links(pos, n);
    }
    uint8_t stop = 0xfc;
    if (out_running)
        send_all(&stop, 1);
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
    v = getenv("RS_MIDI_CLOCK");
    clock_out = !(v && *v == '0');
    v = getenv("RS_MIDI_FOLLOW");
    follow = !(v && *v == '0');
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
