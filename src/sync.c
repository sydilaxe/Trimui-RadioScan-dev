#include "sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "aclock.h"

#define MAX_PEERS 8
#define PEER_TIMEOUT_US 3000000ULL   /* forget a unit after 3 s of silence */
#define SEND_INTERVAL_US 100000ULL   /* 10 packets per second */
#define IFACE_REFRESH_US 5000000ULL  /* re-read broadcast addresses (Wi-Fi may reconnect) */
#define ERR_WINDOW 8                 /* phase errors kept for the median */
#define BAR_STEPS 16.0
#define JUMP_STEPS 0.5               /* farther off than this: jump instead of trimming */
#define TRIM_GAIN 0.15
#define TRIM_MAX 0.03
#define N_LAGS 64                    /* recent packets' travel times kept per unit, each way */
#define LAG_WINDOW_US 8000000ULL     /* the fastest of these, lately, is the baseline */
#define ECHO_MAX_US 1000000ULL       /* echo only packets heard this recently */

/* heard minus sent, for recent packets: the two clocks' difference plus the trip */
typedef struct {
    int64_t v[N_LAGS];
    uint64_t at[N_LAGS];
    int n, head;
} Lags;

typedef struct {
    uint32_t id;
    uint64_t seen_us;
    int playing, mode;
    float bpm;
    double pos;
    uint64_t sent_us;   /* the clock in its last packet (its clock) */
    uint64_t heard_us;  /* when that packet arrived (ours) */
    /* Its packets to us. The clocks' part holds still, so the fastest marks a quick trip, and a packet slower than
     * that was held up by that much more (Wi-Fi queues, broadcasts held for a power-saving unit's beacon). */
    Lags down;
    /* Ours to it, from its echoes: the clocks' difference the other way round. The fastest each way, added, cancel
     * the clocks out and leave two quick trips. */
    Lags up;
} Peer;

static const SyncClock *clk;
static SyncStatus status;
static Peer peers[MAX_PEERS];
static double errs[ERR_WINDOW];
static int n_errs, err_head;

static int sock = -1, echo_next;
static struct sockaddr_in targets[8];
static int n_targets;
static uint64_t last_send_us, last_iface_us;

/* ------------------------------------------------------------ protocol core */

static void wr32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }
static uint32_t rd32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

static int mode_rank(int mode) { return mode == SYNC_LEAD ? 0 : mode == SYNC_AUTO ? 1 : 3; }

static void reset_errors(void) { n_errs = err_head = 0; }

static uint64_t rd64(const uint8_t *b) { uint64_t v; memcpy(&v, b, 8); return v; }

/* the position being heard at time t: the audio clock's, or the clock's own without sound */
static double heard_position(uint64_t t)
{
    double p;
    return aclock_position(t, &p) ? p : clk->position();
}

static void lags_add(Lags *l, int64_t v, uint64_t now_us)
{
    l->v[l->head] = v;
    l->at[l->head] = now_us;
    l->head = (l->head + 1) % N_LAGS;
    if (l->n < N_LAGS) l->n++;
}

static int lags_min(const Lags *l, uint64_t now_us, int64_t *min)
{
    int found = 0;
    for (int i = 0; i < l->n; i++)
        if (now_us - l->at[i] <= LAG_WINDOW_US && (!found || l->v[i] < *min)) { *min = l->v[i]; found = 1; }
    return found;
}

/* the quickest trip one way, in us; -1 until the unit has echoed us */
static double quick_trip_us(const Peer *p, uint64_t now_us)
{
    int64_t down, up;
    if (!lags_min(&p->down, now_us, &down) || !lags_min(&p->up, now_us, &up)) return -1.0;
    double q = (double)(down + up) / 2.0;
    return q < 0.0 ? 0.0 : q;
}

/* how long the packet just heard from p took to arrive, in us */
static double trip_us(const Peer *p, uint64_t now_us)
{
    int64_t last = p->down.v[(p->down.head + N_LAGS - 1) % N_LAGS], fast = last;
    lags_min(&p->down, now_us, &fast);
    double quick = quick_trip_us(p, now_us);
    double t = (double)(last - fast) + (quick < 0.0 ? 0.0 : quick); /* older units can't echo: count it as no time */
    return t > 1e6 ? 1e6 : t;
}

void sync_core_init(const SyncClock *clock, uint32_t id)
{
    clk = clock;
    memset(&status, 0, sizeof status);
    memset(peers, 0, sizeof peers);
    status.id = id;
    status.delay_ms = -1.0f;
    echo_next = 0;
    reset_errors();
}

