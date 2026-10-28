/* Wi-Fi tempo sync between units (Miyoo or TrimUI) on the same network.
 *
 * Every unit broadcasts its tempo, bar position and transport over UDP ten times a second.
 * One unit leads (a unit set to LEAD beats one on AUTO; otherwise the lowest id wins; FOLLOW
 * never leads). Followers adopt the leader's tempo and transport and keep their bar phase
 * locked by trimming clock speed by a few percent, jumping only when far off.
 *
 * Each packet carries the time it was sent, so a follower can tell how long it took to arrive and carry the
 * leader's position forward by that much: Wi-Fi delays and the wait for the next screen frame to read it don't
 * count as being out of step. Echoes of each other's packets measure the quickest trip each way.
 *
 * Packet (48 bytes, little-endian; older units send and read only the first 32, and sync with these all the same):
 *   0  "HI5S"         4  version (1)       5  playing (0/1)     6  sync mode     7  flags (1: echo below)
 *   8  u32 unit id    12 f32 bpm           16 f64 position in 16th notes at the send time: the one being heard,
 *                                             plus SYNC_WIRE_LEAD_US (older units send the one being made, about
 *                                             that far ahead)
 *   24 u64 sender clock (us)
 *   32 u32 echo: id of a unit heard        36 u64 the clock in its packet   44 u32 us from hearing it to this send
 */
#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

#define SYNC_PORT 5150
#define SYNC_PKT_MIN 32           /* the oldest packet, still understood */
#define SYNC_PKT_LEN 48           /* what this version sends */
#define SYNC_WIRE_LEAD_US 32000.0 /* 1.5 buffers of 1024 at 48 kHz */

enum { SYNC_OFF, SYNC_AUTO, SYNC_LEAD, SYNC_FOLLOW };

/* How sync talks to the clock it controls. Called from the main thread. */
typedef struct {
    int (*mode)(void);                  /* SYNC_* */
    float (*bpm)(void);
    void (*set_bpm)(float bpm);
    double (*position)(void);           /* 16th notes since the downbeat */
    void (*set_position)(double steps);
    void (*set_rate)(double rate);      /* clock speed trim, 1.0 = nominal */
    int (*playing)(void);
    void (*set_playing)(int on);
} SyncClock;

typedef struct {
    uint32_t id;
    int peers;        /* other units heard in the last 3 s */
    int leading;      /* this unit is the leader */
    int following;    /* locked to another unit */
    uint32_t leader_id;
    float offset_ms;  /* follower: how far behind the leader (+ = behind) */
    float delay_ms;   /* follower: the leader's packets take this long at best (-1: not measured) */
} SyncStatus;

void sync_init(const SyncClock *clock);  /* picks an id, opens the socket (no-op network if it fails) */
void sync_poll(uint64_t now_us);         /* main thread, every frame */
void sync_close(void);
const SyncStatus *sync_status(void);

/* protocol core without sockets, for tests; now_us is when the packet was sent, or heard */
void sync_core_init(const SyncClock *clock, uint32_t id);
int sync_make_packet(uint8_t *buf, uint64_t now_us);
void sync_handle_packet(const uint8_t *buf, int n, uint64_t now_us);
void sync_elect(uint64_t now_us);

#endif
