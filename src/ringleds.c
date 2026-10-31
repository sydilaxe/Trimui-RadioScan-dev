/* The Brick Pro's LEDs, one by one: see ringleds.h. */
#include "ringleds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NFRAME 23
#define F_BAR 0
#define F_F2 8
#define F_F1 9
#define F_RING_L 10
#define F_RING_R 14
#define F_SHOULDER 18
#define SAVED_FILE "ringleds_saved.txt"
#define COL_BAR 0xff6414 /* the bar's first beat */
#define COL_BEAT 0xb4b4b4

static char dir[256] = "/sys/class/led_anim";
static int on, bar_on = 1;
static uint32_t shown[NFRAME];
static int shown_any;
static volatile uint32_t peak[2];

/* what was there at start, put back at the end: each named zone's colour
 * and effect, each brightness, effect_enable ("-" where there was none) */
static const char *const zone[] = {"f1", "f2", "m", "lr", "l", "r"};
#define NZONE 6
static const char *const scale_name[] = {"max_scale", "max_scale_lr", "max_scale_f1f2"};
#define NSCALE 3
static char saved_col[NZONE][16], saved_eff[NZONE][8], saved_scale[NSCALE][8], saved_enable[8];

static int put(const char *name, const char *val)
{
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f)
        return 0;
    int ok = fputs(val, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void get(const char *name, char *out, int n)
{
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (f) {
        if (!fgets(out, n, f))
            out[0] = 0;
        fclose(f);
    }
    out[strcspn(out, " \r\n")] = 0;
    if (!out[0])
        snprintf(out, (size_t)n, "-");
}

static void env_name(char *b, int n, const char *prefix, const char *what) { snprintf(b, n, "%s_%s", prefix, what); }

int ringleds_open(const char *prefix, int brickpro)
{
    char name[64];
    env_name(name, sizeof name, prefix, "LEDS");
    const char *e = getenv(name);
    if (e && *e == '0')
        return 0;
    env_name(name, sizeof name, prefix, "LED_FRAME");
    e = getenv(name);
    if (e && *e)
        brickpro = *e != '0';
    if (!brickpro)
        return 0;
    env_name(name, sizeof name, prefix, "LED_DIR");
    e = getenv(name);
    snprintf(dir, sizeof dir, "%s", e && *e ? e : "/sys/class/led_anim");
    char p[320];
    snprintf(p, sizeof p, "%s/frame_hex", dir);
    if (access(p, W_OK) != 0)
        return 0; /* no such driver here, or not ours to write */
    env_name(name, sizeof name, prefix, "LED_BAR");
    e = getenv(name);
    bar_on = !(e && *e == '0');

    /* what to put back: from the file a run that didn't get to put it back left, else as found */
    FILE *sf = fopen(SAVED_FILE, "r");
    int n = 0;
    if (sf) {
        for (int z = 0; z < NZONE; z++)
            n += fscanf(sf, "%15s %7s", saved_col[z], saved_eff[z]);
        for (int i = 0; i < NSCALE; i++)
            n += fscanf(sf, "%7s", saved_scale[i]);
        n += fscanf(sf, "%7s", saved_enable);
        fclose(sf);
    }
    if (n != 2 * NZONE + NSCALE + 1) {
        for (int z = 0; z < NZONE; z++) {
            snprintf(name, sizeof name, "effect_rgb_hex_%s", zone[z]);
            get(name, saved_col[z], sizeof saved_col[z]);
            snprintf(name, sizeof name, "effect_%s", zone[z]);
            get(name, saved_eff[z], sizeof saved_eff[z]);
        }
        for (int i = 0; i < NSCALE; i++)
            get(scale_name[i], saved_scale[i], sizeof saved_scale[i]);
        get("effect_enable", saved_enable, sizeof saved_enable);
        if ((sf = fopen(SAVED_FILE, "w"))) {
            for (int z = 0; z < NZONE; z++)
                fprintf(sf, "%s %s\n", saved_col[z], saved_eff[z]);
            for (int i = 0; i < NSCALE; i++)
                fprintf(sf, "%s\n", saved_scale[i]);
            fprintf(sf, "%s\n", saved_enable);
            fclose(sf);
        }
    }
    /* bright enough to see (MustardOS starts the lights at 0), the named effects off: the frame shows */
    env_name(name, sizeof name, prefix, "LED_BRIGHTNESS");
    e = getenv(name);
    int b = e && *e ? atoi(e) : 60;
    b = b < 1 ? 1 : b > 60 ? 60 : b;
    char v[16];
    snprintf(v, sizeof v, "%d\n", b);
    for (int i = 0; i < NSCALE; i++)
        if (strcmp(saved_scale[i], "-"))
            put(scale_name[i], v);
    put("effect_enable", "0\n");
    shown_any = 0;
    peak[0] = peak[1] = 0;
    on = 1;
    return 1;
}

void ringleds_audio(const int16_t *s, int frames)
{
    if (!on)
        return;
    int m[2] = {0, 0};
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            int a = s[i * 2 + c] < 0 ? -s[i * 2 + c] : s[i * 2 + c];
            if (a > m[c])
                m[c] = a;
        }
    for (int c = 0; c < 2; c++) {
        uint32_t want = (uint32_t)m[c], was = __atomic_load_n(&peak[c], __ATOMIC_RELAXED);
        while (want > was && !__atomic_compare_exchange_n(&peak[c], &was, want, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
    }
}

/* a colour at an intensity 0..1, on a curve so that halfway looks about half as bright, in eighths */
static uint32_t glow(uint32_t c, float t)
{
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    float k = powf(floorf(t * 8.f + 0.5f) / 8.f, 1.8f);
    uint32_t r = (uint32_t)lrintf(((c >> 16) & 255) * k), g = (uint32_t)lrintf(((c >> 8) & 255) * k),
             b = (uint32_t)lrintf((c & 255) * k);
    return r << 16 | g << 8 | b;
}

uint32_t ringleds_glow(uint32_t c, float t) { return glow(c, t); }
/* L1, L2, R2, R1: the frame's order */
static uint32_t shoulder[4] = {RINGLEDS_BEAT, RINGLEDS_BEAT, RINGLEDS_BEAT, RINGLEDS_BEAT};
void ringleds_shoulders(uint32_t l1, uint32_t l2, uint32_t r1, uint32_t r2)
{
    shoulder[0] = l1, shoulder[1] = l2, shoulder[2] = r2, shoulder[3] = r1;
}
static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void ringleds_update(float lx, float ly, uint32_t lcol, float rx, float ry, uint32_t rcol, int playing, int step,
                     float bpm, uint32_t f1, uint32_t f2)
{
    if (!on)
        return;
    uint32_t f[NFRAME] = {0};
    f[F_F1] = f1, f[F_F2] = f2;
    /* the top bar: each channel's level over -42 .. 0 dB, from the centre out; it falls back over about a
     * third of a second (called about every 33 ms) */
    static float lev[2];
    static const uint32_t meter_col[4] = {0x20d040, 0x20d040, 0xffb020, 0xff2010};
    for (int c = 0; c < 2; c++) {
        float p = __atomic_exchange_n(&peak[c], 0, __ATOMIC_RELAXED) / 32000.f;
        lev[c] = p > lev[c] ? p : lev[c] * 0.82f;
        float db = lev[c] > 1e-6f ? 20.f * log10f(lev[c]) : -120.f, lit = (db + 42.f) / 42.f * 4.f;
        for (int k = 0; k < 4; k++)
            f[F_BAR + (c ? 4 + k : 3 - k)] = bar_on ? glow(meter_col[k], lit - k) : 0;
    }
    /* the rings: the corners toward where each stick points */
    static const float cx[4] = {-1.f, -1.f, 1.f, 1.f}, cy[4] = {1.f, -1.f, -1.f, 1.f};
    for (int k = 0; k < 4; k++) {
        f[F_RING_L + k] = glow(lcol, lx * cx[k] + ly * cy[k]);
        f[F_RING_R + k] = glow(rcol, rx * cx[k] + ry * cy[k]);
    }
    /* the beat: each quarter note (four 16ths) starts a flash that fades over it */
    static int last_step = -1;
    static uint64_t beat_at;
    static int first;
    uint64_t now = ms_now();
    if (playing && step != last_step && step % 4 == 0)
        beat_at = now, first = step % 16 == 0;
    last_step = step;
    float beat_ms = 60000.f / (bpm > 1.f ? bpm : 120.f);
    float t = playing && beat_at ? (float)(now - beat_at) / beat_ms : 1.f;
    uint32_t beat = t < 1.f ? glow(first ? COL_BAR : COL_BEAT, (1.f - t) * (1.f - t)) : 0;
    for (int k = 0; k < 4; k++)
        f[F_SHOULDER + k] = shoulder[k] == RINGLEDS_BEAT ? beat : shoulder[k];
    if (shown_any && !memcmp(f, shown, sizeof f))
        return;
    char v[NFRAME * 7 + 2];
    for (int i = 0; i < NFRAME; i++)
        snprintf(v + i * 7, 8, "%06X ", (unsigned)(f[i] & 0xffffff));
    strcat(v, "\n");
    put("frame_hex", v);
    memcpy(shown, f, sizeof f);
    shown_any = 1;
}

void ringleds_close(void)
{
    if (!on)
        return;
    char v[NFRAME * 7 + 2], name[48];
    for (int i = 0; i < NFRAME; i++)
        memcpy(v + i * 7, "000000 ", 7);
    v[NFRAME * 7] = '\n', v[NFRAME * 7 + 1] = 0;
    put("frame_hex", v);
    /* back as they were: colour, then effect (writing it starts it), brightness, effect_enable */
    for (int z = 0; z < NZONE; z++) {
        char val[24];
        if (strcmp(saved_col[z], "-")) {
            snprintf(name, sizeof name, "effect_rgb_hex_%s", zone[z]);
            snprintf(val, sizeof val, "%s \n", saved_col[z]);
            put(name, val);
        }
        if (strcmp(saved_eff[z], "-")) {
            snprintf(name, sizeof name, "effect_%s", zone[z]);
            snprintf(val, sizeof val, "%s\n", saved_eff[z]);
            put(name, val);
        }
    }
    for (int i = 0; i < NSCALE; i++)
        if (strcmp(saved_scale[i], "-")) {
            char val[32];
            snprintf(val, sizeof val, "%s\n", saved_scale[i]);
            put(scale_name[i], val);
        }
    if (strcmp(saved_enable, "-")) {
        char val[32];
        snprintf(val, sizeof val, "%s\n", saved_enable);
        put("effect_enable", val);
    }
    remove(SAVED_FILE);
    on = 0;
}