int sync_make_packet(uint8_t *b, uint64_t now_us)
{
    memset(b, 0, SYNC_PKT_LEN);
    memcpy(b, "HI5S", 4);
    b[4] = 1;
    b[5] = (uint8_t)(clk->playing() ? 1 : 0);
    b[6] = (uint8_t)clk->mode();
    wr32(b + 8, status.id);
    float bpm = clk->bpm();
    double pos = heard_position(now_us) + (clk->playing() ? SYNC_WIRE_LEAD_US * bpm / 15e6 : 0.0);
    memcpy(b + 12, &bpm, 4);
    memcpy(b + 16, &pos, 8);
    memcpy(b + 24, &now_us, 8);
    /* echo one unit heard lately, each in turn, so it can time the trip */
    for (int k = 0; k < MAX_PEERS; k++) {
        int i = (echo_next + k) % MAX_PEERS;
        const Peer *p = &peers[i];
        if (!p->seen_us || now_us < p->heard_us || now_us - p->heard_us > ECHO_MAX_US) continue;
        uint32_t hold = (uint32_t)(now_us - p->heard_us);
        b[7] = 1;
        wr32(b + 32, p->id);
        memcpy(b + 36, &p->sent_us, 8);
        wr32(b + 44, hold);
        echo_next = (i + 1) % MAX_PEERS;
        break;
    }
    return SYNC_PKT_LEN;
}

static Peer *find_peer(uint32_t id, uint64_t now_us)
{
    Peer *spare = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].seen_us && peers[i].id == id) return &peers[i];
        if (!spare && (!peers[i].seen_us || now_us - peers[i].seen_us > PEER_TIMEOUT_US)) spare = &peers[i];
    }
    if (spare) memset(spare, 0, sizeof *spare);
    return spare;
}

/* signed distance between two bar positions, in (-8, 8] steps */
static double bar_delta(double d)
{
    d = fmod(d, BAR_STEPS);
    if (d > BAR_STEPS / 2) d -= BAR_STEPS;
    if (d <= -BAR_STEPS / 2) d += BAR_STEPS;
    return d;
}

static double median_error(void)
{
    double a[ERR_WINDOW];
    int n = n_errs < ERR_WINDOW ? n_errs : ERR_WINDOW;
    memcpy(a, errs, sizeof(double) * n);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && a[j - 1] > a[j]; j--) { double t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    return n ? a[n / 2] : 0.0;
}

/* lock to the leader: tempo and transport first, then bar phase. Both sides are compared at the moment the packet
 * arrived: the leader's position carried forward by the packet's trip, ours as heard then. */
static void follow(const Peer *p, uint64_t now_us)
{
    double spu = p->playing ? p->bpm / 15e6 : 0.0; /* its 16ths per us */
    double sent = p->pos + trip_us(p, now_us) * spu, lead = sent - SYNC_WIRE_LEAD_US * spu;
    double quick = quick_trip_us(p, now_us);
    status.delay_ms = quick < 0.0 ? -1.0f : (float)(quick / 1000.0);
    if (fabsf(p->bpm - clk->bpm()) > 0.01f && p->bpm >= 40.0f && p->bpm <= 250.0f) clk->set_bpm(p->bpm);
    if (p->playing != clk->playing()) {
        clk->set_playing(p->playing);
        if (p->playing) clk->set_position(sent); /* the clock's own position runs about as far ahead */
        reset_errors();
    }
    double rate = 1.0;
    if (clk->playing()) {
        errs[err_head] = bar_delta(lead - heard_position(now_us));
        err_head = (err_head + 1) % ERR_WINDOW;
        if (n_errs < ERR_WINDOW) n_errs++;
        double med = median_error();
        if (n_errs >= 3 && fabs(med) > JUMP_STEPS) {
            clk->set_position(clk->position() + med);
            reset_errors();
            med = 0;
        } else if (n_errs >= 3) {
            double k = med * TRIM_GAIN;
            rate = 1.0 + (k > TRIM_MAX ? TRIM_MAX : k < -TRIM_MAX ? -TRIM_MAX : k);
        }
        status.offset_ms = (float)(med * 15000.0 / clk->bpm()); /* one 16th = 15000 / bpm ms */
    }
    clk->set_rate(rate);
}

void sync_handle_packet(const uint8_t *b, int n, uint64_t now_us)
{
    if (n < SYNC_PKT_MIN || memcmp(b, "HI5S", 4) || b[4] != 1) return;
    uint32_t id = rd32(b + 8);
    if (id == status.id || clk->mode() == SYNC_OFF) return;
    Peer *p = find_peer(id, now_us);
    if (!p) return;
    p->id = id;
    p->seen_us = now_us;
    p->playing = b[5];
    p->mode = b[6];
    memcpy(&p->bpm, b + 12, 4);
    memcpy(&p->pos, b + 16, 8);
    if (p->bpm != p->bpm || p->pos != p->pos) { p->seen_us = 0; return; } /* NaN: ignore */
    p->sent_us = rd64(b + 24);
    p->heard_us = now_us;
    lags_add(&p->down, (int64_t)(now_us - p->sent_us), now_us);
    if (n >= SYNC_PKT_LEN && (b[7] & 1) && rd32(b + 32) == status.id) {
        /* it echoes one of ours: when it heard it (its clock: this send, less the time it held it) minus when we
         * sent it (ours) */
        lags_add(&p->up, (int64_t)(p->sent_us - rd32(b + 44) - rd64(b + 36)), now_us);
    }
    sync_elect(now_us);
    if (status.following && status.leader_id == id) follow(p, now_us);
}

void sync_elect(uint64_t now_us)
{
    int mode = clk->mode();
    int best_rank = mode_rank(mode), alive = 0;
    uint32_t best_id = best_rank < 3 ? status.id : 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        const Peer *p = &peers[i];
        if (!p->seen_us || now_us - p->seen_us > PEER_TIMEOUT_US) continue;
        alive++;
        int r = mode_rank(p->mode);
        if (r < best_rank || (r == best_rank && r < 3 && p->id < best_id)) { best_rank = r; best_id = p->id; }
    }
    int on = mode != SYNC_OFF && best_rank < 3;
    int leading = on && best_id == status.id;
    int following = on && best_id != status.id;
    if (!following && status.following) { /* lost the leader: run free again */
        clk->set_rate(1.0);
        if (!clk->playing()) clk->set_playing(1);
        reset_errors();
        status.offset_ms = 0;
        status.delay_ms = -1.0f;
    }
    if (following && (!status.following || status.leader_id != best_id)) reset_errors();
    status.peers = alive;
    status.leading = leading;
    status.following = following;
    status.leader_id = following ? best_id : 0;
}

const SyncStatus *sync_status(void) { return &status; }

/* ------------------------------------------------------------ sockets */

static void refresh_targets(void)
{
    struct ifaddrs *list, *it;
    n_targets = 0;
    if (getifaddrs(&list) != 0) return;
    for (it = list; it && n_targets < 8; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_BROADCAST) || (it->ifa_flags & IFF_LOOPBACK) || !it->ifa_broadaddr) continue;
        targets[n_targets] = *(struct sockaddr_in *)it->ifa_broadaddr;
        targets[n_targets].sin_port = htons(SYNC_PORT);
        n_targets++;
    }
    freeifaddrs(list);
}

static uint32_t random_id(void)
{
    uint32_t id = (uint32_t)time(NULL) ^ ((uint32_t)getpid() * 2654435761u);
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        uint32_t r;
        if (fread(&r, 1, 4, f) == 4) id ^= r;
        fclose(f);
    }
    return id ? id : 1;
}

void sync_init(const SyncClock *clock)
{
    sync_core_init(clock, random_id());
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(SYNC_PORT);
    if (bind(sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "sync: cannot bind UDP %d: %s\n", SYNC_PORT, strerror(errno));
        close(sock);
        sock = -1;
        return;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    /* the kernel notes when each packet arrived, so the wait for the next frame to read it doesn't count */
    setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one);
    /* ask Wi-Fi for its voice queue, which goes first */
    int tos = 0xc0, prio = 6;
    setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof prio);
    refresh_targets();
}

static uint64_t clock_us(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* read one packet; *heard = when it arrived, on the steady clock (the kernel's stamp is wall-clock time, so its
 * age is carried over) */
static ssize_t recv_stamped(uint8_t *b, size_t len, uint64_t mono, uint64_t real, uint64_t *heard)
{
    union {
        char buf[CMSG_SPACE(sizeof(struct timespec)) + 64];
        struct cmsghdr align;
    } ctl;
    struct iovec iov = {b, len};
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.buf;
    m.msg_controllen = sizeof ctl.buf;
    ssize_t n = recvmsg(sock, &m, 0);
    *heard = mono;
    if (n <= 0) return n;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
            struct timespec ts;
            memcpy(&ts, CMSG_DATA(c), sizeof ts);
            uint64_t at = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
            if (at <= real && real - at < 1000000ULL) *heard = mono - (real - at);
        }
    return n;
}

void sync_poll(uint64_t now_us)
{
    if (sock < 0 || !clk) return;
    uint8_t b[64];
    ssize_t n;
    uint64_t mono = clock_us(CLOCK_MONOTONIC), real = clock_us(CLOCK_REALTIME), heard;
    while ((n = recv_stamped(b, sizeof b, mono, real, &heard)) > 0) sync_handle_packet(b, (int)n, heard);
    sync_elect(now_us);
    if (clk->mode() == SYNC_OFF) return;
    if (now_us - last_iface_us > IFACE_REFRESH_US) { last_iface_us = now_us; refresh_targets(); }
    if (now_us - last_send_us < SEND_INTERVAL_US) return;
    last_send_us = now_us;
    int len = sync_make_packet(b, clock_us(CLOCK_MONOTONIC));
    for (int i = 0; i < n_targets; i++) sendto(sock, b, len, 0, (struct sockaddr *)&targets[i], sizeof targets[i]);
    if (!n_targets) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        a.sin_port = htons(SYNC_PORT);
        sendto(sock, b, len, 0, (struct sockaddr *)&a, sizeof a);
    }
}

void sync_close(void)
{
    if (sock >= 0) close(sock);
    sock = -1;
}
