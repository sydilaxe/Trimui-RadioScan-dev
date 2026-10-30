/* RadioScan — a synthesizer for the TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS or MustardOS) whose sound sources are
 * internet radio streams. Ported from the Miyoo Mini Plus version: the same 640x480 pages, drawn into a buffer and
 * scaled to fill the screen through SDL2 (1.6x on the Brick's 1024x768).
 *
 *  PLAY  (home)  three circles: OSC A, the five Euclidean rings, OSC B. Buttons are a scale keyboard.
 *  EDIT          five pages: RADIO, VOICES, SHAPE, MOTION, SPACE.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "aclock.h"
#include "dsp.h"
#include "font.h"
#include "net.h"
#include "perflog.h"
#include "stations.h"
#include "stream.h"
#include "sync.h"
#include "update.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SCREEN_W 640
#define SCREEN_H 480

/* ================================================================== buttons */
/* The TrimUI's buttons come in as a joystick (see the input section near main); a keyboard works too, for trying it
 * on a computer, with the Miyoo's key layout. */
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_X, B_Y, B_L1, B_R1, B_L2, B_R2, B_SELECT, B_START, B_MENU, B_F1, B_F2, B_COUNT,
       B_NONE = -1 };

static int map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return B_UP;
    case SDLK_DOWN: return B_DOWN;
    case SDLK_LEFT: return B_LEFT;
    case SDLK_RIGHT: return B_RIGHT;
    case SDLK_SPACE: return B_A;
    case SDLK_LCTRL: return B_B;
    case SDLK_LSHIFT: return B_X;
    case SDLK_LALT: return B_Y;
    case SDLK_e: return B_L1;
    case SDLK_t: return B_R1;
    case SDLK_TAB: return B_L2;
    case SDLK_BACKSPACE: return B_R2;
    case SDLK_RCTRL: return B_SELECT;
    case SDLK_RETURN: return B_START;
    case SDLK_ESCAPE: return B_MENU;
    case SDLK_F1: return B_F1;
    case SDLK_F2: return B_F2;
    default: return B_NONE;
    }
}

/* ================================================================== state */

static Synth *syn;
static Stream *st[2];
/* Radio scan: the station a voice is crossfading into, on its own stream. xf_state: 0 idle, 1 connecting (main
 * thread), 2 fading (audio thread), 3 faded in (audio thread done; the main thread swaps the streams over). */
static Stream *st_next[2];
static volatile int xf_state[2];
static volatile float xf_pos[2], xf_step[2];
/* RAW: the two streams just as they arrive, through one crossfader, with nothing else in the way: no engine, no
 * effects, no leveller, limiter or dither. The engine keeps running underneath, unheard, so the clock, Wi-Fi sync and
 * everything as it was carry on for when RAW is left. raw_xf: 0 all A .. 1 all B, set by the main thread;
 * raw_peak: each stream's loudest sample since the screen last looked. */
static volatile int raw_on;
static volatile float raw_xf = 0.5f;
static volatile float raw_hand; /* the Brick Pro's right stick across: -1..1 over the top of raw_xf, 0 lets go */

/* x pulled toward one end of lo..hi by h (-1..1, all the way at the ends), as the right stick plays things by hand */
static float toward(float x, float h, float lo, float hi) { return x + h * (h > 0 ? hi - x : x - lo); }
static volatile float raw_peak[2];
static const float raw_fade_s[] = {0, 0.5f, 1, 2, 4, 8, 16}; /* how long L1 / R1 / A take to fade all the way across */
#define RAW_FADES ((int)(sizeof raw_fade_s / sizeof raw_fade_s[0]))
static int audio_rate = 48000;
static StationList stations;
static char app_dir[PATH_MAX], stations_path[PATH_MAX + 16], settings_path[PATH_MAX + 16];

enum { SCR_PLAY, SCR_EDIT, SCR_RAW };
enum { OV_NONE, OV_KEYBOARD, OV_SCANMENU, OV_SCANRESULTS, OV_UPDATE };
enum { KB_ADD_URL, KB_ADD_NAME, KB_SEARCH };
enum { TAB_RADIO, TAB_VOICES, TAB_RHYTHM, TAB_ENGINE, TAB_MOD, TAB_MIX, TAB_COUNT };
static const char *const tab_names[TAB_COUNT] = {"RADIO", "VOICES", "RHYTHM", "ENGINE", "MOD", "MIX"};

#define ROW_HEADER (-3)

static struct {
    int screen, overlay;
    int tab;
    int sel[TAB_COUNT], scroll[TAB_COUNT];
    int radio_sel, radio_scroll;
    int confirm_delete;
    int octave;
    int held[B_COUNT];
    Uint32 held_since[B_COUNT], last_repeat[B_COUNT];
    int l2_used;              /* L2 acted as a modifier while held (otherwise a tap = grab) */
    int cell;                 /* PLAY: selected rhythm, SEQ_A, SEQ_B or SEQ_X (the cross rhythm in the middle) */
    char toast[128];
    Uint32 toast_until;
    Uint32 grab_flash, menu_armed;
    int grab_osc;             /* voice of the last grab: 0, 1 or -1 for both */
    Uint32 taps[8];
    int ntaps;
    int pending_station[2];
    Uint32 pending_at[2];
    int tuned[2];
    int kb_purpose, kb_row, kb_col, kb_shift;
    char kb_text[512], kb_url[512];
    int kb_cat;
    int scan_sel, scan_scroll, scan_menu_sel, scan_menu_scroll, scan_cat;
    StationList scan_results;
    volatile int scan_busy;
    char scan_title[96];
    int dirty_settings;
    int repeating;
    int running;
    int raw_fade;             /* RAW: index into raw_fade_s, how long L1 / R1 / A take to fade across */
    float raw_to;             /* RAW: where a fade is heading (0 A .. 1 B), or -1 when the fader is still */
    int update_asked, update_tries; /* the update prompt has been shown this session; checks made */
    Uint32 update_next_check, relaunch_at;
} ui;

/* ================================================================== audio */

/* The sound device. audio_lock() holds off the audio callback while the main thread changes what it reads; dev_mx
 * also keeps the device from being swapped (a USB sound card plugged in) while it is held. */
static SDL_AudioDeviceID adev;
static pthread_mutex_t dev_mx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static void audio_lock(void)
{
    pthread_mutex_lock(&dev_mx);
    if (adev) SDL_LockAudioDevice(adev);
}
static void audio_unlock(void)
{
    if (adev) SDL_UnlockAudioDevice(adev);
    pthread_mutex_unlock(&dev_mx);
}

static float bufA[4096], bufB[4096], bufO[4096], bufN[4096];

/* a voice's radio: its stream, or during a radio-scan switch an equal-power crossfade into the next station */
static void read_voice(int o, float *buf, int n)
{
    stream_read(st[o], buf, n);
    int x = xf_state[o];
    if (x < 2) return;
    stream_read(st_next[o], bufN, n);
    if (x == 3) { memcpy(buf, bufN, sizeof(float) * n * 2); return; }
    float g = xf_pos[o], step = xf_step[o];
    for (int i = 0; i < n; i++) {
        float a = cosf(g * (float)M_PI * 0.5f), b = sinf(g * (float)M_PI * 0.5f);
        buf[i * 2] = buf[i * 2] * a + bufN[i * 2] * b;
        buf[i * 2 + 1] = buf[i * 2 + 1] * a + bufN[i * 2 + 1] * b;
        g = g + step > 1 ? 1 : g + step;
    }
    xf_pos[o] = g;
    if (g >= 1) xf_state[o] = 3;
}

static float raw_w; /* audio thread: 0 the engine .. 1 RAW */

/* RAW: bufO becomes A * (1 - x) + B * x, the fader x eased over ~5 ms so a jump doesn't click. It lands exactly, so
 * a fader at either end passes that stream through untouched, and a linear fade never adds up past the louder
 * stream, so nothing can clip and nothing needs limiting. Going in and out of RAW crossfades with the engine's
 * output over 10 ms. Returns 1 when the whole buffer was RAW alone. */
static int raw_mix(int n)
{
    static float x = -1;
    float w = raw_w, target = toward(raw_xf, raw_hand, 0, 1), want = raw_on ? 1.0f : 0.0f, pk[2] = {raw_peak[0], raw_peak[1]};
    float k = 1.0f - expf(-1.0f / (0.005f * audio_rate)), step = 1.0f / (0.01f * audio_rate);
    if (x < 0) x = target;
    int pure = w >= 1 && want >= 1;
    for (int i = 0; i < n; i++) {
        x += (target - x) * k;
        if (fabsf(target - x) < 1e-4f) x = target;
        w = w < want ? fminf(w + step, want) : fmaxf(w - step, want);
        for (int c = 0; c < 2; c++) {
            float a = bufA[i * 2 + c], b = bufB[i * 2 + c];
            pk[0] = fmaxf(pk[0], fabsf(a));
            pk[1] = fmaxf(pk[1], fabsf(b));
            bufO[i * 2 + c] = bufO[i * 2 + c] * (1 - w) + (a * (1 - x) + b * x) * w;
        }
    }
    raw_w = w;
    raw_peak[0] = pk[0];
    raw_peak[1] = pk[1];
    return pure;
}

/* Treat denormal floats as zero in the audio thread: decaying reverb/filter tails otherwise
 * fall into denormals, which are slow on many CPUs. */
static void enable_flush_to_zero(void)
{
#if defined(__aarch64__)
    unsigned long fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ul << 24); /* FZ */
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
    unsigned fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24); /* FZ */
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(__x86_64__) || defined(__SSE__)
    unsigned csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8040; /* FTZ | DAZ */
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#endif
}

/* Audio timing, for the event log and the stall watch: a callback that comes much later than its buffer lasts means
 * the sound dropped out (a pop). Written by the audio thread, read by the main thread. The load, dropouts and
 * gaps reported every 10 s come from the shared performance log (perflog.c). */
static volatile unsigned aud_late, aud_calls;
static volatile int aud_rt = -1;          /* real-time priority: 1 granted, 0 refused or turned off */
static int aud_rt_wanted = 1;             /* RS_RT=0 turns it off */
static volatile unsigned aud_epoch;       /* bumps each time the sound device is reopened */
static volatile double aud_first_ms;      /* when steady audio started, to count missing buffers */
static volatile float aud_buf_ms;
static volatile float aud_recent_gap_ms;  /* longest gap since the main loop last looked (for the event log) */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static double mono_ms(void) { return now_us() / 1000.0; }

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    static __thread int ftz;
    if (!ftz) {
        enable_flush_to_zero();
        /* real-time priority, so a busy moment elsewhere (an SD card write, drawing) can't starve the sound;
         * TrimUI OS runs apps as root, and where it isn't allowed nothing changes */
        struct sched_param sp = {.sched_priority = 50};
        aud_rt = !aud_rt_wanted ? 0 : pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
        ftz = 1;
    }
    static double last_start;
    uint64_t perf_t0 = perf_audio_begin(len / 4); /* the performance log's load and dropout counters */
    double t0 = perf_t0 / 1000.0, buf_ms = len / 4 * 1000.0 / audio_rate;
    aclock_request((uint64_t)(t0 * 1000.0), len / 4);
    aud_buf_ms = (float)buf_ms;
    if (aud_calls == 20) aud_first_ms = t0;
    if (last_start > 0 && aud_calls > 20) { /* skip start-up */
        float gap = (float)(t0 - last_start);
        if (gap > buf_ms * 2.0) aud_late++; /* longer than two buffers: the driver's cushion is nearly gone */
        if (gap > aud_recent_gap_ms) aud_recent_gap_ms = gap;
    }
    last_start = t0;
    aud_calls++;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    while (frames > 0) {
        int n = frames > 2048 ? 2048 : frames;
        read_voice(0, bufA, n);
        read_voice(1, bufB, n);
        synth_process(syn, bufA, bufB, bufO, n);
        if ((raw_on || raw_w > 0) && raw_mix(n)) { /* RAW alone: straight to 16 bits, no dither */
            for (int i = 0; i < n * 2; i++) {
                float v = bufO[i] * 32768.0f; /* the streams' own scale, so 16-bit samples come back as they were */
                out[i] = (int16_t)lrintf(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
            }
            out += n * 2;
            frames -= n;
            continue;
        }
        static unsigned dith = 22222;
        /* mono + clock: the clock side is pulses, not sound, so it goes out exactly, without dither */
        int clk_ch = (int)synth_get_param(syn, P_OUTPUT) == OUT_MONO_CLOCK ? !(int)synth_get_param(syn, P_CLK_SIDE) : -1;
        for (int i = 0; i < n * 2; i++) { /* to 16 bits with TPDF dither: quiet tails fade into noise, not grit */
            dith = dith * 1664525u + 1013904223u;
            float d = (i & 1) == clk_ch ? 0.0f : (float)(dith >> 16 & 0xff) - (float)(dith >> 24); /* -255 .. 255, triangular */
            float v = bufO[i] * 32767.0f + d * (1.0f / 256.0f);
            v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
            out[i] = (int16_t)lrintf(v);
        }
        out += n * 2;
        frames -= n;
    }
    aclock_made(len / 4, synth_position(syn), synth_steps_per_frame(syn)); /* where the music is, for Wi-Fi sync */
    perf_audio_output((const int16_t *)stream, len / 4); /* JUMP: a click made here */
    perf_audio_end(perf_t0);
}

/* ------------------------------------------------------------------ event log
 * What the player does (button presses with the screen they were on, settings as they settle, stations starting
 * and failing) and every late audio buffer, each with a time stamp, so a dropout can be matched to the action that
 * caused it. Kept in memory and written to log.txt in one go every 10 s and on quit: no SD card write per press. */
static char ev_buf[32 * 1024];
static int ev_len;
static uint64_t ev_t0; /* perf_now_us() at start: the same clock as the performance log's lines */

static void ev_flush(void)
{
    if (!ev_len) return;
    fwrite(ev_buf, 1, (size_t)ev_len, stderr);
    fflush(stderr);
    ev_len = 0;
}

static void ev(const char *fmt, ...)
{
    if (ev_len > (int)sizeof ev_buf - 256) ev_flush(); /* full: rare, only during a long burst */
    double t = (perf_now_us() - ev_t0) / 1e6;
    ev_len += snprintf(ev_buf + ev_len, sizeof ev_buf - ev_len, "%9.3f ", t);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ev_buf + ev_len, sizeof ev_buf - ev_len, fmt, ap);
    va_end(ap);
    if (n > 0) ev_len += n < (int)sizeof ev_buf - ev_len ? n : (int)sizeof ev_buf - ev_len - 1;
    if (ev_len < (int)sizeof ev_buf - 1) ev_buf[ev_len++] = '\n';
}

/* Every 10 s (every second with RS_LOG=2), the shared performance log's STAT line about the time since the last:
 * the audio callback's load against the time a buffer lasts, dropouts (the sound card went without), the longest
 * wait between buffers, the screen's frame rate and time, and the processor's clock, temperature and cores; HINT
 * lines when something should change. The event log is written out first, so the lines stay in time order. */
static void audio_report(Uint32 now)
{
    static int said_rt = -1;
    if (aud_rt >= 0 && aud_rt != said_rt) {
        said_rt = aud_rt;
        perf_audio_rt(aud_rt);
        if (!aud_rt) perf_log("AUDIO", "real-time priority %s", aud_rt_wanted ? "refused" : "off (RS_RT=0)");
    }
    perf_tick(); /* writes the event log out first (perf_before_stat) */
    static Uint32 last_flush;
    if (perf_level() == PERF_OFF && now - last_flush >= 10000) { /* no STAT lines: the events on their own */
        last_flush = now;
        ev_flush();
    }
}

/* ================================================================== drawing */

static SDL_Surface *screen; /* the 640x480 page everything is drawn into */
static Uint32 *px;
static int pitch_px;

#define RGB(r, g, b) ((Uint32)(((r) << 16) | ((g) << 8) | (b)))
/* forest palette */
static const Uint32 C_BG = RGB(15, 23, 18),        /* forest floor at dusk */
    C_PANEL = RGB(26, 38, 30),                     /* moss shade */
    C_PANEL2 = RGB(42, 58, 45),                    /* lichen */
    C_SEL = RGB(50, 70, 48),                       /* sunlit moss */
    C_TEXT = RGB(233, 228, 212),                   /* birch bark */
    C_DIM = RGB(138, 152, 126),                    /* sage */
    C_LEAF = RGB(143, 191, 106),                   /* OSC A: new leaf */
    C_AMBER = RGB(217, 164, 65),                   /* OSC B: autumn */
    C_RUST = RGB(196, 98, 62),                     /* berries / warnings */
    C_WATER = RGB(112, 166, 166),                  /* voices: river */
    C_BARK = RGB(140, 106, 76);                    /* sound: bark */
static const Uint32 ring_cols[5] = {RGB(118, 160, 92), RGB(170, 196, 118), RGB(217, 164, 65),
                                            RGB(196, 120, 76), RGB(112, 166, 166)};

static Uint32 map_col(Uint32 c) { return SDL_MapRGB(screen->format, (c >> 16) & 255, (c >> 8) & 255, c & 255); }

static void fill(int x, int y, int w, int h, Uint32 c)
{
    if (w <= 0 || h <= 0) return;
    SDL_Rect r = {(Sint16)x, (Sint16)y, (Uint16)w, (Uint16)h};
    SDL_FillRect(screen, &r, map_col(c));
}

static inline void pset(int x, int y, Uint32 m)
{
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H) px[y * pitch_px + x] = m;
}

static Uint32 mix_col(Uint32 a, Uint32 b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return RGB(r, g, bl);
}

static void line(int x0, int y0, int x1, int y1, Uint32 c)
{
    Uint32 m = map_col(c);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        pset(x0, y0, m);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void disc(int cx, int cy, int r, Uint32 c)
{
    for (int dy = -r; dy <= r; dy++) {
        int w = (int)sqrtf((float)(r * r - dy * dy) + 0.5f);
        fill(cx - w, cy + dy, 2 * w + 1, 1, c);
    }
}

static void circle(int cx, int cy, int r, Uint32 c)
{
    Uint32 m = map_col(c);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pset(cx + x, cy + y, m); pset(cx + y, cy + x, m); pset(cx - y, cy + x, m); pset(cx - x, cy + y, m);
        pset(cx - x, cy - y, m); pset(cx - y, cy - x, m); pset(cx + y, cy - x, m); pset(cx + x, cy - y, m);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* arc from fraction a0 to a1 of a full turn, starting at 12 o'clock, clockwise */
static void arc(int cx, int cy, int r, int thick, float a0, float a1, Uint32 c)
{
    Uint32 m = map_col(c);
    if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
    float step = 0.5f / (r * 2.0f * (float)M_PI);
    for (float a = a0; a <= a1; a += step) {
        float s = sinf(a * 2 * (float)M_PI), co = -cosf(a * 2 * (float)M_PI);
        for (int k = 0; k < thick; k++) pset(cx + (int)lroundf(s * (r + k)), cy + (int)lroundf(co * (r + k)), m);
    }
}

static void polar(int cx, int cy, float r, float frac, int *x, int *y)
{
    *x = cx + (int)lroundf(sinf(frac * 2 * (float)M_PI) * r);
    *y = cy - (int)lroundf(cosf(frac * 2 * (float)M_PI) * r);
}

static int text(int x, int y, const char *s, Uint32 col, int max_chars)
{
    Uint32 m = map_col(col);
    int n = 0, len = (int)strlen(s);
    int ellipsis = max_chars > 0 && len > max_chars;
    for (; *s; s++, n++) {
        if (max_chars > 0 && n >= max_chars) break;
        unsigned char ch = (unsigned char)*s;
        if (ellipsis && n == max_chars - 1) ch = '~';
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const uint16_t *g = font_bits[ch - FONT_FIRST];
        for (int r = 0; r < FONT_H; r++) {
            uint16_t bits = g[r];
            if (!bits) continue;
            for (int c = 0; c < FONT_W; c++)
                if (bits & (1 << (15 - c))) pset(x + n * FONT_W + c, y + r, m);
        }
    }
    return n * FONT_W;
}

static void text_right(int xr, int y, const char *s, Uint32 col, int max_chars)
{
    int len = (int)strlen(s);
    if (max_chars > 0 && len > max_chars) len = max_chars;
    text(xr - len * FONT_W, y, s, col, max_chars);
}

static void text_center(int xc, int y, const char *s, Uint32 col, int max_chars)
{
    int len = (int)strlen(s);
    if (max_chars > 0 && len > max_chars) len = max_chars;
    text(xc - len * FONT_W / 2, y, s, col, max_chars);
}

/* round knob: track ring + value arc (bipolar params fill from 12 o'clock) */
static void knob(int cx, int cy, int r, float frac, int bipolar, Uint32 col)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    arc(cx, cy, r, 2, 0.125f, 0.875f, C_PANEL2);
    if (bipolar) arc(cx, cy, r, 2, 0.5f, 0.125f + frac * 0.75f, col);
    else if (frac > 0.002f) arc(cx, cy, r, 2, 0.125f, 0.125f + frac * 0.75f, col);
    int x, y;
    polar(cx, cy, r - 3, 0.625f + frac * 0.75f + 0.5f, &x, &y);
    line(cx, cy, x, y, col);
}

static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui.toast, sizeof ui.toast, fmt, ap);
    va_end(ap);
    ui.toast_until = SDL_GetTicks() + 2200;
    ev("  says: %s", ui.toast);
}

/* ================================================================== settings & stations */

/* Settings from an older version only keep their stations: the sound defaults changed. */
#define SETTINGS_VERSION 3
static int settings_were_reset;

/* The settings text is built on the main thread and written by a short-lived background thread, so a slow SD card
 * never holds up the screen, the buttons or (through them) the audio. */
static char save_buf[64 * 1024];
static volatile int save_busy;

static void *save_thread(void *arg)
{
    (void)arg;
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s.tmp", settings_path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(save_buf, f);
        fclose(f);
        rename(tmp, settings_path);
    }
    save_busy = 0;
    return NULL;
}

/* background: write from a short-lived thread; otherwise write now (on quit) */
static void write_settings(int background)
{
    if (save_busy) return; /* the previous save is still writing: try again later */
    int n = 0, cap = (int)sizeof save_buf;
    n += snprintf(save_buf + n, cap - n, "version=%d\n", SETTINGS_VERSION);
    for (int i = 0; i < P_COUNT && n < cap; i++) n += snprintf(save_buf + n, cap - n, "%s=%g\n", param_info[i].key, synth_get_param(syn, i));
    for (int o = 0; o < 2 && n < cap; o++) {
        int idx = (int)synth_get_param(syn, P_A_STATION + o * OSC_STRIDE);
        if (idx >= 0 && idx < stations.count) n += snprintf(save_buf + n, cap - n, "%c_url=%s\n", 'a' + o, stations.items[idx].url);
    }
    if (n < cap) n += snprintf(save_buf + n, cap - n, "octave=%d\n", ui.octave);
    if (n < cap) snprintf(save_buf + n, cap - n, "raw=%d\nraw_xf=%g\nraw_fade=%d\n", raw_on, raw_xf, ui.raw_fade);
    ui.dirty_settings = 0;
    save_busy = 1;
    pthread_t t;
    if (background && pthread_create(&t, NULL, save_thread, NULL) == 0) pthread_detach(t);
    else save_thread(NULL);
}

static void save_settings(void) /* now, on this thread (on quit): after any save still writing */
{
    while (save_busy) SDL_Delay(5);
    write_settings(0);
}


static int load_settings(void)
{
    FILE *f = fopen(settings_path, "r");
    if (!f) return -1;
    char line_[1024];
    int version = 0;
    while (fgets(line_, sizeof line_, f))
        if (!strncmp(line_, "version=", 8)) version = atoi(line_ + 8);
    rewind(f);
    settings_were_reset = version < SETTINGS_VERSION;
    while (fgets(line_, sizeof line_, f)) {
        char *eq = strchr(line_, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line_, "octave")) { ui.octave = atoi(v); continue; }
        if (!strcmp(line_, "raw")) { raw_on = atoi(v) != 0; continue; }
        if (!strcmp(line_, "raw_xf")) { float x = (float)atof(v); raw_xf = x < 0 ? 0 : x > 1 ? 1 : x; continue; }
        if (!strcmp(line_, "raw_fade")) { ui.raw_fade = atoi(v); continue; }
        if ((line_[0] == 'a' || line_[0] == 'b') && !strcmp(line_ + 1, "_url")) {
            int idx = stations_find_url(&stations, v); /* URL wins over a stale index */
            synth_set_param(syn, P_A_STATION + (line_[0] - 'a') * OSC_STRIDE, (float)idx);
            continue;
        }
        if (settings_were_reset) continue;
        for (int i = 0; i < P_COUNT; i++)
            if (!strcmp(param_info[i].key, line_)) synth_set_param(syn, i, (float)atof(v));
    }
    fclose(f);
    if (ui.octave < -3 || ui.octave > 3) ui.octave = 0;
    if (ui.raw_fade < 0 || ui.raw_fade >= RAW_FADES) ui.raw_fade = 3;
    return 0;
}

static void save_stations(void)
{
    if (stations_save(&stations, stations_path)) toast("Could not save stations.txt");
}

static int station_param(int o) { return (int)synth_get_param(syn, P_A_STATION + o * OSC_STRIDE); }

/* stop a radio-scan switch in progress (a station chosen by hand wins) */
static void scan_cancel(int o)
{
    if (!xf_state[o]) return;
    audio_lock();
    xf_state[o] = 0;
    audio_unlock();
    stream_stop(st_next[o]);
}

static void tune_now(int o, int idx)
{
    scan_cancel(o);
    synth_set_param(syn, P_A_STATION + o * OSC_STRIDE, (float)idx);
    ui.pending_station[o] = -2;
    ui.tuned[o] = idx;
    if (idx >= 0 && idx < stations.count) stream_play(st[o], stations.items[idx].url);
    else stream_stop(st[o]);
    ui.dirty_settings = 1;
}

static void tune_later(int o, int idx)
{
    synth_set_param(syn, P_A_STATION + o * OSC_STRIDE, (float)idx);
    ui.pending_station[o] = idx;
    ui.pending_at[o] = SDL_GetTicks() + 700; /* debounce while scrolling */
}

/* another station for a voice: random, or the next one in the list */
static int pool_next(int o, int random)
{
    int n = stations.count, cur = station_param(o);
    if (n == 0) return -1;
    if (random && n > 1) {
        int i;
        do { i = rand() % n; } while (i == cur);
        return i;
    }
    return (cur + 1) % n;
}

static int first_of_kind(int cat, int fallback)
{
    for (int i = 0; i < stations.count; i++)
        if (stations.items[i].cat == cat) return i;
    return fallback < stations.count ? fallback : -1;
}

static void delete_station(int idx)
{
    scan_cancel(0);
    scan_cancel(1);
    stations_remove(&stations, idx);
    for (int o = 0; o < 2; o++) {
        int cur = station_param(o);
        if (cur == idx) tune_now(o, -1);
        else if (cur > idx) { synth_set_param(syn, P_A_STATION + o * OSC_STRIDE, (float)(cur - 1)); ui.tuned[o] = cur - 1; }
        if (ui.tuned[o] != station_param(o)) tune_now(o, station_param(o)); /* was scanning elsewhere: home */
    }
    save_stations();
    ui.dirty_settings = 1;
}

/* ================================================================== directory scan thread */

static int scan_kind;
static char scan_query[256];
static StationList scan_tmp;
static char scan_err[200];
static volatile int scan_done, scan_rc;

static void *scan_thread(void *arg)
{
    (void)arg;
    scan_rc = directory_search(scan_kind, scan_query, &scan_tmp, scan_err, sizeof scan_err);
    scan_done = 1;
    return NULL;
}

static void start_scan(int kind, const char *query, int cat)
{
    if (ui.scan_busy) return;
    scan_kind = kind;
    ui.scan_cat = cat;
    snprintf(scan_query, sizeof scan_query, "%s", query ? query : "");
    if (kind == 2) snprintf(ui.scan_title, sizeof ui.scan_title, "random stations");
    else snprintf(ui.scan_title, sizeof ui.scan_title, "%s: %s", kind == 0 ? "genre" : "search", scan_query);
    scan_done = 0;
    ui.scan_busy = 1;
    ui.scan_results.count = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, scan_thread, NULL) == 0) pthread_detach(t);
    else ui.scan_busy = 0;
}

/* ================================================================== notes, tempo */


/* ================================================================== Wi-Fi sync */

static int sc_mode(void) { return (int)synth_get_param(syn, P_SYNC); }
static float sc_bpm(void) { return synth_get_param(syn, P_BPM); }
static void sc_set_bpm(float b) { synth_set_param(syn, P_BPM, roundf(b * 10.0f) / 10.0f); }
static double sc_position(void) { audio_lock(); double p = synth_position(syn); audio_unlock(); return p; }
static void sc_set_position(double p)
{
    audio_lock();
    double was = synth_position(syn);
    synth_set_position(syn, p);
    aclock_shift(synth_position(syn) - was);
    audio_unlock();
}
static void sc_set_rate(double r) { synth_set_clock_rate(syn, r); }
static int sc_playing(void) { return synth_running(syn); }
static void sc_set_playing(int on) { synth_set_running(syn, on); }
static const SyncClock sync_clock = {sc_mode, sc_bpm, sc_set_bpm, sc_position, sc_set_position,
                                     sc_set_rate, sc_playing, sc_set_playing};


static void tap_tempo(void)
{
    Uint32 now = SDL_GetTicks();
    if (sync_status()->following) { toast("following the lead unit's tempo"); return; }
    if (ui.ntaps > 0 && now - ui.taps[ui.ntaps - 1] > 2000) ui.ntaps = 0;
    if (ui.ntaps == 8) { memmove(ui.taps, ui.taps + 1, sizeof(Uint32) * 7); ui.ntaps = 7; }
    ui.taps[ui.ntaps++] = now;
    synth_tap_reset(syn);
    if (ui.ntaps >= 2) {
        float avg = (float)(ui.taps[ui.ntaps - 1] - ui.taps[0]) / (ui.ntaps - 1);
        synth_set_param(syn, P_BPM, roundf(60000.0f / avg));
        ui.dirty_settings = 1;
        toast("tempo %.0f bpm", synth_get_param(syn, P_BPM));
    } else {
        toast("tap again to set the tempo");
    }
}

/* ================================================================== on-screen keyboard */

static const char *const kb_rows[2][4] = {
    {"1234567890-=", "qwertyuiop/:", "asdfghjkl.?&", "zxcvbnm_~%#@"},
    {"!\"$'()*+,;[]", "QWERTYUIOP/:", "ASDFGHJKL.?&", "ZXCVBNM_~%#@"},
};
static const char *const kb_special[] = {"SHIFT", "SPACE", "DEL", "http://", "https://", ".com", "DONE"};
#define KB_SPECIAL_N 7

static void open_keyboard(int purpose, const char *initial)
{
    ui.overlay = OV_KEYBOARD;
    ui.kb_purpose = purpose;
    ui.kb_row = ui.kb_col = ui.kb_shift = 0;
    snprintf(ui.kb_text, sizeof ui.kb_text, "%s", initial ? initial : "");
}

static void kb_append(const char *s)
{
    size_t n = strlen(ui.kb_text);
    snprintf(ui.kb_text + n, sizeof ui.kb_text - n, "%s", s);
}

static void kb_done(void)
{
    if (ui.kb_purpose == KB_ADD_URL) {
        if (strncasecmp(ui.kb_text, "http://", 7) && strncasecmp(ui.kb_text, "https://", 8) &&
            strncasecmp(ui.kb_text, "file://", 7)) {
            toast("the URL must start with http://, https:// or file://");
            return;
        }
        snprintf(ui.kb_url, sizeof ui.kb_url, "%s", ui.kb_text);
        char name[96];
        station_default_name(ui.kb_url, name, sizeof name);
        if (!stream_is_file(ui.kb_url)) name[strcspn(name, "/:")] = 0; /* just the host */
        open_keyboard(KB_ADD_NAME, name);
        return;
    }
    if (ui.kb_purpose == KB_ADD_NAME) {
        int idx = stations_add(&stations, ui.kb_text[0] ? ui.kb_text : "custom", ui.kb_url, CAT_ANY);
        ui.overlay = OV_NONE;
        if (idx < 0) { toast("station list is full"); return; }
        save_stations();
        ui.radio_sel = idx;
        toast("added %s  (L2 sets voice/music/sound)", stations.items[idx].name);
        return;
    }
    ui.overlay = OV_SCANRESULTS;
    start_scan(1, ui.kb_text, CAT_ANY);
}

static void kb_press(void)
{
    if (ui.kb_row < 4) {
        char ch[2] = {kb_rows[ui.kb_shift][ui.kb_row][ui.kb_col], 0};
        kb_append(ch);
        return;
    }
    switch (ui.kb_col) {
    case 0: ui.kb_shift = !ui.kb_shift; break;
    case 1: kb_append(" "); break;
    case 2: { size_t n = strlen(ui.kb_text); if (n) ui.kb_text[n - 1] = 0; break; }
    case 3: kb_append("http://"); break;
    case 4: kb_append("https://"); break;
    case 5: kb_append(".com"); break;
    default: kb_done(); break;
    }
}

static void kb_input(int b)
{
    int cols = ui.kb_row < 4 ? 12 : KB_SPECIAL_N;
    switch (b) {
    case B_UP: ui.kb_row = (ui.kb_row + 4) % 5; break;
    case B_DOWN: ui.kb_row = (ui.kb_row + 1) % 5; break;
    case B_LEFT: ui.kb_col = (ui.kb_col + cols - 1) % cols; break;
    case B_RIGHT: ui.kb_col = (ui.kb_col + 1) % cols; break;
    case B_A: kb_press(); break;
    case B_B: { size_t n = strlen(ui.kb_text); if (n) ui.kb_text[n - 1] = 0; break; }
    case B_Y: kb_append(" "); break;
    case B_X: ui.kb_shift = !ui.kb_shift; break;
    case B_START: kb_done(); break;
    case B_MENU: case B_SELECT: ui.overlay = OV_NONE; break;
    }
    cols = ui.kb_row < 4 ? 12 : KB_SPECIAL_N;
    if (ui.kb_col >= cols) ui.kb_col = cols - 1;
}

static void draw_keyboard(void)
{
    fill(20, 56, 600, 384, C_PANEL);
    const char *title = ui.kb_purpose == KB_ADD_URL ? "add station - stream URL" :
                        ui.kb_purpose == KB_ADD_NAME ? "add station - name" : "search the directory by name";
    text(36, 66, title, C_LEAF, 0);
    fill(36, 94, 568, 30, C_BG);
    int len = (int)strlen(ui.kb_text);
    const char *shown = len > 54 ? ui.kb_text + len - 54 : ui.kb_text;
    int w = text(44, 100, shown, C_TEXT, 0);
    if ((SDL_GetTicks() / 400) & 1) fill(44 + w + 1, 100, 2, FONT_H, C_LEAF);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 12; c++) {
            int cx = 61 + c * 47, cy = 162 + r * 48;
            int sel = ui.kb_row == r && ui.kb_col == c;
            disc(cx, cy, 20, sel ? C_LEAF : C_PANEL2);
            char ch[2] = {kb_rows[ui.kb_shift][r][c], 0};
            text(cx - 4, cy - 9, ch, sel ? C_BG : C_TEXT, 0);
        }
    int x = 40;
    for (int c = 0; c < KB_SPECIAL_N; c++) {
        int wbtn = (int)strlen(kb_special[c]) * FONT_W + 20;
        int sel = ui.kb_row == 4 && ui.kb_col == c;
        Uint32 bg = sel ? C_LEAF : (c == 0 && ui.kb_shift) ? C_SEL : C_PANEL2;
        disc(x + 14, 370, 14, bg);
        disc(x + wbtn - 14, 370, 14, bg);
        fill(x + 14, 356, wbtn - 28, 29, bg);
        text(x + 10, 361, kb_special[c], sel ? C_BG : C_TEXT, 0);
        x += wbtn + 6;
    }
    text(36, 404, "A type  B del  X shift  Y space  START done  MENU cancel", C_DIM, 0);
}

/* ================================================================== scan overlays */

static void scan_menu_input(int b)
{
    int n = scan_genre_count + 2;
    switch (b) {
    case B_UP: ui.scan_menu_sel = (ui.scan_menu_sel + n - 1) % n; break;
    case B_DOWN: ui.scan_menu_sel = (ui.scan_menu_sel + 1) % n; break;
    case B_LEFT: ui.scan_menu_sel = ui.scan_menu_sel >= 10 ? ui.scan_menu_sel - 10 : 0; break;
    case B_RIGHT: ui.scan_menu_sel = ui.scan_menu_sel + 10 < n ? ui.scan_menu_sel + 10 : n - 1; break;
    case B_A:
        if (ui.scan_menu_sel == 0) { open_keyboard(KB_SEARCH, ""); break; }
        ui.overlay = OV_SCANRESULTS;
        if (ui.scan_menu_sel == 1) start_scan(2, NULL, CAT_ANY);
        else start_scan(0, scan_genres[ui.scan_menu_sel - 2].tag, scan_genres[ui.scan_menu_sel - 2].cat);
        break;
    case B_B: case B_MENU: case B_R2: ui.overlay = OV_NONE; break;
    }
}

static Uint32 cat_col(int cat)
{
    switch (cat) {
    case CAT_VOICE: return C_WATER;
    case CAT_MUSIC: return C_LEAF;
    case CAT_SOUND: return C_BARK;
    default: return C_PANEL2;
    }
}

static void draw_list_frame(const char *title, const char *hint)
{
    fill(20, 40, 600, 404, C_PANEL);
    text(36, 50, title, C_LEAF, 50);
    text(36, 418, hint, C_DIM, 58);
}

static void draw_scan_menu(void)
{
    draw_list_frame("scan the airwaves  (radio-browser.info)", "A open  B back    river = voices  bark = sound  leaf = music");
    int n = scan_genre_count + 2, rows = 15;
    if (ui.scan_menu_sel < ui.scan_menu_scroll) ui.scan_menu_scroll = ui.scan_menu_sel;
    if (ui.scan_menu_sel >= ui.scan_menu_scroll + rows) ui.scan_menu_scroll = ui.scan_menu_sel - rows + 1;
    for (int r = 0; r < rows && ui.scan_menu_scroll + r < n; r++) {
        int i = ui.scan_menu_scroll + r;
        int y = 76 + r * 22;
        if (i == ui.scan_menu_sel) fill(28, y - 2, 584, 22, C_SEL);
        if (i == 0) text(52, y, "search by name...", C_TEXT, 0);
        else if (i == 1) text(52, y, "random stations", C_TEXT, 0);
        else {
            disc(40, y + 8, 5, cat_col(scan_genres[i - 2].cat));
            text(52, y, scan_genres[i - 2].tag, i == ui.scan_menu_sel ? C_LEAF : C_TEXT, 0);
        }
    }
}

static void scan_results_input(int b)
{
    int n = ui.scan_results.count;
    Station *s = n ? &ui.scan_results.items[ui.scan_sel] : NULL;
    switch (b) {
    case B_UP: if (n) ui.scan_sel = (ui.scan_sel + n - 1) % n; break;
    case B_DOWN: if (n) ui.scan_sel = (ui.scan_sel + 1) % n; break;
    case B_LEFT: ui.scan_sel = ui.scan_sel >= 10 ? ui.scan_sel - 10 : 0; break;
    case B_RIGHT: if (n) ui.scan_sel = ui.scan_sel + 10 < n ? ui.scan_sel + 10 : n - 1; break;
    case B_A: if (s) { stream_play(st[0], s->url); ui.tuned[0] = -1; toast("OSC A hears %s", s->name); } break;
    case B_Y: if (s) { stream_play(st[1], s->url); ui.tuned[1] = -1; toast("OSC B hears %s", s->name); } break;
    case B_X:
        if (s) {
            int cat = ui.scan_cat != CAT_ANY ? ui.scan_cat : s->cat;
            int idx = stations_add(&stations, s->name, s->url, cat);
            if (idx >= 0) { save_stations(); toast("saved %s", s->name); }
            else toast("station list is full");
        }
        break;
    case B_B: case B_MENU: ui.overlay = OV_SCANMENU; break;
    }
}

static void draw_scan_results(void)
{
    draw_list_frame(ui.scan_title, "A try on OSC A   Y try on OSC B   X save   B back");
    if (ui.scan_busy) {
        /* five orbiting dots while the directory answers */
        float t = SDL_GetTicks() / 1000.0f;
        for (int i = 0; i < 5; i++) {
            int x, y;
            polar(320, 220, 30, t * 0.4f + i / 5.0f, &x, &y);
            disc(x, y, 4, ring_cols[i]);
        }
        text_center(320, 270, "listening to the directory...", C_DIM, 0);
        return;
    }
    int n = ui.scan_results.count, rows = 15;
    if (n == 0) { text_center(320, 220, scan_rc < 0 ? scan_err : "no MP3 stations found", C_DIM, 56); return; }
    if (ui.scan_sel < ui.scan_scroll) ui.scan_scroll = ui.scan_sel;
    if (ui.scan_sel >= ui.scan_scroll + rows) ui.scan_scroll = ui.scan_sel - rows + 1;
    for (int r = 0; r < rows && ui.scan_scroll + r < n; r++) {
        int i = ui.scan_scroll + r;
        int y = 76 + r * 22;
        Station *s = &ui.scan_results.items[i];
        if (i == ui.scan_sel) fill(28, y - 2, 584, 22, C_SEL);
        disc(40, y + 8, 5, cat_col(ui.scan_cat != CAT_ANY ? ui.scan_cat : s->cat));
        if (stations_find_url(&stations, s->url) >= 0) circle(40, y + 8, 8, C_TEXT);
        text(54, y, s->name, i == ui.scan_sel ? C_LEAF : C_TEXT, 33);
        text_right(604, y, s->info, C_DIM, 21);
    }
    char c[32];
    snprintf(c, sizeof c, "%d/%d", ui.scan_sel + 1, n);
    text_right(604, 50, c, C_DIM, 0);
}

/* ================================================================== self-update */

static int update_auto; /* check GitHub for a newer release: on the TrimUI, or with RS_UPDATE=1 */

/* a few seconds after starting (so Wi-Fi is up), then every minute until GitHub has answered, five tries */
static void update_watch(Uint32 now)
{
    const UpdateStatus *u = update_status();
    if (update_auto && (u->state == UP_IDLE || u->state == UP_CHECK_FAILED) && ui.update_tries < 5 &&
        now >= ui.update_next_check) {
        if (u->state == UP_CHECK_FAILED) ev("  %s", u->err);
        ui.update_tries++;
        ui.update_next_check = now + 60000;
        update_check_start();
    }
    if (u->state == UP_AVAILABLE && !ui.update_asked && ui.overlay == OV_NONE) {
        ui.update_asked = 1;
        ui.overlay = OV_UPDATE;
        ev("  update: v%s is out (this is v" RS_VERSION ")", u->latest);
    }
    if (u->state == UP_INSTALLED && !ui.relaunch_at) {
        ev("  update: v%s installed, relaunching", u->latest);
        ui.relaunch_at = now + 1200; /* long enough to read "restarting" */
    }
    if (ui.relaunch_at && now >= ui.relaunch_at) ui.running = 0;
}

static void update_input(int b)
{
    const UpdateStatus *u = update_status();
    if (u->state == UP_INSTALLING || u->state == UP_INSTALLED) return;
    if (b == B_A && (u->state == UP_AVAILABLE || u->state == UP_FAILED)) { update_install_start(app_dir); return; }
    if (b == B_B || b == B_MENU) {
        ui.overlay = OV_NONE;
        if (u->state == UP_AVAILABLE) toast("v%s will be offered again next time", u->latest);
    }
}

static void draw_update(void)
{
    const UpdateStatus *u = update_status();
    char b[200];
    fill(60, 130, 520, 220, C_PANEL);
    snprintf(b, sizeof b, "RadioScan v%s is out", u->latest);
    text_center(320, 150, b, C_LEAF, 0);
    text_center(320, 176, "you have v" RS_VERSION, C_DIM, 0);
    if (u->state == UP_INSTALLING || u->state == UP_INSTALLED) {
        float t = SDL_GetTicks() / 1000.0f;
        for (int i = 0; i < 5; i++) {
            int x, y;
            polar(320, 250, 26, t * 0.4f + i / 5.0f, &x, &y);
            disc(x, y, 4, ring_cols[i]);
        }
        if (u->state == UP_INSTALLED) snprintf(b, sizeof b, "updated - restarting...");
        else snprintf(b, sizeof b, "downloading... %d%%", u->progress);
        text_center(320, 300, b, C_TEXT, 0);
        return;
    }
    if (u->state == UP_FAILED) {
        text_center(320, 220, u->err, C_RUST, 50);
        text_center(320, 244, "nothing was changed", C_DIM, 0);
        text_center(320, 310, "A try again    B close", C_TEXT, 0);
        return;
    }
    text_center(320, 220, "download it now and restart?", C_TEXT, 0);
    text_center(320, 244, "your stations and settings stay as they are", C_DIM, 0);
    text_center(320, 310, "A update and restart    B not now", C_TEXT, 0);
}

/* ================================================================== shared widgets */

static Uint32 osc_col(int o) { return o ? C_AMBER : C_LEAF; }
static int voice_muted(int o) { return (int)synth_get_param(syn, P_A_MUTE + o * OSC_STRIDE); }

static void osc_status_line(int o, char *buf, int n)
{
    char status[160], title[256];
    stream_status(st[o], status, sizeof status);
    stream_title(st[o], title, sizeof title);
    if (stream_state(st[o]) == ST_PLAYING && title[0]) snprintf(buf, n, "%s", title);
    else snprintf(buf, n, "%s", status);
}

static const char *osc_station_name(int o)
{
    int idx = station_param(o);
    int t = ui.tuned[o];
    if (t >= 0 && t != idx && t < stations.count) { /* radio scan has moved away from the voice's own station */
        static char away[2][100];
        snprintf(away[o], sizeof away[o], "~%s", stations.items[t].name);
        return away[o];
    }
    if (ui.tuned[o] == -1 && stream_state(st[o]) != ST_IDLE) return "(trying a scan result)";
    if (idx >= 0 && idx < stations.count) return stations.items[idx].name;
    return "no station";
}

static Uint32 state_col(int o)
{
    switch (stream_state(st[o])) {
    case ST_PLAYING: return C_LEAF;
    case ST_ERROR: return C_RUST;
    case ST_IDLE: return C_PANEL2;
    default: return C_AMBER;
    }
}

static void draw_toast(void)
{
    if (SDL_GetTicks() > ui.toast_until) return;
    int w = (int)strlen(ui.toast) * FONT_W + 32;
    if (w > 620) w = 620;
    int x = 320 - w / 2;
    disc(x + 15, 420, 15, C_TEXT);
    disc(x + w - 15, 420, 15, C_TEXT);
    fill(x + 15, 405, w - 30, 31, C_TEXT);
    text(x + 16, 411, ui.toast, C_BG, 58);
}

static void draw_beat(int x, int y)
{
    float ph = synth_beat_phase(syn);
    char b[24];
    if (synth_running(syn)) {
        disc(x, y + 8, 6, ph < 0.15f ? C_TEXT : C_PANEL2);
        snprintf(b, sizeof b, "%.0f bpm", synth_get_param(syn, P_BPM));
    } else { /* stopped: a square, as on a transport */
        fill(x - 5, y + 3, 11, 11, C_RUST);
        snprintf(b, sizeof b, "stopped");
    }
    text(x + 14, y, b, synth_running(syn) ? C_DIM : C_RUST, 0);
    if (sc_mode() != SYNC_OFF) { /* Wi-Fi sync: ring = searching, filled = linked */
        const SyncStatus *ss = sync_status();
        const char *t = ss->leading ? "lead" : ss->following ? "follow" : "sync";
        int linked = ss->leading || ss->following;
        int w = (int)strlen(t) * FONT_W;
        int bx = x - w - 26;
        if (linked) disc(bx, y + 8, 5, C_WATER);
        else circle(bx, y + 8, 5, C_PANEL2);
        text(bx + 10, y, t, linked ? C_WATER : C_PANEL2, 0);
    }
}

/* voice sense as five dots: river-blue for speech, leaf for music */
static void draw_voice_dots(int cx, int y, int o)
{
    float sp = synth_speechness(syn, o);
    int vs = synth_voice_state(syn, o);
    int lit = (int)(sp * 5 + 0.5f);
    for (int i = 0; i < 5; i++) {
        int x = cx - 32 + i * 16;
        if (vs == VS_SILENT) circle(x, y, 4, C_PANEL2);
        else if (i < lit) disc(x, y, 4, C_WATER);
        else disc(x, y, 4, C_PANEL2);
    }
}

/* ================================================================== PLAY screen */

/* Abstract sound forms. Each tap of the signal chain drives a "bloom": its core swells with loudness,
 * brightness sets how many petals it has (3, 5, 8, 13, 21) and how fast it turns, a spiky signal gives
 * sharp petals, and sudden attacks send out ripples. No waveform is drawn literally. */
typedef struct { float r, a; } Ripple;
typedef struct {
    float level, peak_env, slow;
    float bright, rough, spin;
    Ripple rip[5];
} Bloom;
static Bloom blooms[TAP_COUNT];

static void bloom_update(int tap, float dir)
{
    float rms, br, cr;
    synth_tap_features(syn, tap, &rms, &br, &cr);
    Bloom *b = &blooms[tap];
    b->peak_env = rms > b->peak_env ? rms : b->peak_env * 0.985f;
    float lv = rms / (b->peak_env > 0.003f ? b->peak_env : 0.003f);
    if (rms < 0.0006f) lv *= rms / 0.0006f; /* true silence stays still */
    b->level += (lv - b->level) * 0.35f;
    b->bright += (br - b->bright) * 0.08f;
    float rough = (cr - 1.3f) / 2.5f;
    rough = rough < 0 ? 0 : rough > 1 ? 1 : rough;
    b->rough += (rough - b->rough) * 0.15f;
    b->spin += (0.004f + b->bright * 0.025f) * dir;
    if (rms > b->slow * 1.6f && rms > 0.002f) /* an attack: release a ripple */
        for (int i = 0; i < 5; i++)
            if (b->rip[i].a < 0.05f) { b->rip[i].r = 0; b->rip[i].a = 1; break; }
    b->slow += (rms - b->slow) * 0.05f;
    for (int i = 0; i < 5; i++) {
        b->rip[i].r += 0.05f;
        b->rip[i].a *= 0.88f;
    }
}

static void update_blooms(void)
{
    for (int t = 0; t < TAP_COUNT; t++) bloom_update(t, (t & 1) ? -1.0f : 1.0f);
}

/* a bloom in the band rin..rout; ripples travel out to rmax. fold > 0 shapes the petals through the
 * folder, so folding is seen as petals bending and splitting. */
static void draw_bloom(int cx, int cy, float rin, float rout, float rmax, int tap, Uint32 col, float fold)
{
    static const int fib[5] = {3, 5, 8, 13, 21};
    const Bloom *b = &blooms[tap];
    for (int i = 0; i < 5; i++)
        if (b->rip[i].a > 0.05f) circle(cx, cy, (int)(rin + (rmax - rin) * b->rip[i].r), mix_col(C_BG, col, b->rip[i].a * 0.6f));
    float bn = b->bright / 1.1f;
    bn = bn < 0 ? 0 : bn > 1 ? 1 : bn;
    float fi = bn * (rout - rin > 30 ? 4.0f : 3.0f);
    int k0i = (int)fi, k1i = k0i + 1 > 4 ? 4 : k0i + 1;
    float w = fi - k0i;
    float lv = b->level > 1 ? 1 : b->level;
    float rc = rin + (rout - rin) * (0.12f + 0.38f * lv);
    float amp = (rout - rin) * (0.22f + 0.45f * b->rough) * (0.25f + 0.75f * lv);
    float sharp = 1.0f + b->rough * 3.0f;
    int x0 = 0, y0 = 0;
    const int N = 120;
    for (int i = 0; i <= N; i++) {
        float th = (float)(i % N) / N * 2.0f * (float)M_PI, g = 0;
        for (int pass = 0; pass < 2; pass++) {
            float k = (float)fib[pass ? k1i : k0i];
            float u = sinf(k * th + b->spin), gv;
            if (fold > 0.01f) gv = 0.5f + 0.5f * synth_fold_preview(u * 0.8f, fold);
            else gv = powf(0.5f + 0.5f * u, sharp);
            g += pass ? w * gv : (1 - w) * gv;
        }
        int x, y;
        polar(cx, cy, rc + amp * g, (float)(i % N) / N, &x, &y);
        if (i) line(x0, y0, x, y, col);
        x0 = x;
        y0 = y;
    }
}

/* ------------------------------------------------------------------ spectrum backdrop */

#define FFT_N 1024
#define SPEC_BANDS 64

static void fft(float *re, float *im, int n) /* in-place radix-2 */
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

/* band powers (log-spaced 50 Hz .. 16 kHz) of a tap */
static void tap_bands(int tap, float *bands)
{
    static float re[FFT_N], im[FFT_N], win[FFT_N];
    if (win[1] == 0)
        for (int i = 0; i < FFT_N; i++) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
    synth_tap_read(syn, tap, re, FFT_N);
    for (int i = 0; i < FFT_N; i++) { re[i] *= win[i]; im[i] = 0; }
    fft(re, im, FFT_N);
    static int edge[SPEC_BANDS + 1];
    if (!edge[SPEC_BANDS]) { /* FFT bins at the band edges, computed once */
        float hz_per_bin = (float)audio_rate / FFT_N;
        for (int b = 0; b <= SPEC_BANDS; b++) {
            int k = (int)(50.0f * powf(320.0f, (float)b / SPEC_BANDS) / hz_per_bin);
            if (b && k <= edge[b - 1]) k = edge[b - 1] + 1;
            edge[b] = k > FFT_N / 2 ? FFT_N / 2 : k;
        }
    }
    for (int b = 0; b < SPEC_BANDS; b++) {
        int k0 = edge[b], k1 = edge[b + 1] > k0 ? edge[b + 1] : k0 + 1;
        float pw = 0;
        for (int k = k0; k < k1; k++) pw += re[k] * re[k] + im[k] * im[k];
        bands[b] = pw / (k1 - k0);
    }
}

/* Soft spectrum of the overall sound behind the circles, mirrored around the middle of the screen. Its colour runs
 * with frequency, from indigo bass through rose mids to sky-blue highs: hues away from the voices' green and amber,
 * so it reads as the whole mix rather than either voice. */
static Uint32 spectrum_col(float t) /* t: 0 lowest band .. 1 highest */
{
    static const Uint32 stops[3] = {RGB(118, 92, 214), RGB(206, 102, 168), RGB(118, 192, 232)};
    return t < 0.5f ? mix_col(stops[0], stops[1], t * 2) : mix_col(stops[1], stops[2], t * 2 - 1);
}

static void draw_spectrum(void)
{
    static float height[SPEC_BANDS];
    float out[SPEC_BANDS];
    tap_bands(TAP_OUT, out);
    float w = (float)SCREEN_W / SPEC_BANDS;
    for (int i = 0; i < SPEC_BANDS; i++) {
        float db = 10.0f * log10f(out[i] + 1e-12f);       /* about -110 .. +30 */
        float h = (db + 60.0f) / 80.0f;
        h = h < 0 ? 0 : h > 1 ? 1 : h;
        height[i] = h > height[i] ? h : height[i] * 0.9f; /* rises fast, falls gently */
        int hp = (int)(height[i] * 130);
        if (hp < 1) continue;
        Uint32 col = mix_col(C_BG, spectrum_col((float)i / (SPEC_BANDS - 1)), 0.10f + 0.18f * height[i]); /* a quiet backdrop */
        int x = (int)(i * w);
        fill(x + 1, 240 - hp, (int)w - 2, hp * 2, col);
    }
}

/* centred text with a dark outline 2 px wide, so it reads over a busy waveform. The outline is each glyph grown
 * by 2 px in every direction, drawn in one pass, then the text goes on top. */
static void text_halo(int cx, int y, const char *s, Uint32 col)
{
    Uint32 m = map_col(C_PANEL);
    int x = cx - (int)strlen(s) * FONT_W / 2;
    for (const char *c = s; *c; c++, x += FONT_W) {
        unsigned char ch = (unsigned char)*c;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const uint16_t *g = font_bits[ch - FONT_FIRST];
        for (int r = -2; r < FONT_H + 2; r++) {
            uint32_t rows = 0; /* the glyph rows within 2 of this one, shifted so column -2 is bit 19 */
            for (int rr = r - 2; rr <= r + 2; rr++)
                if (rr >= 0 && rr < FONT_H) rows |= (uint32_t)g[rr] << 2;
            uint32_t d = rows | rows << 1 | rows << 2 | rows >> 1 | rows >> 2;
            for (int k = -2; k < FONT_W + 2; k++)
                if (d & (1u << (17 - k))) pset(x + k, y + r, m);
        }
    }
    text_center(cx, y, s, col, 0);
}

/* PLAY layout: two voices with the cross stage between them, each wearing its rhythm as a ring of steps */
#define VOICE_Y 118
#define VOICE_R 80
#define VOICE_AX 108
#define VOICE_BX 532
#define CENTRE_R 68
#define STUT_Y 256          /* the stutter rings, under the voices */
#define STUT_R 30

static const char *seq_name(int q)
{
    static const char *const names[SEQ_COUNT] = {"A", "B", "cross", "A stutter", "B stutter", "cross stutter"};
    return names[q];
}
static Uint32 seq_col(int q) { return SEQ_VOICE(q) == SEQ_X ? C_TEXT : osc_col(SEQ_VOICE(q)); }

/* a rhythm's steps around a circle: pulses lit (with a dark edge so they read over a waveform), a white ring on the
 * current step. big: the gate rings around the voices; otherwise the small stutter rings. */
static void draw_steps(int q, int cx, int cy, float r, int big)
{
    const float *p = synth_params(syn);
    int on = (int)p[SEQ_PARAM(q, 0)], steps = (int)p[SEQ_PARAM(q, 1)];
    int pulses = (int)p[SEQ_PARAM(q, 2)], rot = (int)p[SEQ_PARAM(q, 3)];
    Uint32 cc = on ? seq_col(q) : mix_col(C_PANEL2, seq_col(q), 0.3f);
    int cur = synth_seq_step(syn, q);
    float fl = synth_seq_flash(syn, q);
    for (int i = 0; i < steps; i++) {
        int dx, dy;
        polar(cx, cy, r, (float)i / steps, &dx, &dy);
        int now = i == cur && on;
        Uint32 dot = now ? mix_col(cc, C_TEXT, fl) : cc;
        if (!euclid_hit(steps, pulses, rot, i)) {
            if (big) disc(dx, dy, 4, C_BG);
            disc(dx, dy, big ? 2 : 1, mix_col(C_PANEL2, C_DIM, big ? 0.5f : 0.4f));
        } else if (big) {
            int rr = now ? 6 + (int)(fl * 3) : 6;
            disc(dx, dy, rr + 2, C_BG);
            disc(dx, dy, rr, dot);
        } else {
            disc(dx, dy, 5, C_BG);
            disc(dx, dy, now ? 4 + (int)(fl * 2) : 3, dot);
        }
        if (now && big) { circle(dx, dy, 9, C_TEXT); circle(dx, dy, 10, C_TEXT); }
        else if (now) circle(dx, dy, 6, C_TEXT);
    }
}

/* a stutter rhythm, drawn geometrically: its pulses joined into a polygon, which lights up when a step ratchets */
static void draw_stut_ring(int o, int cx, int cy, int r) /* o: voice 0, 1 or SEQ_X */
{
    const float *p = synth_params(syn);
    int q = SEQ_SA + o;
    Uint32 col = seq_col(q);
    int on = (int)p[SEQ_PARAM(q, 0)], steps = (int)p[SEQ_PARAM(q, 1)];
    int pulses = (int)p[SEQ_PARAM(q, 2)], rot = (int)p[SEQ_PARAM(q, 3)];
    float fl = on ? synth_seq_flash(syn, q) : 0;
    disc(cx, cy, r + 6, C_PANEL);
    if (ui.cell == q) { circle(cx, cy, r + 10, col); circle(cx, cy, r + 11, col); }
    int px_[16], py_[16], k = 0;
    for (int i = 0; i < steps; i++)
        if (euclid_hit(steps, pulses, rot, i)) polar(cx, cy, r, (float)i / steps, &px_[k], &py_[k]), k++;
    Uint32 lc = mix_col(on ? mix_col(C_PANEL2, col, 0.55f) : C_PANEL2, C_TEXT, fl);
    if (k == 1) line(cx, cy, px_[0], py_[0], lc);
    for (int i = 0; k > 1 && i < k; i++) {
        line(px_[i], py_[i], px_[(i + 1) % k], py_[(i + 1) % k], lc);
        if (fl > 0.05f) line(px_[i] + 1, py_[i], px_[(i + 1) % k] + 1, py_[(i + 1) % k], lc); /* a flash thickens it */
    }
    draw_steps(q, cx, cy, r, 0);
    char t[8], mode[16] = "stutter";
    if (on) {
        snprintf(t, sizeof t, "x%d", (int)p[RHYTHM_P(o, STUT_RATCHET)]);
        int id = RHYTHM_P(o, STUT_MODE); /* its mode's name, in lower case */
        snprintf(mode, sizeof mode, "%s", param_info[id].choices[(int)p[id]]);
        for (char *c = mode; *c; c++) *c = (char)tolower((unsigned char)*c);
    } else {
        snprintf(t, sizeof t, "off");
    }
    int lx = o == 1 ? cx - r - 44 : cx + r + 44; /* beside the ring, so the shape stays clear */
    text_halo(lx, cy - 18, mode, on ? C_DIM : C_PANEL2);
    text_halo(lx, cy, t, on ? (fl > 0.3f ? C_TEXT : C_DIM) : C_PANEL2);
}

/* a gate rhythm, hi5 style, as a ring of steps (outlined when selected) */
static void draw_seq_ring(int q, int cx, int cy, float r)
{
    if (ui.cell == q) { circle(cx, cy, (int)r + 9, seq_col(q)); circle(cx, cy, (int)r + 10, seq_col(q)); }
    draw_steps(q, cx, cy, r, 1);
}

static void seq_label(int q, char *t, int n)
{
    const float *p = synth_params(syn);
    if ((int)p[SEQ_PARAM(q, 0)]) snprintf(t, n, "%d/%d", (int)p[SEQ_PARAM(q, 2)], (int)p[SEQ_PARAM(q, 1)]);
    else snprintf(t, n, "open");
}

static void seq_rate_label(int q, char *t, int n) { param_format_ctx(SEQ_SHAPE(q, RATE), synth_params(syn), t, n); }

/* A waveform wrapped around a circle, like a polar oscilloscope: the line swings out and in with the signal.
 * It is triggered on a rising zero crossing so it holds still on a steady tone, and scaled to its own recent
 * peak so even a quiet station swings wide. *env keeps that peak between frames. */
static void draw_polar_wave(int cx, int cy, float base, float swing, int tap, Uint32 col, int thick, float *env)
{
    enum { WN = 2048, SPAN = 1024, PTS = 180 };
    static float buf[WN];
    synth_tap_read(syn, tap, buf, WN);
    int start = WN - SPAN;
    for (int i = WN - SPAN - 1; i > WN - SPAN - 900; i--) /* the latest rising zero crossing with room after it */
        if (buf[i - 1] < 0 && buf[i] >= 0) { start = i; break; }
    float pk = 0;
    for (int i = 0; i < SPAN; i++) pk = fmaxf(pk, fabsf(buf[start + i]));
    *env = pk > *env ? pk : *env * 0.93f;
    float g = 1.0f / fmaxf(*env, 0.01f);
    if (pk < 0.0005f) g = 0; /* silence: a still circle */
    float v[PTS];
    for (int k = 0; k < PTS; k++) { /* average each slice so the line is smooth, not aliased */
        int a = start + k * SPAN / PTS, b = start + (k + 1) * SPAN / PTS;
        float sum = 0, mx = 0;
        for (int i = a; i < b; i++) { sum += buf[i]; if (fabsf(buf[i]) > fabsf(mx)) mx = buf[i]; }
        v[k] = 0.5f * (sum / (b - a)) + 0.5f * mx; /* keep some of the peaks: it should look lively */
        v[k] *= g;
        v[k] = v[k] > 1 ? 1 : v[k] < -1 ? -1 : v[k];
    }
    for (int k = 0; k < PTS / 10; k++) { /* close the loop smoothly */
        float t = (float)k / (PTS / 10);
        v[PTS - 1 - k] = v[PTS - 1 - k] * t + v[0] * (1 - t);
    }
    static float sn[PTS + 1], cs[PTS + 1]; /* the same angles every frame: what polar() would compute */
    if (cs[0] == 0)
        for (int k = 0; k <= PTS; k++) {
            float frac = (float)k / PTS;
            sn[k] = sinf(frac * 2 * (float)M_PI);
            cs[k] = cosf(frac * 2 * (float)M_PI);
        }
    for (int th = 0; th < thick; th++) {
        int x0 = 0, y0 = 0;
        for (int k = 0; k <= PTS; k++) {
            float r = base + th + swing * v[k % PTS];
            int x = cx + (int)lroundf(sn[k] * r), y = cy - (int)lroundf(cs[k] * r);
            if (k) line(x0, y0, x, y, col);
            x0 = x;
            y0 = y;
        }
    }
}

/* a voice: the raw broadcast's waveform faint inside, the voice as it sounds bright outside, and its rhythm
 * around the rim */
static void draw_voice(int o, int cx, int cy, int R)
{
    Uint32 col = osc_col(o);
    const float *p = synth_params(syn);
    float gate = synth_gate_value(syn, o);
    disc(cx, cy, R, C_PANEL);
    circle(cx, cy, R, mix_col(C_PANEL2, col, gate * synth_osc_level(syn, o) * 2.0f));
    if (synth_floor(syn) == o) { circle(cx, cy, R + 1, col); circle(cx, cy, R + 2, col); } /* converse: has the floor */
    static float env_raw[2], env_out[2];
    draw_polar_wave(cx, cy, R * 0.34f, R * 0.14f, o ? TAP_RAW_B : TAP_RAW_A, mix_col(C_PANEL, col, 0.5f), 1, &env_raw[o]);
    float open = gate > 0.02f ? 0.35f + 0.65f * gate : 0.0f; /* the swing opens and closes with the rhythm */
    draw_polar_wave(cx, cy, R * 0.58f, R * 0.26f * open, o ? TAP_B : TAP_A, col, 2, &env_out[o]);
    draw_seq_ring(o, cx, cy, R - 7);
    static const char *const mn[] = {"live", "loop", "wave", "grain"};
    char t[16];
    seq_label(o, t, sizeof t);
    int mode = (int)p[P_A_MODE + o * OSC_STRIDE], dir = (int)p[P_A_DIR + o * OSC_STRIDE];
    char mt[16];
    snprintf(mt, sizeof mt, "%s%s", mn[mode], mode == OSC_LIVE || dir == DIR_FORWARD ? "" : dir == DIR_REVERSE ? " rev" : " <>");
    text_halo(cx, cy - 20, mt, C_TEXT);
    if (voice_muted(o)) text_halo(cx, cy - 44, "muted", C_RUST);
    disc(cx, cy + 4, 4, state_col(o));
    text_halo(cx, cy + 12, t, (int)p[SEQ_PARAM(o, 0)] ? C_DIM : C_PANEL2);
    if ((int)p[SEQ_PARAM(o, 0)]) {
        seq_rate_label(o, t, sizeof t);
        text_halo(cx, cy + 30, t, C_DIM);
    }
}

/* five circles along the chain after the voices; each grows the more it changes the sound */
static const char *const stage_names[5] = {"cross", "filter", "fold", "echo", "space"};
static const int stage_taps[5] = {TAP_CROSS, TAP_FILTER, TAP_FOLD, TAP_ECHO, TAP_OUT};

static float stage_amount(int i)
{
    const float *p = synth_params(syn);
    switch (i) {
    case 0: return synth_cross_amount(syn);
    case 1: { /* filter and bass / treble */
        float tone = synth_played_param(syn, P_TONE); /* with the LFOs and the right stick */
        float f = fabsf(tone) > 0.02f ? fabsf(tone) + p[P_RESO] * 0.2f : 0;
        float eq = (fabsf(p[P_LOW]) + fabsf(p[P_HIGH])) / 24.0f;
        return fminf(1.0f, f > eq ? f : eq);
    }
    case 2: return fmaxf(p[P_FOLD], fmaxf(p[P_DRIVE], p[P_CRUSH])); /* drive, fold, crush */
    case 3: return p[P_ECHO];
    default: return fminf(1.0f, fmaxf(p[P_SPACE], fabsf(p[P_WIDTH] - 1.0f)));
    }
}

static void draw_stages(int x0, int y, int spacing, float rmin, float rmax, int highlight, int labels)
{
    for (int i = 0; i < 5; i++) {
        int cx = x0 + i * spacing;
        float amt = stage_amount(i);
        float R = rmin + (rmax - rmin) * amt;
        if (i < 4) line(cx + (int)R + 3, y, cx + spacing - (int)(rmin + (rmax - rmin) * stage_amount(i + 1)) - 3, y, C_PANEL2);
        disc(cx, y, (int)R, C_PANEL);
        Uint32 col = amt > 0.01f ? mix_col(C_DIM, C_TEXT, amt) : C_PANEL2;
        circle(cx, y, (int)R, i == highlight ? C_TEXT : mix_col(C_PANEL2, C_LEAF, amt));
        if (i == highlight) circle(cx, y, (int)R + 4, C_TEXT);
        draw_bloom(cx, y, R * 0.2f, R * 0.92f, R, stage_taps[i], col, i == 2 ? synth_get_param(syn, P_FOLD) : 0);
        if (labels) text_center(cx, y + (int)rmax + 6, stage_names[i], amt > 0.01f ? C_DIM : C_PANEL2, 0);
    }
}

/* one line under the centre: a bar for the effect's set amount, brighter for how much of it is let through
 * now (open), and what it does */
static void centre_fx(int cx, int *y, int *n, float amt, float open, Uint32 col, const char *what)
{
    if (++*n > 3) return;
    int bx = cx - 92;
    float now = amt * open;
    fill(bx, *y + 6, 36, 6, C_PANEL);
    fill(bx, *y + 6, (int)(36 * fminf(amt, 1.0f)), 6, mix_col(C_PANEL, col, 0.35f));
    fill(bx, *y + 6, (int)(36 * fminf(now, 1.0f)), 6, col);
    text(bx + 44, *y, what, now > 0.02f ? C_TEXT : C_DIM, 15);
    *y += 17;
}

/* centre: the final sound with the cross rhythm around it, the morph position travelling over the top from
 * A (left) to B (right), and underneath, in words, what the cross stage is doing to the sound right now */
static void draw_centre(int cx, int cy)
{
    const float *p = synth_params(syn);
    int on = (int)p[P_X_GATE_ON];
    float open = synth_cross_gate(syn) * fminf(p[P_CROSS_LEVEL], 1.0f); /* the cross rhythm and the MIX return */
    float amount = synth_cross_amount(syn);
    disc(cx, cy, CENTRE_R, C_PANEL);
    circle(cx, cy, CENTRE_R, mix_col(C_PANEL2, C_TEXT, amount * open));
    draw_bloom(cx, cy, 12, CENTRE_R - 16, CENTRE_R + 20, TAP_OUT, C_TEXT, 0);
    draw_seq_ring(SEQ_X, cx, cy, CENTRE_R - 7);
    text_center(cx, cy - 20, "cross", amount > 0.01f ? C_TEXT : C_DIM, 0);
    char t[16];
    seq_label(SEQ_X, t, sizeof t);
    text_halo(cx, cy + 12, t, on ? C_DIM : C_PANEL2);
    if (on) { seq_rate_label(SEQ_X, t, sizeof t); text_halo(cx, cy + 30, t, C_DIM); }

    float m = synth_played_param(syn, P_MORPH); /* with the LFOs and the right stick */
    int ar = CENTRE_R + 20;
    arc(cx, cy, ar, 2, 0.75f, 1.25f, C_PANEL2);
    int mx, my;
    polar(cx, cy, ar, 0.75f + 0.5f * m, &mx, &my);
    disc(mx, my, m > 0.01f ? 6 : 4, m > 0.01f ? mix_col(C_LEAF, C_AMBER, m) : C_PANEL2);

    /* one line per active effect (up to three): a bar for how much it is set and how much is applied now, and
     * what it does */
    int y = cy + CENTRE_R + 8, n = 0;
    int voc = (int)p[P_VOCODE];
    if (voc && p[P_VOCODE_AMT] > 0.01f)
        centre_fx(cx, &y, &n, p[P_VOCODE_AMT], open, voc == VOC_A_TO_B ? C_LEAF : C_AMBER,
                  voc == VOC_A_TO_B ? "A talks thru B" : "B talks thru A");
    if (m > 0.01f) {
        char what[24];
        snprintf(what, sizeof what, "morph %.0f%% to B", m * 100);
        centre_fx(cx, &y, &n, m, open, mix_col(C_LEAF, C_AMBER, m), what);
    }
    if (p[P_FM] > 0.01f) {
        int ab = (int)p[P_FM_DIR] == FM_A_TO_B;
        centre_fx(cx, &y, &n, p[P_FM], synth_cross_gate(syn), ab ? C_LEAF : C_AMBER, ab ? "A bends B pitch" : "B bends A pitch");
    }
    if (p[P_RING] > 0.01f) centre_fx(cx, &y, &n, p[P_RING], open, C_TEXT, "ring A x B");
    if (p[P_CONVERSE] > 0.01f) {
        int f = synth_floor(syn);
        centre_fx(cx, &y, &n, p[P_CONVERSE], 1, f == 1 ? C_AMBER : C_LEAF, f == 1 ? "B talks, A waits" : "A talks, B waits");
    }
    if (n == 0) text_center(cx, y, "no cross-mod", C_PANEL2, 0);
}

static void draw_play(void)
{
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    draw_spectrum();
    text(16, 8, "radioscan", C_DIM, 0);
    draw_beat(520, 8);
    char info[48];
    snprintf(info, sizeof info, "pitch %+d", ui.octave);
    text(126, 8, info, C_DIM, 0);

    draw_voice(0, VOICE_AX, VOICE_Y, VOICE_R);
    draw_voice(1, VOICE_BX, VOICE_Y, VOICE_R);
    draw_centre(320, VOICE_Y);
    for (int o = 0; o < 2; o++) {
        int cx = o ? VOICE_BX : VOICE_AX;
        draw_voice_dots(cx, VOICE_Y + VOICE_R + 12, o);
        draw_stut_ring(o, cx, STUT_Y, STUT_R);
        if (o == 0) draw_stut_ring(SEQ_X, 320, STUT_Y + 26, 24); /* under the centre: the mix after cross / morph */
        text_center(cx, STUT_Y + STUT_R + 14, osc_station_name(o), C_TEXT, 19);
        char line_[256];
        osc_status_line(o, line_, sizeof line_);
        text_center(cx, STUT_Y + STUT_R + 31, line_, C_DIM, 19);
    }
    if (SDL_GetTicks() < ui.grab_flash)
        for (int o = 0; o < 2; o++)
            if (ui.grab_osc < 0 || ui.grab_osc == o) text_center(o ? VOICE_BX : VOICE_AX, VOICE_Y + 34, "grabbed", C_TEXT, 0);

    int hl = -1;
    if (ui.held[B_R2]) {
        if (ui.held[B_UP] || ui.held[B_DOWN]) hl = 1;
        if (ui.held[B_X] || ui.held[B_B]) hl = 2;
        if (ui.held[B_Y] || ui.held[B_A]) hl = 4;
        if (ui.held[B_LEFT] || ui.held[B_RIGHT]) hl = 0;
    }
    draw_stages(72, 394, 124, 14, 28, hl, 1);

    int stut = ui.cell >= SEQ_SA;
    const char *hint = ui.held[B_R2] ? "dpad: filter / morph   X B: fold   Y A: space" :
                       ui.held[B_L2] ? (stut ? "^v steps  <> rate  Y X ratchets  B mode  A start/stop" :
                                               "^v steps  <> rate  Y X length  B mute  A start/stop") :
                                       "<> rhythm  ^v pulses  A on/off  B fewer  X rotate  Y new";
    text_center(320, 344, hint, C_DIM, 0);
    text(16, 455, "L1/R1 pitch  L2 grab  SELECT tap  START edit  MENU x2 quit", C_PANEL2, 0);
}

/* ================================================================== RAW screen */

#define RAW_X0 64 /* the crossfader's travel */
#define RAW_X1 576
#define RAW_Y 276

static void draw_raw(void)
{
    static float shown[2];
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    text(16, 8, "radioscan", C_DIM, 0);
    text(126, 8, "RAW", C_TEXT, 0);
    text_right(624, 8, "no engine, no effects", C_PANEL2, 0);
    float x = toward(raw_xf, raw_hand, 0, 1);
    for (int o = 0; o < 2; o++) {
        int x0 = o ? 332 : 24, w = 284, cx = x0 + w / 2;
        float g = o ? x : 1 - x; /* this stream's share */
        Uint32 col = osc_col(o);
        fill(x0, 44, w, 168, C_PANEL);
        disc(cx, 80, 22, mix_col(C_PANEL2, col, 0.25f + 0.75f * g));
        text_center(cx, 71, o ? "B" : "A", C_BG, 0);
        text_center(cx, 116, osc_station_name(o), C_TEXT, 27);
        char line_[256];
        osc_status_line(o, line_, sizeof line_);
        text_center(cx, 138, line_, C_DIM, 27);
        /* the stream's level as it arrives (-48 .. 0 dB), then what the fader lets through */
        shown[o] = fmaxf(raw_peak[o], shown[o] * 0.8f);
        raw_peak[o] = 0;
        float db = 20.0f * log10f(shown[o] + 1e-6f), mf = (db + 48.0f) / 48.0f;
        mf = mf < 0 ? 0 : mf > 1 ? 1 : mf;
        int mx = x0 + 20, mw = w - 40;
        fill(mx, 168, mw, 8, C_BG);
        fill(mx, 168, (int)(mf * mw), 8, db > -1.0f ? C_RUST : col);
        char gs[24];
        if (g <= 0) snprintf(gs, sizeof gs, "faded out");
        else if (g >= 1) snprintf(gs, sizeof gs, "fader 0 dB");
        else snprintf(gs, sizeof gs, "fader %.1f dB", 20.0f * log10f(g));
        text_center(cx, 184, gs, g > 0 ? C_TEXT : C_DIM, 0);
    }

    /* the crossfader */
    text_center(RAW_X0 - 28, RAW_Y - 9, "A", C_LEAF, 0);
    text_center(RAW_X1 + 28, RAW_Y - 9, "B", C_AMBER, 0);
    fill(RAW_X0, RAW_Y - 3, RAW_X1 - RAW_X0, 6, C_PANEL2);
    for (int i = 0; i <= 4; i++) {
        int tx = RAW_X0 + i * (RAW_X1 - RAW_X0) / 4, th = i == 2 ? 14 : 8;
        fill(tx, RAW_Y - 22 - th, 1, th, C_DIM);
    }
    if (ui.raw_to >= 0) { /* where a fade is heading */
        int gx = RAW_X0 + (int)(ui.raw_to * (RAW_X1 - RAW_X0));
        fill(gx - 1, RAW_Y + 24, 3, 10, C_DIM);
    }
    int tx = RAW_X0 + (int)(x * (RAW_X1 - RAW_X0));
    fill(tx - 11, RAW_Y - 20, 23, 41, C_TEXT);
    fill(tx - 9, RAW_Y, 19, 1, C_BG);
    char fs[48], fade[16];
    float f = raw_fade_s[ui.raw_fade];
    if (f <= 0) snprintf(fade, sizeof fade, "cut");
    else snprintf(fade, sizeof fade, "%g s", f);
    const char *to = ui.raw_to <= 0 ? "A" : ui.raw_to >= 1 ? "B" : "centre";
    if (ui.raw_to >= 0) snprintf(fs, sizeof fs, "fading to %s   fade %s", to, fade);
    else snprintf(fs, sizeof fs, "fade %s", fade);
    text_center(320, RAW_Y + 40, fs, C_DIM, 0);

    text_center(320, 352, "<> fader (B held: slow)   L1/R1 fade to A/B   A across", C_DIM, 0);
    text_center(320, 374, "Y centre   ^v fade time   L2/R2 + ^v station A/B", C_DIM, 0);
    text(16, 455, "START edit   MENU + SELECT leave RAW   MENU x2 quit", C_PANEL2, 0);
}

/* ================================================================== EDIT screen */

static int tab_page(int tab) { return tab - 1; } /* VOICES, RHYTHM, ENGINE, MOD, MIX -> PAGE_* */

/* rows for the current tab; ids >= 0 are params, ROW_HEADER rows carry a section name */
static int build_rows(int tab, int *rows, const char **hdrs)
{
    int n = 0;
    const char *last = NULL;
    int page = tab_page(tab);
    for (int i = 0; i < P_COUNT; i++) {
        if (param_info[i].page != page) continue;
        const char *sec = param_section(i);
        if (!last || strcmp(sec, last)) { rows[n] = ROW_HEADER; hdrs[n++] = sec; last = sec; }
        rows[n] = i;
        hdrs[n++] = NULL;
    }
    return n;
}

static void draw_tabs(void)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        int x = 30 + i * 104;
        int sel = ui.tab == i;
        if (sel) disc(x, 16, 7, C_LEAF);
        else circle(x, 16, 7, C_DIM);
        text(x + 14, 7, tab_names[i], sel ? C_TEXT : C_DIM, 0);
    }
}

static void draw_radio_page(void)
{
    int rows = 13;
    char b[160];
    snprintf(b, sizeof b, "%d stations", stations.count);
    text(24, 38, b, C_DIM, 0);
    text_right(624, 38, "A/B to voice  Y add  X del  L2 kind  R2 scan", C_PANEL2, 0);
    if (stations.count == 0) { text_center(320, 180, "no stations yet - Y adds a URL, R2 scans the directory", C_DIM, 0); return; }
    if (ui.radio_sel >= stations.count) ui.radio_sel = stations.count - 1;
    if (ui.radio_sel < ui.radio_scroll) ui.radio_scroll = ui.radio_sel;
    if (ui.radio_sel >= ui.radio_scroll + rows) ui.radio_scroll = ui.radio_sel - rows + 1;
    for (int r = 0; r < rows && ui.radio_scroll + r < stations.count; r++) {
        int i = ui.radio_scroll + r;
        int y = 62 + r * 22;
        Station *s = &stations.items[i];
        if (i == ui.radio_sel) fill(8, y - 2, 624, 22, ui.confirm_delete ? RGB(80, 34, 24) : C_SEL);
        disc(24, y + 8, 5, cat_col(s->cat));
        text(40, y, s->name, i == ui.radio_sel ? C_TEXT : C_DIM, 48);
        for (int o = 0; o < 2; o++)
            if (station_param(o) == i) {
                disc(584 + o * 26, y + 8, 9, osc_col(o));
                text(580 + o * 26, y, o ? "B" : "A", C_BG, 0);
            }
    }
    Station *s = &stations.items[ui.radio_sel];
    if (ui.confirm_delete) text(24, 352, "press X again to let this station go", C_RUST, 0);
    else {
        snprintf(b, sizeof b, "%s  %s", category_names[s->cat], s->url);
        text(24, 352, b, C_PANEL2, 60);
    }
}

/* where a value sits between its parameter's ends, 0..1 (log parameters on a log scale) */
static float param_frac(int id, float v)
{
    const ParamInfo *pi = &param_info[id];
    float f = pi->log ? logf(v / pi->min) / logf(pi->max / pi->min) : (v - pi->min) / (pi->max - pi->min);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void draw_param_row(int id, int x, int y, int knob_x, int val_x, int val_chars, int selected)
{
    const ParamInfo *pi = &param_info[id];
    const float *p = synth_params(syn);
    text(x, y, param_label(id, p), selected ? C_TEXT : C_DIM, (knob_x - x - 16) / FONT_W);
    int ky = y + 8;
    char val[128];
    if (!strcmp(pi->unit, "station")) {
        int idx = (int)p[id], o = id == P_A_STATION ? 0 : 1;
        snprintf(val, sizeof val, "%s", idx >= 0 && idx < stations.count ? stations.items[idx].name : "no station");
        if (ui.pending_station[o] >= -1) strcat(val, " ...");
        disc(knob_x, ky, 6, state_col(o));
        text(val_x, y, val, selected ? C_TEXT : C_DIM, val_chars);
        return;
    }
    param_format_ctx(id, p, val, sizeof val);
    Uint32 kc = selected ? C_LEAF : C_DIM;
    if (id >= P_A_STATION && id <= P_B_STUT_CHANCE) kc = selected ? osc_col(id >= P_B_STATION) : C_DIM;
    if (pi->choices) {
        int n = (int)pi->max + 1, v = (int)p[id];
        circle(knob_x, ky, 8, C_PANEL2); /* choices: a dot orbiting to its position */
        int dx, dy;
        polar(knob_x, ky, 8, (float)v / n, &dx, &dy);
        disc(dx, dy, 3, kc);
    } else {
        knob(knob_x, ky, 9, param_frac(id, p[id]), pi->min < 0 && !pi->log, kc);
    }
    text(val_x, y, val, selected ? C_TEXT : C_DIM, val_chars);
}

static void draw_param_page(void)
{
    int rows[256];
    const char *hdrs[256];
    int n = build_rows(ui.tab, rows, hdrs);
    int *sel = &ui.sel[ui.tab], *scroll = &ui.scroll[ui.tab];
    if (*sel >= n) *sel = n - 1;
    while (*sel > 0 && rows[*sel] == ROW_HEADER) (*sel)--;
    if (rows[*sel] == ROW_HEADER) (*sel)++;
    int x = 24, knob_x = 300, val_x = 320;
    int val_chars = (636 - val_x) / FONT_W;
    int visible = 14;
    if (*sel < *scroll + 1) *scroll = *sel - 1;
    if (*sel >= *scroll + visible - 1) *scroll = *sel - visible + 2;
    if (*scroll > n - visible) *scroll = n - visible;
    if (*scroll < 0) *scroll = 0;
    for (int r = 0; r < visible && *scroll + r < n; r++) {
        int i = *scroll + r;
        int y = 40 + r * 22;
        if (rows[i] == ROW_HEADER) {
            text(x, y + 2, hdrs[i], C_LEAF, 0);
            fill(x + (int)strlen(hdrs[i]) * FONT_W + 10, y + 11, 636 - x - (int)strlen(hdrs[i]) * FONT_W - 20, 1, C_PANEL2);
            if (!strncmp(hdrs[i], "LFO ", 4)) { /* the LFO moving, live: a dot riding a short track */
                int o = hdrs[i][4] == 'B', k = hdrs[i][5] == '2', cx = 560, w = 56;
                int on = (int)synth_get_param(syn, LFO_PARAM(o, k, DEST)) != LFO_OFF &&
                         synth_get_param(syn, LFO_PARAM(o, k, DEPTH)) > 0.001f;
                fill(cx - w, y + 8, 2 * w, 7, C_BG);
                fill(cx - w, y + 11, 2 * w, 1, on ? C_DIM : C_PANEL2);
                disc(cx + (int)(synth_lfo_value(syn, o, k) * w), y + 11, 4, on ? osc_col(o) : C_PANEL2);
            }
            continue;
        }
        if (i == *sel) fill(x - 10, y - 2, 640 - x, 22, C_SEL);
        draw_param_row(rows[i], x, y, knob_x, val_x, val_chars, i == *sel);
    }
    if (n > visible) { /* scroll position as an arc */
        float a = (float)*scroll / (n - visible);
        arc(632, 200, 6, 2, 0.0f, 1.0f, C_PANEL2);
        int dx, dy;
        polar(632, 200, 6, a, &dx, &dy);
        disc(dx, dy, 2, C_DIM);
    }
}

/* ------------------------------------------------------------------ MIX page: a fader for every level */

/* one control on the MIX page: a fader, or a knob under its strip's fader (a voice's drive, the limiter's release,
 * the output ceiling) */
typedef struct { int id, meter, group, strip, knob, stage; const char *name; } MixStrip; /* stage: of the five, or -1 */
enum { MG_A, MG_B, MG_FX, MG_MASTER, MG_COUNT };
static const char *const mix_group_names[MG_COUNT] = {"VOICE A", "VOICE B", "EFFECTS", "MASTER"};
static const MixStrip mix_strips[] = { /* in Left / Right order */
    {P_A_LEVEL, MIX_A, MG_A, 0, 0, -1, "level"},           {P_A_DRIVE, -1, MG_A, 0, 1, -1, "drive"},
    {P_B_LEVEL, MIX_B, MG_B, 1, 0, -1, "level"},           {P_B_DRIVE, -1, MG_B, 1, 1, -1, "drive"},
    {P_CROSS_LEVEL, MIX_CROSS, MG_FX, 2, 0, 0, "cross"},  {P_SHAPE_LEVEL, MIX_SHAPE, MG_FX, 3, 0, 2, "shape"},
    {P_ECHO_LEVEL, MIX_ECHO, MG_FX, 4, 0, 3, "echo"},     {P_SPACE_LEVEL, MIX_SPACE, MG_FX, 5, 0, 4, "space"},
    {P_LIMIT, MIX_LIMIT, MG_MASTER, 6, 0, -1, "limit"},    {P_LIM_RELEASE, -1, MG_MASTER, 6, 1, -1, "rel"},
    {P_VOLUME, MIX_OUT, MG_MASTER, 7, 0, -1, "volume"},    {P_LIM_CEILING, -1, MG_MASTER, 7, 1, -1, "ceil"},
    {P_AUTO_LEVEL, -1, MG_MASTER, 8, 0, -1, "auto"},
};
#define MIX_N ((int)(sizeof mix_strips / sizeof mix_strips[0]))
#define MIX_W 62      /* strip width; the two voice strips are wider */
#define MIX_W_VOICE 88
#define MIX_TOP 84
#define MIX_BOT 236
#define MIX_KNOB_Y 288

static int mix_strip_x(int strip) { return 15 + (strip < 2 ? strip * MIX_W_VOICE : 2 * MIX_W_VOICE + (strip - 2) * MIX_W); }
static int mix_strip_w(int strip) { return strip < 2 ? MIX_W_VOICE : MIX_W; }

static Uint32 mix_group_col(int g) { return g == MG_A ? C_LEAF : g == MG_B ? C_AMBER : g == MG_FX ? C_WATER : C_TEXT; }



/* short value for under a control: the full one without its " dB" (shown in full for the selected control) */
static void mix_short_value(int id, char *b, int n)
{
    param_format_ctx(id, synth_params(syn), b, n);
    size_t len = strlen(b);
    if (len > 3 && !strcmp(b + len - 3, " dB")) b[len - 3] = 0;
}

static void draw_mix_fader(const MixStrip *m, int cx, int sel, const float *p)
{
    Uint32 col = mix_group_col(m->group);
    int h = MIX_BOT - MIX_TOP;
    int tx = cx - 6; /* the track, with the meter to its right */
    fill(tx - 1, MIX_TOP, 3, h, C_PANEL2);
    float f = param_frac(m->id, p[m->id]);
    int ty = MIX_BOT - (int)(f * h);
    int muted = (m->id == P_A_LEVEL || m->id == P_B_LEVEL) && voice_muted(m->id == P_B_LEVEL);
    fill(tx - 1, ty, 3, MIX_BOT - ty, muted ? C_RUST : sel ? col : mix_col(C_PANEL2, col, 0.6f));
    float unity = !strcmp(param_info[m->id].unit, "lvl") ? param_frac(m->id, 1.0f) :
                  m->id == P_VOLUME ? param_frac(m->id, 0.0f) : -1;
    if (unity >= 0) { int uy = MIX_BOT - (int)(unity * h); fill(tx - 9, uy, 4, 1, C_DIM); }
    fill(tx - 11, ty - 4, 23, 9, sel ? C_TEXT : C_DIM); /* thumb */
    fill(tx - 9, ty, 19, 1, C_BG);
    if (m->meter >= 0) {
        float mv = synth_mix_meter(syn, m->meter);
        int mx = cx + 10;
        fill(mx, MIX_TOP, 5, h, C_PANEL);
        if (m->meter == MIX_LIMIT) { /* gain reduction grows down from the top */
            int gh = (int)(fminf(mv * 2.0f, 1.0f) * h);
            fill(mx, MIX_TOP, 5, gh, C_RUST);
        } else {
            float db = 20.0f * log10f(mv + 1e-6f), mf = (db + 48.0f) / 48.0f; /* -48 .. 0 dB */
            mf = mf < 0 ? 0 : mf > 1 ? 1 : mf;
            int mh = (int)(mf * h);
            fill(mx, MIX_BOT - mh, 5, mh, db > -1.0f ? C_RUST : db > -9.0f ? C_AMBER : col);
        }
    }
    char v[24];
    mix_short_value(m->id, v, sizeof v);
    text_center(cx, MIX_BOT + 8, muted ? "muted" : v, muted ? C_RUST : sel ? C_TEXT : C_DIM, 7);
}

static void draw_mix_knob(const MixStrip *m, int cx, int sel, const float *p)
{
    Uint32 col = mix_group_col(m->group);
    int w = mix_strip_w(m->strip);
    if (sel) fill(cx - w / 2 + 2, MIX_KNOB_Y - 26, w - 4, 66, C_SEL);
    text_center(cx, MIX_KNOB_Y - 24, m->name, sel ? C_TEXT : C_DIM, 7);
    knob(cx, MIX_KNOB_Y + 4, 13, param_frac(m->id, p[m->id]), 0, sel ? col : C_DIM);
    char v[24];
    mix_short_value(m->id, v, sizeof v);
    text_center(cx, MIX_KNOB_Y + 20, v, sel ? C_TEXT : C_DIM, 7);
}

static void draw_mix_page(void)
{
    const float *p = synth_params(syn);
    if (ui.sel[TAB_MIX] < 0 || ui.sel[TAB_MIX] >= MIX_N) ui.sel[TAB_MIX] = 0;
    /* group headings over their strips */
    for (int g = 0; g < MG_COUNT; g++) {
        int first = -1, last = -1;
        for (int i = 0; i < MIX_N; i++)
            if (mix_strips[i].group == g) { if (first < 0) first = mix_strips[i].strip; last = mix_strips[i].strip; }
        int x0 = mix_strip_x(first) + 4, x1 = mix_strip_x(last) + mix_strip_w(last) - 4;
        Uint32 col = mix_group_col(g);
        text_center((x0 + x1) / 2, 36, mix_group_names[g], mix_col(C_DIM, col, 0.6f), 0);
        fill(x0, 58, x1 - x0, 1, mix_col(C_PANEL2, col, 0.4f));
    }
    const MixStrip *cur = &mix_strips[ui.sel[TAB_MIX]];
    for (int i = 0; i < MIX_N; i++) { /* the selected fader's strip is lit */
        const MixStrip *m = &mix_strips[i];
        if (m->knob) continue;
        int x = mix_strip_x(m->strip), w = mix_strip_w(m->strip), cx = x + w / 2, sel = m == cur;
        if (sel) fill(x + 2, 62, w - 4, MIX_BOT - 62 + 30, C_SEL);
        text_center(cx, 62, m->name, sel ? C_TEXT : C_DIM, 7);
        draw_mix_fader(m, cx, sel, p);
    }
    for (int i = 0; i < MIX_N; i++)
        if (mix_strips[i].knob)
            draw_mix_knob(&mix_strips[i], mix_strip_x(mix_strips[i].strip) + mix_strip_w(mix_strips[i].strip) / 2,
                          &mix_strips[i] == cur, p);
    char full[64], line_[160];
    param_format_ctx(cur->id, p, full, sizeof full);
    snprintf(line_, sizeof line_, "%s %s  %s", mix_group_names[cur->group], param_info[cur->id].label, full);
    char *val = strstr(line_, full);
    for (char *c = line_; *c && c < val; c++) *c = (char)tolower((unsigned char)*c); /* names in lower case */
    text(24, 340, line_, C_TEXT, 36);
    text_right(624, 340, "A mute  B fine", C_PANEL2, 0);
}

/* the parameter selected on a list page (VOICES, RHYTHM, ENGINE), or -1 */
static int selected_param(void)
{
    if (ui.tab == TAB_RADIO || ui.tab == TAB_MIX) return -1;
    int rows[256];
    const char *hdrs[256];
    int n = build_rows(ui.tab, rows, hdrs), sel = ui.sel[ui.tab];
    return sel >= 0 && sel < n ? rows[sel] : -1;
}

/* which of the five stages the selected setting acts on (-1: a voice or nothing) */
static int selected_stage(void)
{
    if (ui.tab == TAB_MIX) return mix_strips[ui.sel[TAB_MIX]].stage;
    if (ui.tab != TAB_ENGINE) return -1;
    int id = selected_param();
    if (id >= P_VOCODE && id <= P_CONVERSE) return 0;
    if (id == P_TONE || id == P_RESO || id == P_LOW || id == P_HIGH) return 1;
    if (id == P_DRIVE || id == P_FOLD || id == P_CRUSH) return 2;
    if (id == P_ECHO) return 3;
    if (id == P_SPACE || id == P_WIDTH || id == P_VOLUME) return 4;
    return -1;
}

static void draw_edit(void)
{
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    draw_tabs();
    if (ui.tab == TAB_RADIO) draw_radio_page();
    else if (ui.tab == TAB_MIX) draw_mix_page();
    else draw_param_page();

    /* the two voices, then the five stages, with the stage being edited ringed */
    for (int o = 0; o < 2; o++) {
        int cx = 30 + o * 64;
        disc(cx, 402, 26, C_PANEL);
        circle(cx, 402, 26, osc_col(o));
        draw_bloom(cx, 402, 6, 24, 30, o ? TAP_B : TAP_A, osc_col(o), 0);
    }
    draw_stages(168, 402, 64, 12, 26, selected_stage(), 0);
    draw_beat(440, 456);
    if (raw_on) text(16, 456, "L1/R1 page  START raw (the engine isn't heard)", C_PANEL2, 0);
    else text(16, 456, ui.tab == TAB_MIX ? "L1/R1 page  Y reset  START play" : "L1/R1 page  Y dice  START play", C_PANEL2, 0);
}

/* ================================================================== input */

static void set_pitch(void)
{
    int note = 60 + 12 * ui.octave + (int)synth_get_param(syn, P_ROOT);
    synth_note_on(syn, note);
    synth_note_off(syn, note); /* the pitch stays on the last note */
}

/* a fresh slice from the stream for one voice (0, 1) or both (-1); LIVE voices have nothing to grab */
static void do_grab(int osc)
{
    synth_grab(syn, osc);
    ui.grab_osc = osc;
    ui.grab_flash = SDL_GetTicks() + 600;
    for (int o = 0; o < 2; o++)
        if ((osc < 0 || osc == o) && (int)synth_get_param(syn, P_A_MODE + o * OSC_STRIDE) == OSC_LIVE)
            toast(osc < 0 ? "grabbed (voice %c is LIVE: nothing to grab)" : "voice %c is LIVE: nothing to grab", 'A' + o);
}

/* PLAY: the selected rhythm's voice, or both when the cross rhythm is selected */
static int play_voice(void) { return SEQ_VOICE(ui.cell) == SEQ_X ? -1 : SEQ_VOICE(ui.cell); }

static void hop(int o)
{
    int next = pool_next(o, 1);
    if (next >= 0) tune_now(o, next);
}

static void adjust_param(int id, int dir)
{
    int fine = ui.held[B_B];
    const ParamInfo *pi = &param_info[id];
    if (!strcmp(pi->unit, "station")) {
        int o = id == P_A_STATION ? 0 : 1;
        int idx = (int)synth_get_param(syn, id) + dir;
        if (idx < -1) idx = stations.count - 1;
        if (idx >= stations.count) idx = -1;
        tune_later(o, idx);
        return;
    }
    float before = synth_get_param(syn, id);
    float v = param_adjust(id, before, dir, fine);
    synth_set_param(syn, id, v);
    if ((id == P_A_MODE || id == P_B_MODE) && v != before) { /* pick a size that suits the new engine */
        static const float size_for[] = {250, 300, 5, 120};
        synth_set_param(syn, id + (P_A_SIZE - P_A_MODE), size_for[(int)v]);
    }
    if (id == P_ROOT) set_pitch();
    ui.dirty_settings = 1;
}

/* ---- rhythm cells on the PLAY page, played like hi5's pattern grid ---- */

/* start / stop the rhythms; starting begins again from the downbeat */
static void transport_toggle(void)
{
    if (sync_status()->following) { toast("the lead unit starts and stops the rhythms"); return; }
    audio_lock();
    int run = !synth_running(syn);
    if (run) synth_tap_reset(syn);
    synth_set_running(syn, run);
    audio_unlock();
    toast(run ? "rhythms playing" : "rhythms stopped (voices with a rhythm are quiet)");
}

/* mute or unmute a voice (it fades over a few ms) */
static void toggle_mute(int o)
{
    synth_set_param(syn, P_A_MUTE + o * OSC_STRIDE, !voice_muted(o));
    toast(voice_muted(o) ? "voice %c muted" : "voice %c back", 'A' + o);
    ui.dirty_settings = 1;
}

/* ------------------------------------------------------------------ RAW */

/* in or out of RAW (MENU + SELECT, from anywhere); radio scan waits while it's on */
static void raw_toggle(void)
{
    raw_on = !raw_on;
    ui.raw_to = -1;
    ui.menu_armed = 0;
    ui.screen = raw_on ? SCR_RAW : SCR_PLAY;
    if (raw_on) { scan_cancel(0); scan_cancel(1); }
    ui.dirty_settings = 1;
    toast(raw_on ? "RAW: the two streams as they are" : "RAW off: back to the engine");
}

static void raw_fade_to(float to)
{
    if (raw_fade_s[ui.raw_fade] <= 0) { raw_xf = to; ui.raw_to = -1; ui.dirty_settings = 1; }
    else ui.raw_to = to;
}

static void raw_station(int o, int dir)
{
    int n = stations.count, cur = station_param(o);
    if (n < 1) return;
    int next = cur < 0 ? 0 : ((cur + dir) % n + n) % n;
    tune_later(o, next);
    toast("voice %c: %s", 'A' + o, stations.items[next].name);
}

static void raw_button(int b)
{
    float dest = ui.raw_to >= 0 ? ui.raw_to : raw_xf;
    if (b == B_L1) raw_fade_to(0);
    if (b == B_R1) raw_fade_to(1);
    if (b == B_A) raw_fade_to(dest < 0.5f ? 1 : 0);
    if (b == B_Y) raw_fade_to(0.5f);
    if ((b == B_UP || b == B_DOWN) && (ui.held[B_L2] || ui.held[B_R2])) { /* L2 / R2 + up / down: a station */
        if (ui.held[B_L2]) raw_station(0, b == B_UP ? -1 : 1);
        if (ui.held[B_R2]) raw_station(1, b == B_UP ? -1 : 1);
    } else if (b == B_UP || b == B_DOWN) { /* the fade time */
        int f = ui.raw_fade + (b == B_UP ? 1 : -1);
        ui.raw_fade = f < 0 ? 0 : f >= RAW_FADES ? RAW_FADES - 1 : f;
        ui.dirty_settings = 1;
    }
}

/* the fader follows a held left / right (hold B to go slowly), or glides to where L1 / R1 / A / Y sent it */
static void raw_update(Uint32 now)
{
    static Uint32 last;
    float dt = last ? (now - last) / 1000.0f : 0, x = raw_xf;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    int dir = ui.screen == SCR_RAW && ui.overlay == OV_NONE ? ui.held[B_RIGHT] - ui.held[B_LEFT] : 0;
    if (dir) {
        ui.raw_to = -1;
        x += dir * dt / (ui.held[B_B] ? 8.0f : 1.5f);
    } else if (ui.raw_to >= 0) {
        float fade = raw_fade_s[ui.raw_fade], d = fade > 0 ? dt / fade : 1;
        x = x < ui.raw_to ? fminf(x + d, ui.raw_to) : fmaxf(x - d, ui.raw_to);
        if (x == ui.raw_to) ui.raw_to = -1;
    }
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    if (x != raw_xf) { raw_xf = x; ui.dirty_settings = 1; }
}

static void cell_say(int q)
{
    const float *p = synth_params(syn);
    char rate[24];
    param_format_ctx(SEQ_SHAPE(q, RATE), p, rate, sizeof rate);
    if (q >= SEQ_SA) {
        int o = SEQ_VOICE(q);
        char mode[16];
        param_format_ctx(RHYTHM_P(o, STUT_MODE), p, mode, sizeof mode);
        if (!(int)p[SEQ_PARAM(q, 0)]) toast("%s off", seq_name(q));
        else toast("%s  E(%d,%d) >%d  x%d %s", seq_name(q), (int)p[SEQ_PARAM(q, 2)], (int)p[SEQ_PARAM(q, 1)],
                   (int)p[SEQ_PARAM(q, 3)], (int)p[RHYTHM_P(o, STUT_RATCHET)], mode);
        return;
    }
    if (!(int)p[SEQ_PARAM(q, 0)]) toast(q == SEQ_X ? "cross rhythm off (cross always on)" : "%s rhythm off (always open)", seq_name(q));
    else toast("%s  E(%d,%d) >%d  %s", seq_name(q), (int)p[SEQ_PARAM(q, 2)], (int)p[SEQ_PARAM(q, 1)],
               (int)p[SEQ_PARAM(q, 3)], rate);
}

/* keep pulses and rotation inside the pattern */
static void cell_fix(int q)
{
    int st = (int)synth_get_param(syn, SEQ_PARAM(q, 1));
    if (synth_get_param(syn, SEQ_PARAM(q, 2)) > st) synth_set_param(syn, SEQ_PARAM(q, 2), (float)st);
    if (synth_get_param(syn, SEQ_PARAM(q, 3)) >= st) synth_set_param(syn, SEQ_PARAM(q, 3), (float)(st - 1));
}

/* a new random Euclidean pattern (steps, pulses, rotation) for one rhythm; whether it's on stays as it is */
static void seq_shuffle(int q)
{
    static const int lens[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 16, 16};
    int n = lens[rand() % 12], kmax = q >= SEQ_SA ? (n / 3 > 1 ? n / 3 : 1) : (n * 2 / 3 > 1 ? n * 2 / 3 : 1);
    synth_set_param(syn, SEQ_PARAM(q, 1), (float)n);
    synth_set_param(syn, SEQ_PARAM(q, 2), (float)(1 + rand() % kmax));
    synth_set_param(syn, SEQ_PARAM(q, 3), (float)(rand() % n));
}

static void cell_button(int b)
{
    static const int order[SEQ_COUNT] = {SEQ_A, SEQ_SA, SEQ_X, SEQ_SX, SEQ_SB, SEQ_B}; /* left to right on screen */
    int q = ui.cell;
    int on = SEQ_PARAM(q, 0), steps = SEQ_PARAM(q, 1), pulses = SEQ_PARAM(q, 2), rot = SEQ_PARAM(q, 3);
    int st = (int)synth_get_param(syn, steps);
    if (ui.held[B_L2]) { /* steps, rate and gate length */
        ui.l2_used = 1;
        if (b == B_UP || b == B_DOWN) { synth_set_param(syn, steps, st + (b == B_UP ? 1 : -1)); cell_fix(q); }
        if (b == B_LEFT || b == B_RIGHT) { /* jump between 1/4, 1/8, 1/16 and 1/32; right = faster */
            static const int quick[] = {0, 1, 3, 7}; /* indices of 1/32, 1/16, 1/8, 1/4 in the rate list */
            int r = SEQ_SHAPE(q, RATE), cur = (int)synth_get_param(syn, r), next = cur;
            if (b == B_RIGHT) { for (int i = 3; i >= 0; i--) if (quick[i] < cur) { next = quick[i]; break; } }
            else { for (int i = 0; i < 4; i++) if (quick[i] > cur) { next = quick[i]; break; } }
            synth_set_param(syn, r, (float)next);
        }
        if (b == B_A) { transport_toggle(); return; }
        if (q < SEQ_SA && b == B_B) { /* a voice's ring: mute it */
            if (q == SEQ_X) toast("select voice A or B to mute it");
            else toggle_mute(q);
            return;
        }
        if (q >= SEQ_SA && (b == B_X || b == B_Y || b == B_B)) { /* stutter: ratchets and mode */
            int o = SEQ_VOICE(q);
            int id = b == B_B ? RHYTHM_P(o, STUT_MODE) : RHYTHM_P(o, STUT_RATCHET);
            if (b == B_B) synth_set_param(syn, id, param_adjust(id, synth_get_param(syn, id), 1, 0));
            else synth_set_param(syn, id, synth_get_param(syn, id) + (b == B_X ? 1 : -1));
            synth_set_param(syn, on, 1);
            cell_say(q);
            ui.dirty_settings = 1;
            return;
        }
        if (b == B_X || b == B_Y) {
            int l = SEQ_SHAPE(q, LENGTH);
            synth_set_param(syn, l, synth_get_param(syn, l) + (b == B_X ? 0.05f : -0.05f));
            char v[16];
            param_format_ctx(l, synth_params(syn), v, sizeof v);
            toast("%s gate length %s", seq_name(q), v);
            ui.dirty_settings = 1;
            return;
        }
    } else {
        int pos = 0;
        for (int i = 0; i < SEQ_COUNT; i++) if (order[i] == q) pos = i;
        switch (b) {
        case B_LEFT: ui.cell = order[(pos + SEQ_COUNT - 1) % SEQ_COUNT]; break;
        case B_RIGHT: ui.cell = order[(pos + 1) % SEQ_COUNT]; break;
        case B_UP: synth_set_param(syn, pulses, synth_get_param(syn, pulses) + 1); synth_set_param(syn, on, 1); break;
        case B_DOWN: synth_set_param(syn, pulses, synth_get_param(syn, pulses) - 1); break;
        case B_A: synth_set_param(syn, on, !(int)synth_get_param(syn, on)); break;
        case B_B: { /* fewer pulses, wrapping back to all of them (as on hi5) */
            int k = (int)synth_get_param(syn, pulses);
            synth_set_param(syn, pulses, (float)(k > 1 ? k - 1 : st));
            synth_set_param(syn, on, 1);
            break;
        }
        case B_X: synth_set_param(syn, rot, (float)(((int)synth_get_param(syn, rot) + 1) % st)); break;
        case B_Y: seq_shuffle(q); synth_set_param(syn, on, 1); break; /* a new random Euclidean pattern */
        default: return;
        }
    }
    cell_say(ui.cell);
    ui.dirty_settings = 1;
}

static void dice_section(void)
{
    int rows[256];
    const char *hdrs[256];
    int n = build_rows(ui.tab, rows, hdrs);
    int sel = ui.sel[ui.tab];
    if (sel >= n) return;
    int start = sel, end = sel;
    while (start > 0 && rows[start - 1] != ROW_HEADER) start--;
    while (end + 1 < n && rows[end + 1] != ROW_HEADER) end++;
    int ids[64], k = 0;
    for (int i = start; i <= end && k < 64; i++)
        if (rows[i] >= 0) ids[k++] = rows[i];
    synth_randomize(syn, ids, k, 1.0f);
    const char *name = start > 0 && rows[start - 1] == ROW_HEADER ? hdrs[start - 1] : "section";
    toast("rolled the dice on %s", name);
    ui.dirty_settings = 1;
}

/* EDIT: the voice the selected setting belongs to, or -1 (both) */
static int edit_voice(void)
{
    if (ui.tab == TAB_MIX) { int g = mix_strips[ui.sel[TAB_MIX]].group; return g == MG_A ? 0 : g == MG_B ? 1 : -1; }
    int id = selected_param();
    if (id < P_A_STATION || id > P_B_STUT_CHANCE) return -1;
    return id >= P_B_STATION;
}

static void edit_nav(int b)
{
    if (ui.tab == TAB_MIX) {
        if (b == B_LEFT) ui.sel[TAB_MIX] = (ui.sel[TAB_MIX] + MIX_N - 1) % MIX_N;
        if (b == B_RIGHT) ui.sel[TAB_MIX] = (ui.sel[TAB_MIX] + 1) % MIX_N;
        if (b == B_UP || b == B_DOWN) adjust_param(mix_strips[ui.sel[TAB_MIX]].id, b == B_UP ? 1 : -1);
        return;
    }
    if (ui.tab == TAB_RADIO) {
        int n = stations.count;
        if (b == B_UP && n) ui.radio_sel = (ui.radio_sel + n - 1) % n;
        if (b == B_DOWN && n) ui.radio_sel = (ui.radio_sel + 1) % n;
        if (b == B_LEFT) ui.radio_sel = ui.radio_sel >= 13 ? ui.radio_sel - 13 : 0;
        if (b == B_RIGHT && n) ui.radio_sel = ui.radio_sel + 13 < n ? ui.radio_sel + 13 : n - 1;
        ui.confirm_delete = 0;
        return;
    }
    int rows[256];
    const char *hdrs[256];
    int n = build_rows(ui.tab, rows, hdrs);
    int *sel = &ui.sel[ui.tab];
    if (b == B_UP || b == B_DOWN) {
        int d = b == B_UP ? -1 : 1;
        do { *sel = (*sel + d + n) % n; } while (rows[*sel] == ROW_HEADER);
        return;
    }
    if (rows[*sel] >= 0) adjust_param(rows[*sel], b == B_LEFT ? -1 : 1);
}

static const char *const button_names[B_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L1", "R1",
                                                   "L2", "R2", "SELECT", "START", "MENU", "F1", "F2"};

/* where a press lands, for the event log: the screen, page and selection */
static void log_press(int b)
{
    char where[96], mods[32] = "";
    if (ui.overlay == OV_KEYBOARD) snprintf(where, sizeof where, "keyboard");
    else if (ui.overlay == OV_SCANMENU || ui.overlay == OV_SCANRESULTS) snprintf(where, sizeof where, "scan");
    else if (ui.screen == SCR_PLAY) snprintf(where, sizeof where, "PLAY, %s ring", seq_name(ui.cell));
    else if (ui.screen == SCR_RAW) snprintf(where, sizeof where, "RAW, fader %.2f", raw_xf);
    else if (ui.tab == TAB_MIX) snprintf(where, sizeof where, "MIX, %s", param_info[mix_strips[ui.sel[TAB_MIX]].id].key);
    else if (ui.tab == TAB_RADIO) snprintf(where, sizeof where, "RADIO, station %d", ui.radio_sel + 1);
    else {
        int id = selected_param();
        snprintf(where, sizeof where, "%s, %s", tab_names[ui.tab], id >= 0 ? param_info[id].key : "-");
    }
    if (ui.held[B_L2] && b != B_L2) strcat(mods, " +L2");
    if (ui.held[B_R2] && b != B_R2) strcat(mods, " +R2");
    ev("press %s%s  (%s)", button_names[b], mods, where);
}

static void on_press(int b)
{
    if (!ui.repeating) log_press(b);
    if (b == B_START && ui.held[B_MENU]) { /* MENU + START, from anywhere: a clean slate */
        synth_clean(syn);
        ui.menu_armed = 0;
        ui.dirty_settings = 1;
        toast("clean slate: both voices live, no effects");
        return;
    }
    if (b == B_SELECT && ui.held[B_MENU]) { raw_toggle(); return; } /* MENU + SELECT, from anywhere: RAW in / out */
    Uint32 now = SDL_GetTicks();
    if (ui.overlay == OV_KEYBOARD) { kb_input(b); return; }
    if (ui.overlay == OV_SCANMENU) { scan_menu_input(b); return; }
    if (ui.overlay == OV_SCANRESULTS) { scan_results_input(b); return; }
    if (ui.overlay == OV_UPDATE) { update_input(b); return; }

    /* F1 and F2, from any screen: new Euclidean patterns for every rhythm (each stays on or off as it was), and the
     * dice on every voice and engine setting (as R2 + R1) */
    if (b == B_F1) {
        for (int q = 0; q < SEQ_COUNT; q++) seq_shuffle(q);
        toast("new patterns");
        ui.dirty_settings = 1;
        return;
    }
    if (b == B_F2) { synth_dice(syn, DICE_SOUND); toast("new sound"); ui.dirty_settings = 1; return; }

    if (b == B_MENU) {
        if (ui.menu_armed && now - ui.menu_armed < 1500) { ui.running = 0; return; }
        ui.menu_armed = now;
        toast("MENU again: leave  hold MENU + START: clean, + SELECT: raw");
        return;
    }
    if (ui.screen == SCR_PLAY && ui.held[B_R2] && b != B_R2) { /* sound gestures and dice */
        if (b == B_L1) { synth_dice(syn, DICE_RHYTHM); toast("new rhythms"); ui.dirty_settings = 1; }
        if (b == B_R1) { synth_dice(syn, DICE_SOUND); toast("new sound"); ui.dirty_settings = 1; }
        if (b == B_SELECT) { hop(0); hop(1); toast("hopping stations"); }
        return; /* d-pad and face buttons act continuously while held */
    }
    if (b == B_SELECT) { tap_tempo(); return; }
    if (b == B_START) {
        ui.screen = ui.screen != SCR_EDIT ? SCR_EDIT : raw_on ? SCR_RAW : SCR_PLAY;
        return;
    }
    if (ui.screen == SCR_RAW) { raw_button(b); return; }

    if (ui.screen == SCR_PLAY) {
        if (b == B_L2) { ui.l2_used = 0; return; }
        if (b == B_R2) return;
        if (b == B_L1 && ui.octave > -3) { ui.octave--; set_pitch(); ui.dirty_settings = 1; return; }
        if (b == B_R1 && ui.octave < 3) { ui.octave++; set_pitch(); ui.dirty_settings = 1; return; }
        cell_button(b);
        return;
    }

    /* EDIT */
    if (b == B_L1) { ui.tab = (ui.tab + TAB_COUNT - 1) % TAB_COUNT; ui.confirm_delete = 0; return; }
    if (b == B_R1) { ui.tab = (ui.tab + 1) % TAB_COUNT; ui.confirm_delete = 0; return; }
    if (b == B_UP || b == B_DOWN || b == B_LEFT || b == B_RIGHT) { edit_nav(b); return; }

    if (ui.tab == TAB_RADIO) {
        int n = stations.count;
        Station *s = n ? &stations.items[ui.radio_sel] : NULL;
        if (b == B_A && s) { tune_now(0, ui.radio_sel); toast("voice A hears %s", s->name); }
        if (b == B_B && s) { tune_now(1, ui.radio_sel); toast("voice B hears %s", s->name); }
        if (b == B_Y) open_keyboard(KB_ADD_URL, "http://");
        if (b == B_R2) ui.overlay = OV_SCANMENU;
        if (b == B_L2 && s) {
            s->cat = (s->cat + 1) % 4;
            save_stations();
            toast("%s is %s", s->name, s->cat ? category_names[s->cat] : "uncategorised");
        }
        if (b == B_X && s) {
            if (ui.confirm_delete) {
                char name[96];
                snprintf(name, sizeof name, "%s", s->name);
                delete_station(ui.radio_sel);
                toast("let go of %s", name);
                ui.confirm_delete = 0;
            } else {
                ui.confirm_delete = 1;
            }
        }
        return;
    }
    if (b == B_L2 || b == B_X) do_grab(edit_voice());
    if (b == B_A && ui.tab == TAB_MIX) { /* mute the voice of the selected control */
        int g = mix_strips[ui.sel[TAB_MIX]].group;
        if (g == MG_A || g == MG_B) toggle_mute(g == MG_B);
    }
    if (b == B_Y && ui.tab == TAB_MIX) { /* back to unity / the default */
        const MixStrip *m = &mix_strips[ui.sel[TAB_MIX]];
        synth_set_param(syn, m->id, param_info[m->id].def);
        toast("%s %s reset", mix_group_names[m->group], m->name);
        ui.dirty_settings = 1;
    } else if (b == B_Y) dice_section();
}

static void on_release(int b)
{
    if (ui.screen == SCR_PLAY && ui.overlay == OV_NONE && b == B_L2 && !ui.l2_used) do_grab(play_voice()); /* a tap of L2 */
}

static void held_keys_update(Uint32 now)
{
    static const int keys[8] = {B_UP, B_DOWN, B_LEFT, B_RIGHT, B_X, B_B, B_Y, B_A};
    for (int i = 0; i < 8; i++) {
        int b = keys[i];
        if (!ui.held[b]) continue;
        if (ui.screen == SCR_PLAY && ui.overlay == OV_NONE && ui.held[B_R2]) {
            /* real-time sound: d-pad = filter (up brighter) / morph (right toward B), X/B = fold, Y/A = space */
            float step = ui.held[B_L2] ? 0.004f : 0.015f;
            int id = -1, dir = 0;
            switch (b) {
            case B_UP: id = P_TONE; dir = 1; break;
            case B_DOWN: id = P_TONE; dir = -1; break;
            case B_LEFT: id = P_MORPH; dir = -1; break;
            case B_RIGHT: id = P_MORPH; dir = 1; break;
            case B_X: id = P_FOLD; dir = 1; break;
            case B_B: id = P_FOLD; dir = -1; break;
            case B_Y: id = P_SPACE; dir = -1; break;
            case B_A: id = P_SPACE; dir = 1; break;
            }
            synth_set_param(syn, id, synth_get_param(syn, id) + dir * step);
            ui.dirty_settings = 1;
            continue;
        }
        if (ui.screen == SCR_PLAY && i >= 4 && ui.overlay == OV_NONE) { /* face buttons repeat on PLAY only */
        } else if (i >= 4) {
            continue;
        }
        Uint32 held_for = now - ui.held_since[b];
        Uint32 interval = ui.screen == SCR_PLAY ? 140 : held_for > 1500 ? 30 : 70;
        if (held_for > 320 && now - ui.last_repeat[b] >= interval) {
            ui.last_repeat[b] = now;
            ui.repeating = 1; /* a held key repeating: not logged as a new press */
            on_press(b);
            ui.repeating = 0;
        }
    }
}

/* ================================================================== periodic logic */

/* ------------------------------------------------------------------ the sound device
 * The speaker and headphones, or a USB sound card or DAC while one is plugged in (RS_AUDIO=internal keeps to the
 * speaker; RS_AUDIO=<part of a name> picks a device, see log.txt). The synth always makes 16-bit stereo at
 * audio_rate; SDL converts for a device that wants something else. */
static int audio_open_ok, audio_buffer = 1024, audio_latency = -1;
static char adev_name[128]; /* "" is the speaker and headphones */

static int is_usb_name(const char *n)
{
    return n && (strcasestr(n, "usb") || strcasestr(n, "uac") || strcasestr(n, "dac"));
}

static const char *pick_output(void)
{
    const char *want = getenv("RS_AUDIO");
    if (want && !strcasecmp(want, "internal")) return NULL;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (!name) continue;
        if (want && *want && strcasecmp(want, "auto") ? strcasestr(name, want) != NULL : is_usb_name(name)) return name;
    }
    return NULL;
}

/* (re)open the output and start it; NULL is the system's own */
static int audio_open(const char *name)
{
    char keep[128];
    snprintf(keep, sizeof keep, "%s", name ? name : ""); /* the name may belong to SDL's list, which reopening changes */
    name = *keep ? keep : NULL;
    pthread_mutex_lock(&dev_mx);
    if (adev) {
        SDL_CloseAudioDevice(adev);
        adev = 0;
    }
    aud_calls = 0;
    aud_first_ms = 0;
    aud_epoch++;
    /* the driver holds about a buffer before it's heard; RS_SYNC_LATENCY (frames) lines this unit up with other
     * apps by ear if needed. Each device starts the audio clock afresh. */
    aclock_init(audio_rate, audio_latency >= 0 ? audio_latency : audio_buffer);
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = audio_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)audio_buffer;
    want.callback = audio_cb;
    adev = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
    if (!adev && name) {
        fprintf(stderr, "audio: could not open %s (%s), using the speaker\n", name, SDL_GetError());
        name = NULL;
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    audio_open_ok = adev != 0;
    if (adev) {
        snprintf(adev_name, sizeof adev_name, "%s", name ? name : "");
        perf_audio_open(audio_rate, have.samples, name);
        /* which way the sound goes (on MustardOS: PipeWire, or ALSA through PipeWire's plugin), and what the
         * device took */
        perf_log("AUDIO", "SDL %s driver: %d Hz, %d channels, format 0x%x, %d frames a buffer", SDL_GetCurrentAudioDriver(),
                 have.freq, (int)have.channels, (unsigned)have.format, (int)have.samples);
        SDL_PauseAudioDevice(adev, 0);
    } else
        fprintf(stderr, "audio: %s\n", SDL_GetError());
    pthread_mutex_unlock(&dev_mx);
    return audio_open_ok;
}

/* a USB device came or went: follow it (unless RS_AUDIO names one) */
static void audio_hotplug(void)
{
    const char *want = getenv("RS_AUDIO");
    if (want && *want && strcasecmp(want, "auto")) return;
    const char *name = pick_output();
    if (audio_open_ok && !strcmp(name ? name : "", adev_name)) return;
    ev("audio device change: %s", name ? name : "speaker / headphones");
    audio_open(name);
    toast("audio: %.40s", name ? name : "speaker / headphones");
}

/* A sound driver can get stuck feeding the hardware at half speed after a hiccup, and stay that way (heard as
 * constant dropouts) until the device is reopened; the Miyoo's did. Every 2 s, if the audio has fallen clearly
 * behind real time, close and reopen it: a moment's silence instead of dropouts for the rest of the session. */
static void reopen_audio(void)
{
    char name[128];
    snprintf(name, sizeof name, "%s", adev_name);
    if (!audio_open(*name ? name : NULL)) ev("  could not reopen the sound device: %s", SDL_GetError());
}

static void audio_watch(Uint32 now)
{
    static Uint32 last_check, last_reopen;
    static double last_missing = -1;
    static unsigned last_epoch;
    if (!audio_open_ok || now - last_check < 2000) return;
    last_check = now;
    if (aud_epoch != last_epoch) { last_epoch = aud_epoch; last_missing = -1; }
    if (aud_first_ms <= 0 || aud_buf_ms <= 0) return;
    double missing = (mono_ms() - aud_first_ms) / aud_buf_ms - (aud_calls - 20);
    if (last_missing >= 0 && missing - last_missing >= 8 && (!last_reopen || now - last_reopen > 10000)) {
        ev("AUDIO STALLED: %.0f buffers missing in 2 s, reopening the sound device", missing - last_missing);
        reopen_audio();
        last_reopen = now;
        ev("  sound device reopened");
        return;
    }
    last_missing = missing;
}

/* settings as they settle (a held fader is one line, not hundreds), station state changes, late audio */
static void log_watch(Uint32 now)
{
    static float last[P_COUNT], from[P_COUNT];
    static Uint32 changed_at[P_COUNT];
    static unsigned char pending[P_COUNT];
    static int started, last_state[2] = {-1, -1};
    static unsigned last_late;
    const float *p = synth_params(syn);
    if (!started) { memcpy(last, p, sizeof last); started = 1; }
    for (int i = 0; i < P_COUNT; i++) {
        if (p[i] != last[i]) {
            if (!pending[i]) { from[i] = last[i]; pending[i] = 1; changed_at[i] = 0; }
            if (!changed_at[i]) changed_at[i] = now;
            last[i] = p[i];
            changed_at[i] = now;
        } else if (pending[i] && now - changed_at[i] > 400) {
            float tmp[P_COUNT];
            char a[48], b[48];
            memcpy(tmp, p, sizeof tmp);
            tmp[i] = from[i];
            param_format_ctx(i, tmp, a, sizeof a);
            param_format_ctx(i, p, b, sizeof b);
            ev("  set %s: %s -> %s", param_info[i].key, a, b);
            pending[i] = 0;
        }
    }
    for (int o = 0; o < 2; o++) {
        int stt = stream_state(st[o]);
        if (stt != last_state[o]) {
            static const char *const names[] = {"idle", "connecting", "buffering", "playing", "error"};
            char msg[160];
            stream_status(st[o], msg, sizeof msg);
            ev("  voice %c stream %s: %s", 'A' + o, names[stt], msg);
            last_state[o] = stt;
        }
    }
    static int last_octave = 99;
    if (ui.octave != last_octave) {
        if (last_octave != 99) ev("  set octave: %+d -> %+d", last_octave, ui.octave);
        last_octave = ui.octave;
    }
    unsigned late = aud_late;
    if (late != last_late) {
        ev("AUDIO LATE x%u (longest gap %.0f ms)", late - last_late, aud_recent_gap_ms);
        last_late = late;
    }
    aud_recent_gap_ms = 0;
}

/* The Brick Pro's left stick sweeps the stations by hand, like a tuning dial: across for voice A (right = further
 * down the list), up and down for voice B (down = further down the list, as on the RADIO page). The further it is
 * pushed, the further from the voice's own station, up to half the list either way; near the centre it moves a
 * station at a time. Let go, the voice glides home. It is radio scan played by hand, so nothing is saved. -1 .. 1,
 * set by on_axis. */
static float sweep[2];
/* The right stick plays the sound by hand and springs back when let go: across morphs toward A (left) or B (right),
 * up and down opens or closes the filter. In RAW, across moves the crossfader the same way. */
static float rstick[2];

static void rstick_apply(void)
{
    int raw = raw_on;
    synth_set_hand(syn, HAND_MORPH, raw ? 0 : rstick[0]);
    synth_set_hand(syn, HAND_FILTER, raw ? 0 : -rstick[1]); /* up is negative */
    raw_hand = raw ? rstick[0] : 0;
}

/* how many stations from home the stick has a voice: off is the offset now, kept until the stick is well past the
 * next station's place so a stick resting on a boundary doesn't flip between two */
static int sweep_offset(float x, int n, int off)
{
    int span = (n - 1) / 2;
    if (span < 1) span = 1;
    float pos = x * fabsf(x) * span; /* fine near the centre, far at the edge */
    return fabsf(pos - off) < 0.65f ? off : (int)lroundf(pos);
}

/* Radio scan: a voice's LFO set to RADIO SCAN picks stations from a window of the list around the voice's own
 * station (depth: from a station or two either side up to the whole list), and the voice crossfades into each one
 * once it is playing. With scan off it glides back home. One switch at a time; a station that fails is skipped.
 * The left stick (sweep) wins over the LFO while it is pushed, and works in RAW too, where the LFO waits. */
static void radio_scan(Uint32 now)
{
    static int target_of[2], bad[2] = {-1, -1}, hand_off[2], by_hand[2];
    static Uint32 started[2], bad_until[2], next_try[2], hand_moved[2];
    int n = stations.count;
    for (int o = 0; o < 2; o++) {
        /* Until its fade begins, nothing reads the next stream, and a stream only drops the old station's audio when
         * it is read. Read nothing from it here, so the drop happens now: otherwise the old audio stays in its
         * buffer (a full buffer stalls the next station), and the fade's first read throws away what it buffered. */
        if (xf_state[o] < 2) {
            float none[2];
            stream_read(st_next[o], none, 0);
        }
        float v = 0, depth = 0, rate = 0;
        for (int k = 0; k < 2 && !raw_on; k++)
            if ((int)synth_get_param(syn, LFO_PARAM(o, k, DEST)) == LFO_SCAN) {
                float d = synth_get_param(syn, LFO_PARAM(o, k, DEPTH));
                if (d > depth) { depth = d; v = synth_lfo_value(syn, o, k); rate = synth_lfo_hz(syn, o, k); }
            }
        int base = station_param(o), scanning = depth > 0.01f, hand = sweep[o] != 0.0f;
        if (base < 0 || base >= n || n < 2 || ui.pending_station[o] >= -1) continue;
        int off = hand ? sweep_offset(sweep[o], n, hand_off[o]) : 0;
        if (off != hand_off[o]) {
            hand_off[o] = off;
            hand_moved[o] = now;
            if (hand) toast("voice %c: %s", 'A' + o, stations.items[((base + off) % n + n) % n].name);
        }
        if (hand) by_hand[o] = 1;
        else if (by_hand[o] && ((xf_state[o] == 0 && ui.tuned[o] == base) || scanning))
            by_hand[o] = 0; /* home again, or the LFO's scan takes over */
        if (raw_on && !by_hand[o]) continue; /* in RAW only the stick moves a voice */
        int target = base;
        if (hand)
            target = ((base + off) % n + n) % n;
        else if (scanning) {
            int span = (int)lroundf(depth * (n - 1) * 0.5f);
            if (span < 1) span = 1;
            target = ((base + (int)lroundf(v * span)) % n + n) % n;
        }
        switch (xf_state[o]) {
        case 0:
            if (target == ui.tuned[o] || now < next_try[o] || (target == bad[o] && now < bad_until[o])) break;
            if (hand && now - hand_moved[o] < 200) break; /* sweeping past: wait until the stick rests on one */
            stream_play(st_next[o], stations.items[target].url);
            target_of[o] = target;
            started[o] = now;
            xf_state[o] = 1;
            ev("  voice %c %s %s", 'A' + o, hand ? "sweeps to" : scanning ? "scans to" : "returns to",
               stations.items[target].name);
            break;
        case 1:
            if (target != target_of[o] && by_hand[o]) { /* the stick has moved on, or let go: drop this one */
                stream_stop(st_next[o]);
                xf_state[o] = 0;
                break;
            }
            if (stream_state(st_next[o]) == ST_PLAYING && stream_buffer_seconds(st_next[o]) > 1.0f) {
                float fade = hand ? 0.4f : scanning ? 0.6f / rate : 1.5f; /* slower scans, longer fades */
                fade = fade < 0.3f ? 0.3f : fade > 4.0f ? 4.0f : fade;
                xf_pos[o] = 0;
                xf_step[o] = 1.0f / (fade * audio_rate);
                xf_state[o] = 2;
            } else if (stream_state(st_next[o]) == ST_ERROR || now - started[o] > 12000) {
                ev("  voice %c scan skips %s (it did not play)", 'A' + o, stations.items[target_of[o]].name);
                stream_stop(st_next[o]);
                bad[o] = target_of[o];
                bad_until[o] = now + 60000;
                xf_state[o] = 0;
            }
            break;
        case 3: { /* faded in: the new station becomes the voice's stream */
            audio_lock();
            Stream *t = st[o];
            st[o] = st_next[o];
            st_next[o] = t;
            xf_state[o] = 0;
            audio_unlock();
            stream_stop(st_next[o]);
            ui.tuned[o] = target_of[o];
            next_try[o] = now + 250;
            break;
        }
        }
    }
}

static void logic_update(Uint32 now)
{
    log_watch(now);
    update_watch(now);
    radio_scan(now); /* in RAW only the left stick's sweep */
    rstick_apply();
    raw_update(now);
    audio_watch(now);
    for (int o = 0; o < 2; o++)
        if (ui.pending_station[o] >= -1 && now >= ui.pending_at[o]) tune_now(o, ui.pending_station[o]);
    if (ui.scan_busy && scan_done) {
        ui.scan_busy = 0;
        ui.scan_results = scan_tmp;
        ui.scan_sel = ui.scan_scroll = 0;
    }
    /* save once the settings have stopped changing for a few seconds, so tweaking doesn't keep writing to the card */
    static Uint32 last_change;
    static unsigned last_hash;
    unsigned h = 2166136261u;
    const float *p = synth_params(syn);
    for (int i = 0; i < P_COUNT; i++) { unsigned v; memcpy(&v, &p[i], sizeof v); h = (h ^ v) * 16777619u; }
    h = (h ^ (unsigned)ui.octave) * 16777619u;
    float rx = raw_xf;
    unsigned rv;
    memcpy(&rv, &rx, sizeof rv);
    h = (h ^ rv ^ (unsigned)raw_on << 30 ^ (unsigned)ui.raw_fade << 26) * 16777619u;
    if (h != last_hash) { last_hash = h; last_change = now; }
    if (ui.dirty_settings && now - last_change > 4000) write_settings(1);
    audio_report(now);
}

/* ================================================================== presentation */

/* the 640x480 page, scaled to fill the screen and kept in proportion (1.6x on the Brick's 1024x768) */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_Rect dst_rect;
static int rotate; /* RS_ROTATE=90/180/270, for a firmware whose screen is turned */

/* RS_SCREEN_WAKE: a command to run once the window is on show. MustardOS's mux_launch.sh points it at
 * wake_screen.sh: on the Brick Pro the screen can stay dark when an app opens its window there, until waking from
 * sleep sets the display up again, so the script does what waking does. Run in the background (a double fork, so
 * nothing is left to reap), never waited for. As in Columbia Street, HI5 and Jaffedrone. */
#define SCREEN_WAKE_FRAME 10 /* about a third of a second of frames */
static void screen_wake(void)
{
    const char *cmd = getenv("RS_SCREEN_WAKE");
    if (!cmd || !*cmd) return;
    perf_log("SCREEN", "waking the screen: %s", cmd);
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void present(void)
{
    static int frames;
    SDL_UpdateTexture(tex, NULL, screen->pixels, screen->pitch);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopyEx(ren, tex, NULL, &dst_rect, rotate, NULL, SDL_FLIP_NONE);
    SDL_RenderPresent(ren);
    if (++frames == SCREEN_WAKE_FRAME) screen_wake();
}

static int video_init(void)
{
#ifdef RS_DEVICE
    int windowed = 0;
#else
    int windowed = 1; /* on a computer: a window the size of the Brick's screen */
#endif
    if (getenv("RS_WINDOWED")) windowed = atoi(getenv("RS_WINDOWED")) != 0;
    SDL_DisplayMode dm;
    int sw = 1024, sh = 768;
    if (!windowed && SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) sw = dm.w, sh = dm.h;
    win = SDL_CreateWindow("RadioScan", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sw, sh,
                           SDL_WINDOW_SHOWN | (windowed ? 0 : SDL_WINDOW_FULLSCREEN));
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H) : NULL;
    if (!tex) {
        fprintf(stderr, "video: %s\n", SDL_GetError());
        return 0;
    }
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(ren, &ri);
    SDL_GetRendererOutputSize(ren, &sw, &sh);
    fprintf(stderr, "radioscan: screen %dx%d, drawn by %s, %d cores\n", sw, sh, ri.name, (int)sysconf(_SC_NPROCESSORS_ONLN));
    rotate = getenv("RS_ROTATE") ? atoi(getenv("RS_ROTATE")) : 0;
    int side = rotate == 90 || rotate == 270;
    float scale = side ? fminf((float)sw / SCREEN_H, (float)sh / SCREEN_W) : fminf((float)sw / SCREEN_W, (float)sh / SCREEN_H);
    dst_rect.w = (int)(SCREEN_W * scale + 0.5f), dst_rect.h = (int)(SCREEN_H * scale + 0.5f);
    dst_rect.x = (sw - dst_rect.w) / 2, dst_rect.y = (sh - dst_rect.h) / 2;
    return 1;
}

/* ================================================================== input
 * The buttons, as SDL sees them. The d-pad is hat 0 everywhere.
 *   TrimUI OS (NextUI and MinUI agree): the TrimUI's own controller: B 0, A 1, Y 2, X 3, L1 4, R1 5, SELECT 6,
 *     START 7, MENU 8; L2 and R2 are axes 2 and 5; the Brick Pro's left stick is axes 0, 1.
 *   MustardOS: its "muOS-Keys" controller, numbered as in MustardOS's own controller maps (device/tui-brick*),
 *     which differ between the Brick and the Brick Pro. L2 and R2 are buttons; the Brick Pro's left stick is
 *     axes 0, 1. One build serves both devices, so which one it is comes from the controller (product 2: Brick,
 *     3: Brick Pro), else from the board MustardOS names.
 * The Brick Pro's left stick sweeps the stations (see radio_scan), its right stick plays morph and the filter (see
 * rstick_apply), and its clicks are the buttons after MENU, read as F1 and F2. RS_JOYMAP=A=1,B=0,... sets another firmware's button numbers
 * (for every controller); a button that isn't mapped is noted in log.txt when pressed. As in Columbia Street, HI5
 * and Jaffedrone. */
enum { PAD_TRIMUI, PAD_MUOS_BRICK, PAD_MUOS_BRICKPRO, PAD_KINDS };
static const char *const pad_names[PAD_KINDS] = {"TrimUI OS", "MustardOS (Brick)", "MustardOS (Brick Pro)"};
/* each pad's button numbers, in the order of the B_ enum. F1 and F2 are the two buttons SDL numbers after MENU (L3 and
 * R3 in other firmwares' maps; the Brick has no sticks to click). */
static const signed char pad_buttons[PAD_KINDS][B_COUNT] = {
    /*                     UP  DN  LT  RT  A  B  X  Y  L1  R1  L2  R2  SEL STA MENU F1  F2 */
    [PAD_TRIMUI]        = {-1, -1, -1, -1, 1, 0, 3, 2, 4,  5,  -1, -1, 6,  7,  8,   9,  10},
    [PAD_MUOS_BRICK]    = {-1, -1, -1, -1, 5, 4, 6, 7, 8,  9,  10, 11, 12, 13, 14,  15, 16},
    [PAD_MUOS_BRICKPRO] = {-1, -1, -1, -1, 6, 5, 7, 8, 9,  10, 11, 12, 13, 14, 15,  16, 17},
};
/* what each pad's axes do */
enum { AX_NONE, AX_L2, AX_R2, AX_LX, AX_LY, AX_RX, AX_RY };
#define PAD_AXES 6
static const unsigned char pad_axes[PAD_KINDS][PAD_AXES] = {
    [PAD_TRIMUI] = {AX_LX, AX_LY, AX_L2, AX_RX, AX_RY, AX_R2},
    [PAD_MUOS_BRICK] = {AX_NONE},
    [PAD_MUOS_BRICKPRO] = {AX_LX, AX_LY, AX_RX, AX_RY},
};
/* joy_map[pad][SDL button]: a B_, or B_NONE */
static int joy_map[PAD_KINDS][32];

static void joy_map_init(void)
{
    for (int p = 0; p < PAD_KINDS; p++) {
        for (int i = 0; i < 32; i++) joy_map[p][i] = B_NONE;
        for (int b = 0; b < B_COUNT; b++)
            if (pad_buttons[p][b] >= 0) joy_map[p][(int)pad_buttons[p][b]] = b;
    }
    const char *m = getenv("RS_JOYMAP");
    while (m && *m) {
        char key[16];
        int num;
        if (sscanf(m, "%15[^=]=%d", key, &num) == 2 && num >= 0 && num < 32)
            for (int b = 0; b < B_COUNT; b++)
                if (!strcasecmp(key, button_names[b]))
                    for (int p = 0; p < PAD_KINDS; p++) {
                        for (int i = 0; i < 32; i++)
                            if (joy_map[p][i] == b) joy_map[p][i] = B_NONE;
                        joy_map[p][num] = b;
                    }
        m = strchr(m, ',');
        if (m) m++;
    }
}

/* the open controllers, by SDL instance id, and which kind each is */
#define MAX_PADS 8
static struct {
    SDL_JoystickID id;
    int kind;
} pads[MAX_PADS];
static int npads;

/* vendor and product from the GUID (Linux: bytes 4-5 and 8-9, little-endian), which every SDL2 has, unlike
 * SDL_JoystickGetVendor */
static int pad_vendor(SDL_Joystick *j)
{
    SDL_JoystickGUID g = SDL_JoystickGetGUID(j);
    return g.data[4] | g.data[5] << 8;
}
static int pad_product(SDL_Joystick *j)
{
    SDL_JoystickGUID g = SDL_JoystickGetGUID(j);
    return g.data[8] | g.data[9] << 8;
}

/* MustardOS's controller: named muOS-Keys, vendor 0x756d ("mu"); product 2 on the Brick and Brick Hammer, 3 on
 * the Brick Pro. Otherwise the board MustardOS names (tui-brick, tui-brick-pro), else its button count. */
static int pad_kind(SDL_Joystick *j)
{
    const char *name = SDL_JoystickName(j);
    if (!(name && strcasestr(name, "muos")) && pad_vendor(j) != 0x756d) return PAD_TRIMUI;
    switch (pad_product(j)) {
    case 2: return PAD_MUOS_BRICK;
    case 3: return PAD_MUOS_BRICKPRO;
    }
    char board[64] = "";
    FILE *f = fopen("/opt/muos/device/config/board/name", "r");
    if (f) {
        if (!fgets(board, sizeof board, f)) board[0] = 0;
        fclose(f);
    }
    if (board[0]) return strstr(board, "pro") ? PAD_MUOS_BRICKPRO : PAD_MUOS_BRICK;
    return SDL_JoystickNumButtons(j) > 17 ? PAD_MUOS_BRICKPRO : PAD_MUOS_BRICK;
}

/* which map an event uses, or -1 to ignore it: when MustardOS's controller is there, it alone is read, so a press
 * can't arrive twice through the TrimUI's own controller underneath it */
static int event_pad(SDL_JoystickID id)
{
    int muos = 0, kind = -1;
    for (int i = 0; i < npads; i++) {
        muos |= pads[i].kind != PAD_TRIMUI;
        if (pads[i].id == id) kind = pads[i].kind;
    }
    if (kind < 0) return PAD_TRIMUI; /* not seen opening: as before */
    return muos && kind == PAD_TRIMUI ? -1 : kind;
}

static void open_joysticks(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        if (!j) continue;
        SDL_JoystickID id = SDL_JoystickInstanceID(j);
        int known = 0;
        for (int k = 0; k < npads; k++) known |= pads[k].id == id;
        if (known) { /* already open: drop the extra reference */
            SDL_JoystickClose(j);
            continue;
        }
        int kind = pad_kind(j);
        if (npads < MAX_PADS) pads[npads].id = id, pads[npads].kind = kind, npads++;
        fprintf(stderr, "controller %d: %s (%04x:%04x), %d buttons, %d axes, %d hats: %s buttons\n", i,
                SDL_JoystickName(j), pad_vendor(j), pad_product(j), SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j),
                SDL_JoystickNumHats(j), pad_names[kind]);
    }
}

/* a button pressed or let go, from whichever source; held buttons repeat in held_keys_update */
static void button(int b, int down, Uint32 now)
{
    if (b == B_NONE) return;
    if (down) {
        if (ui.held[b]) return;
        ui.held[b] = 1;
        ui.held_since[b] = ui.last_repeat[b] = now;
        on_press(b);
    } else if (ui.held[b]) {
        ui.held[b] = 0;
        on_release(b);
    }
}

/* the d-pad is hat 0 */
static int dpad_hat;
static void dpad_update(Uint32 now)
{
    static const int bit[4] = {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT};
    static const int btn[4] = {B_UP, B_DOWN, B_LEFT, B_RIGHT};
    for (int i = 0; i < 4; i++) button(btn[i], (dpad_hat & bit[i]) != 0, now);
}

/* a stick axis as -1..1, the centre's dead zone removed */
static float stick_value(int v)
{
    float x = v / 32767.0f, a = fabsf(x);
    return a < 0.2f ? 0.0f : (x > 0 ? 1.0f : -1.0f) * fminf((a - 0.2f) / 0.75f, 1.0f);
}

static void on_axis(int role, int v, Uint32 now)
{
    static int trig[2];
    if (role == AX_L2 || role == AX_R2) { /* L2, R2: pressed past halfway, let go below a quarter */
        int t = role == AX_R2;
        int down = trig[t] ? v > 8192 : v > 16384;
        if (down != trig[t]) {
            trig[t] = down;
            if (perf_level() == PERF_DETAIL) perf_log("INPUT", "%s %s (axis at %d)", t ? "R2" : "L2", down ? "down" : "up", v);
            button(t ? B_R2 : B_L2, down, now);
        }
    } else if (role == AX_LX || role == AX_LY) { /* the left stick sweeps the stations: across A, up and down B */
        sweep[role == AX_LY] = stick_value(v);
    } else if (role == AX_RX || role == AX_RY) { /* the right stick plays morph and the filter by hand */
        rstick[role == AX_RY] = stick_value(v);
    }
}

static void on_event(const SDL_Event *e, Uint32 now)
{
    switch (e->type) {
    case SDL_QUIT: ui.running = 0; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (!e->key.repeat) button(map_key(e->key.keysym.sym), e->type == SDL_KEYDOWN, now);
        break;
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP: {
        int pad = event_pad(e->jbutton.which), down = e->type == SDL_JOYBUTTONDOWN;
        int b = e->jbutton.button, m = pad >= 0 && b < 32 ? joy_map[pad][b] : B_NONE;
        /* every press, raw and as read: for checking a firmware's numbering */
        if (perf_level() == PERF_DETAIL)
            perf_log("INPUT", "controller %d button %d %s: %s", (int)e->jbutton.which, b, down ? "down" : "up",
                     pad < 0 ? "ignored (MustardOS's controller is read instead)" : m != B_NONE ? button_names[m] : "not used");
        if (pad < 0) break;
        if (m != B_NONE && m < B_COUNT) button(m, down, now);
        else if (m == B_NONE && down) ev("button %d (not used)", b); /* worth knowing on a new firmware */
        break;
    }
    case SDL_JOYHATMOTION:
        if (perf_level() == PERF_DETAIL)
            perf_log("INPUT", "controller %d hat %d: %d", (int)e->jhat.which, e->jhat.hat, e->jhat.value);
        if (e->jhat.hat == 0 && event_pad(e->jhat.which) >= 0) {
            dpad_hat = e->jhat.value;
            dpad_update(now);
        }
        break;
    case SDL_JOYAXISMOTION: {
        int pad = event_pad(e->jaxis.which);
        int role = pad >= 0 && e->jaxis.axis < PAD_AXES ? pad_axes[pad][e->jaxis.axis] : AX_NONE;
        on_axis(role, e->jaxis.value, now);
        break;
    }
    case SDL_JOYDEVICEADDED: open_joysticks(); break;
    case SDL_JOYDEVICEREMOVED:
        for (int i = 0; i < npads; i++)
            if (pads[i].id == e->jdevice.which) {
                pads[i] = pads[--npads];
                break;
            }
        break;
    case SDL_AUDIODEVICEADDED:
    case SDL_AUDIODEVICEREMOVED:
        if (!e->adevice.iscapture) audio_hotplug();
        break;
    }
}

static void render(void)
{
    update_blooms();
    if (ui.screen == SCR_PLAY) draw_play();
    else if (ui.screen == SCR_RAW) draw_raw();
    else draw_edit();
    if (ui.overlay == OV_KEYBOARD) draw_keyboard();
    else if (ui.overlay == OV_SCANMENU) draw_scan_menu();
    else if (ui.overlay == OV_SCANRESULTS) draw_scan_results();
    else if (ui.overlay == OV_UPDATE) draw_update();
    draw_toast();
}

/* ================================================================== setup */

static void resolve_paths(void)
{
    const char *env = getenv("RADIOSCAN_DIR");
    if (env) snprintf(app_dir, sizeof app_dir, "%s", env);
    else if (!getcwd(app_dir, sizeof app_dir)) snprintf(app_dir, sizeof app_dir, ".");
    snprintf(stations_path, sizeof stations_path, "%s/stations.txt", app_dir);
    snprintf(settings_path, sizeof settings_path, "%s/settings.cfg", app_dir);
}

static void init_common(void)
{
    char ca[PATH_MAX + 16];
    snprintf(ca, sizeof ca, "%s/cacert.pem", app_dir);
    net_init(ca);
    stream_set_file_root(app_dir);
    int fresh = stations_load(&stations, stations_path) < 0;
    if (fresh) {
        stations_add(&stations, "SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3", CAT_SOUND);
        stations_add(&stations, "SomaFM Mission Control", "http://ice1.somafm.com/missioncontrol-128-mp3", CAT_VOICE);
        stations_add(&stations, "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3", CAT_MUSIC);
    }
    /* .wav files dropped into the app's wav folder join the list as stations */
    char wav_dir[PATH_MAX + 16];
    snprintf(wav_dir, sizeof wav_dir, "%s/wav", app_dir);
    int wavs = stations_add_wav_folder(&stations, wav_dir, "wav");
    if (wavs) fprintf(stderr, "radioscan: added %d .wav file%s from %s\n", wavs, wavs == 1 ? "" : "s", wav_dir);
    if (fresh || wavs) save_stations();
    ui.pending_station[0] = ui.pending_station[1] = -2;
    ui.tuned[0] = ui.tuned[1] = -2;
    ui.tab = TAB_VOICES;
    ui.raw_fade = 3; /* 2 s */
    ui.raw_to = -1;
    if (load_settings() < 0) { /* first run: music on A, a talk station sliced into rhythm on B */
        synth_set_param(syn, P_A_STATION, (float)first_of_kind(CAT_MUSIC, 0));
        synth_set_param(syn, P_B_STATION, (float)first_of_kind(CAT_VOICE, 1));
    }
    if (settings_were_reset) toast("new version: sound settings reset, stations kept");
    if (raw_on) ui.screen = SCR_RAW;
    set_pitch();
    for (int o = 0; o < 2; o++) {
        int idx = station_param(o);
        if (idx >= stations.count) idx = -1;
        tune_now(o, idx);
    }
}

/* ================================================================== offline render (testing) */

static void write_wav_header(FILE *f, long data_bytes)
{
    unsigned char h[44];
    unsigned v32;
    unsigned short v16;
    memcpy(h, "RIFF", 4);
    v32 = (unsigned)(data_bytes + 36); memcpy(h + 4, &v32, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v32 = 16; memcpy(h + 16, &v32, 4);
    v16 = 1; memcpy(h + 20, &v16, 2);
    v16 = 2; memcpy(h + 22, &v16, 2);
    v32 = (unsigned)audio_rate; memcpy(h + 24, &v32, 4);
    v32 = (unsigned)audio_rate * 4; memcpy(h + 28, &v32, 4);
    v16 = 4; memcpy(h + 32, &v16, 2);
    v16 = 16; memcpy(h + 34, &v16, 2);
    memcpy(h + 36, "data", 4);
    v32 = (unsigned)data_bytes; memcpy(h + 40, &v32, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 44, 1, f);
}

/* radioscan --render <seconds> <out.wav> [key=value ...] [note=N] [grab=1] [dice=0|1|2] */
static int offline_render(int argc, char **argv)
{
    double secs = atof(argv[2]);
    audio_rate = getenv("RS_RATE") ? atoi(getenv("RS_RATE")) : 48000;
    syn = synth_create(audio_rate);
    st[0] = stream_create(audio_rate);
    st[1] = stream_create(audio_rate);
    st_next[0] = stream_create(audio_rate);
    st_next[1] = stream_create(audio_rate);
    init_common();
    for (int i = 4; i < argc; i++) {
        char k[64];
        float v;
        if (sscanf(argv[i], "%63[^=]=%f", k, &v) != 2) continue;
        if (!strcmp(k, "note")) { synth_note_on(syn, (int)v); continue; }
        if (!strcmp(k, "grab")) { synth_grab(syn, -1); continue; }
        if (!strcmp(k, "dice")) { synth_dice(syn, (int)v); continue; }
        if (!strcmp(k, "raw")) { raw_on = v != 0; continue; }
        if (!strcmp(k, "raw_xf")) { raw_xf = v; continue; }
        for (int p = 0; p < P_COUNT; p++)
            if (!strcmp(param_info[p].key, k)) synth_set_param(syn, p, v);
    }
    for (int i = 0; i < 200; i++) {
        int ready = 1;
        for (int o = 0; o < 2; o++) {
            StreamState s = stream_state(st[o]);
            if (s == ST_CONNECTING || s == ST_BUFFERING) ready = 0;
        }
        if (ready) break;
        SDL_Delay(100);
    }
    for (int o = 0; o < 2; o++) {
        char m[160];
        stream_status(st[o], m, sizeof m);
        fprintf(stderr, "OSC %c: %s (%d Hz, %d kbps)\n", 'A' + o, m, stream_src_rate(st[o]), stream_bitrate(st[o]));
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f) return 1;
    write_wav_header(f, 0);
    long frames = (long)(secs * audio_rate);
    int16_t pcm[1024 * 2];
    double peak = 0, sum = 0;
    Uint32 dsp_ms = 0;
    for (long done = 0; done < frames; done += 1024) {
        Uint32 t0 = SDL_GetTicks();
        audio_cb(NULL, (Uint8 *)pcm, sizeof pcm);
        dsp_ms += SDL_GetTicks() - t0;
        for (int i = 0; i < 2048; i++) { double v = pcm[i] / 32768.0; sum += v * v; if (fabs(v) > peak) peak = fabs(v); }
        fwrite(pcm, sizeof pcm, 1, f);
        if (!getenv("RS_FAST")) SDL_Delay(1024 * 1000 / audio_rate); /* real time so the network keeps up */
    }
    write_wav_header(f, frames / 1024 * 1024 * 4);
    fclose(f);
    fprintf(stderr, "rendered %.1fs: peak %.3f rms %.4f, dsp %.1f%% of real time\n", secs, peak,
            sqrt(sum / (frames * 2.0)), 100.0 * dsp_ms / (secs * 1000.0));
    static const char *const vs[] = {"silent", "music", "voice"};
    for (int o = 0; o < 2; o++) {
        char t[256];
        stream_title(st[o], t, sizeof t);
        fprintf(stderr, "OSC %c: voice sense %.2f (%s), title \"%s\"\n", 'A' + o, synth_speechness(syn, o),
                vs[synth_voice_state(syn, o)], t);
    }
    stream_destroy(st[0]);
    stream_destroy(st[1]);
    stream_destroy(st_next[0]);
    stream_destroy(st_next[1]);
    synth_destroy(syn);
    return 0;
}

/* ================================================================== main */

static volatile sig_atomic_t got_signal;
static void on_signal(int sig) { got_signal = sig; }

/* Wi-Fi sync over a simulated network: a leader 1.7 steps ahead whose
 * packets arrive 3 ms late at best and up to 40 ms later at random (one in
 * twenty held 100 ms, as for a power-saving unit), read only every 33 ms,
 * with the audio made in 1024-frame buffers. echo: the leader echoes our
 * packets (a current unit), or not (an older one). Returns how far the
 * follower is heard from the leader over the last 4 of 20 s, in ms. */
#define SIM_RATE 48000
static unsigned sim_rand = 12345;
static double sim_jitter(void)
{
    sim_rand = sim_rand * 1103515245u + 12345u;
    double u = (sim_rand >> 8) / 16777216.0;
    return 3000.0 + (u < 0.05 ? 100000.0 : u * 40000.0);
}

typedef struct {
    uint8_t b[SYNC_PKT_LEN];
    uint64_t at;
    int live;
} SimPkt;

static void sim_sync(int echo, double *mean_ms, double *worst_ms, float *delay)
{
    const double bpm = 120.0, spu = bpm / 15e6; /* 16ths per us */
    const uint64_t off = 987654321ULL;           /* the leader's clock runs elsewhere */
    const int buf = 1024;
    if (syn) synth_destroy(syn);
    syn = synth_create(SIM_RATE);
    synth_set_param(syn, P_SYNC, (float)SYNC_FOLLOW);
    synth_set_param(syn, P_BPM, 97.0f);
    sync_core_init(&sync_clock, 100);
    aclock_init(SIM_RATE, 0);
    static float zin[2 * 1024], zout[2 * 1024];
    SimPkt q[64];
    memset(q, 0, sizeof q);
    uint64_t heard_mine = 0, heard_mine_at = 0;
    double sum = 0, worst = 0;
    int n = 0;
    uint64_t next_cb = 0, next_poll = 0, next_lead = 7000, next_mine = 57000;
    for (uint64_t t = 0; t < 20000000ULL; t += 250) {
        double lead_pos = 1.7 + spu * (double)t, wire_pos = lead_pos + SYNC_WIRE_LEAD_US * spu;
        if (t >= next_cb) { /* the audio device asks for a buffer */
            aclock_request(t, buf);
            synth_process(syn, zin, zin, zout, buf);
            aclock_made(buf, synth_position(syn), synth_steps_per_frame(syn));
            next_cb += (uint64_t)(buf * 1e6 / SIM_RATE);
        }
        if (t >= next_lead) { /* the leader sends */
            next_lead += 100000;
            for (int i = 0; i < 64; i++)
                if (!q[i].live) {
                    uint8_t *b = q[i].b;
                    uint64_t ts = t + off;
                    float fb = (float)bpm;
                    uint32_t id = 7;
                    memset(b, 0, sizeof q[i].b);
                    memcpy(b, "HI5S", 4);
                    b[4] = 1, b[5] = 1, b[6] = SYNC_LEAD;
                    memcpy(b + 8, &id, 4);
                    memcpy(b + 12, &fb, 4);
                    memcpy(b + 16, &wire_pos, 8);
                    memcpy(b + 24, &ts, 8);
                    if (echo && heard_mine_at && t - heard_mine_at < 1000000) {
                        uint32_t me = 100, hold = (uint32_t)(t - heard_mine_at);
                        b[7] = 1;
                        memcpy(b + 32, &me, 4);
                        memcpy(b + 36, &heard_mine, 8);
                        memcpy(b + 44, &hold, 4);
                    }
                    q[i].at = t + (uint64_t)sim_jitter();
                    q[i].live = 1;
                    break;
                }
        }
        if (t >= next_mine) { /* we send; the leader hears it a trip later */
            next_mine += 100000;
            uint8_t b[SYNC_PKT_LEN];
            sync_make_packet(b, t);
            memcpy(&heard_mine, b + 24, 8);
            heard_mine_at = t + (uint64_t)sim_jitter();
        }
        if (t >= next_poll) { /* the screen's frame reads what has come in */
            next_poll += 33000;
            for (int i = 0; i < 64; i++)
                if (q[i].live && q[i].at <= t) {
                    sync_handle_packet(q[i].b, SYNC_PKT_LEN, q[i].at);
                    q[i].live = 0;
                }
        }
        if (t >= 16000000ULL && t % 10000 == 0) {
            double mine;
            if (aclock_position(t, &mine)) {
                double e = fmod(lead_pos - mine, 16.0);
                if (e > 8)
                    e -= 16;
                if (e <= -8)
                    e += 16;
                e = fabs(e / spu / 1000.0);
                sum += e;
                worst = e > worst ? e : worst;
                n++;
            }
        }
    }
    *mean_ms = n ? sum / n : 1e9;
    *worst_ms = worst;
    *delay = sync_status()->delay_ms;
}

/* --syncsim: the simulated network above, with a current leader and an older one */
static int sync_sim(void)
{
    double mean, worst;
    float delay;
    sim_sync(1, &mean, &worst, &delay);
    printf("sync timing: packets 3-43 ms late, read every 33 ms -> off by %.2f ms on average, %.2f at worst, "
           "trip measured %.1f ms (want under 1 and 2: %d)\n",
           mean, worst, delay, mean < 1.0 && worst < 2.0);
    int ok = mean < 1.0 && worst < 2.0;
    sim_sync(0, &mean, &worst, &delay);
    printf("sync timing, a leader without echo: off by %.2f ms on average (want under 6: %d)\n", mean, mean < 6.0);
    return ok && mean < 6.0 ? 0 : 1;
}


#ifndef RS_DEVICE
/* --joytest: the TrimUI's buttons, hat, triggers and stick through SDL's virtual joystick, checked against what the
 * app does with them (desktop builds only: the device's SDL2 may be older than the virtual joystick) */
static SDL_Joystick *jt;
static void jt_pump(void)
{
    SDL_Event e;
    SDL_JoystickUpdate();
    while (SDL_PollEvent(&e)) on_event(&e, SDL_GetTicks());
}
static void jt_button(int n, int down) { SDL_JoystickSetVirtualButton(jt, n, (Uint8)down); jt_pump(); }
static void jt_axis(int n, int v) { SDL_JoystickSetVirtualAxis(jt, n, (Sint16)v); jt_pump(); }
static void jt_hat(int v) { SDL_JoystickSetVirtualHat(jt, 0, (Uint8)v); jt_pump(); }

static int joy_test(void)
{
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) < 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    audio_rate = 48000;
    syn = synth_create(audio_rate);
    for (int o = 0; o < 2; o++) st[o] = stream_create(audio_rate), st_next[o] = stream_create(audio_rate);
    init_common();
    ui.screen = SCR_PLAY;
    ui.running = 1;
    joy_map_init();
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 11, 1);
    jt = idx >= 0 ? SDL_JoystickOpen(idx) : NULL;
    if (!jt) { fprintf(stderr, "virtual joystick: %s\n", SDL_GetError()); return 1; }
    jt_pump();
    int ok = 1, r;
#define JT_CHECK(what, cond) (r = (cond), ok &= r, printf("joy %s: %d\n", what, r))
    jt_button(7, 1);
    JT_CHECK("START (7) opens EDIT", ui.screen == SCR_EDIT && ui.held[B_START]);
    jt_button(7, 0);
    int tab = ui.tab;
    jt_button(5, 1), jt_button(5, 0);
    JT_CHECK("R1 (5) next page", ui.tab == (tab + 1) % TAB_COUNT);
    jt_button(4, 1), jt_button(4, 0);
    JT_CHECK("L1 (4) page back", ui.tab == tab);
    static const struct { int n, b; const char *what; } face[] = {
        {0, B_B, "B (0)"}, {1, B_A, "A (1)"}, {2, B_Y, "Y (2)"}, {3, B_X, "X (3)"}, {6, B_SELECT, "SELECT (6)"}};
    ui.screen = SCR_PLAY;
    for (int i = 0; i < 5; i++) {
        jt_button(face[i].n, 1);
        int held = ui.held[face[i].b];
        jt_button(face[i].n, 0);
        JT_CHECK(face[i].what, held && !ui.held[face[i].b]);
    }
    { /* F1 (9): new patterns, each rhythm still on or off; F2 (10): the dice on the sound */
        float before[SEQ_COUNT][4], after_on[SEQ_COUNT];
        for (int q = 0; q < SEQ_COUNT; q++) {
            synth_set_param(syn, SEQ_PARAM(q, 0), (float)(q & 1));
            for (int f = 0; f < 4; f++) before[q][f] = synth_get_param(syn, SEQ_PARAM(q, f));
        }
        int changed = 0, kept = 1;
        for (int t = 0; t < 4 && !changed; t++) { /* a roll can land on the same pattern; four in a row all can't */
            jt_button(9, 1), jt_button(9, 0);
            for (int q = 0; q < SEQ_COUNT; q++) {
                after_on[q] = synth_get_param(syn, SEQ_PARAM(q, 0));
                kept &= after_on[q] == before[q][0];
                for (int f = 1; f < 4; f++) changed |= synth_get_param(syn, SEQ_PARAM(q, f)) != before[q][f];
                kept &= synth_get_param(syn, SEQ_PARAM(q, 2)) <= synth_get_param(syn, SEQ_PARAM(q, 1));
            }
        }
        JT_CHECK("F1 (9) new patterns, on/off kept", changed && kept);
        float size_a = synth_get_param(syn, P_A_SIZE), size_b = synth_get_param(syn, P_B_SIZE);
        jt_button(10, 1), jt_button(10, 0);
        JT_CHECK("F2 (10) dice on the sound",
                 synth_get_param(syn, P_A_SIZE) != size_a || synth_get_param(syn, P_B_SIZE) != size_b);
    }
    jt_hat(SDL_HAT_RIGHT);
    int right = ui.held[B_RIGHT];
    jt_hat(SDL_HAT_RIGHTUP);
    int both = ui.held[B_RIGHT] && ui.held[B_UP];
    jt_hat(SDL_HAT_CENTERED);
    JT_CHECK("d-pad hat", right && both && !ui.held[B_RIGHT] && !ui.held[B_UP]);
    jt_axis(2, 32767);
    int l2 = ui.held[B_L2];
    jt_axis(2, 12000); /* past a quarter: still down */
    l2 &= ui.held[B_L2];
    jt_axis(2, -32768);
    JT_CHECK("L2 (axis 2)", l2 && !ui.held[B_L2]);
    jt_axis(5, 32767);
    int r2 = ui.held[B_R2];
    jt_axis(5, 0);
    JT_CHECK("R2 (axis 5)", r2 && !ui.held[B_R2]);
    jt_axis(0, 32767);
    int sa = sweep[0] == 1.0f && !ui.held[B_RIGHT]; /* the stick sweeps; it is no longer a d-pad */
    jt_axis(1, -32767);
    int sb = sweep[1] == -1.0f && !ui.held[B_UP];
    jt_axis(0, 4000); /* inside the dead zone */
    jt_axis(1, 0);
    JT_CHECK("left stick sweeps A across, B up / down", sa && sb && sweep[0] == 0.0f && sweep[1] == 0.0f);
    JT_CHECK("sweep: half the list either way, a station at a time near the centre, held on a boundary",
             sweep_offset(1, 21, 0) == 10 && sweep_offset(-1, 21, 0) == -10 && sweep_offset(0.3f, 21, 0) == 1 &&
             sweep_offset(1, 3, 0) == 1 && sweep_offset(0.51f, 21, 2) == 2 && sweep_offset(0.6f, 21, 2) == 4);
    { /* the right stick plays morph and the filter by hand and springs back, the settings untouched */
        static float in0[512 * 2], out0[512 * 2];
        synth_set_param(syn, P_MORPH, 0.25f);
        synth_set_param(syn, P_TONE, 0);
        jt_axis(3, 32767), jt_axis(4, -32767);
        rstick_apply();
        audio_lock();
        for (int i = 0; i < 40; i++) synth_process(syn, in0, in0, out0, 512);
        float m1 = synth_played_param(syn, P_MORPH), t1 = synth_played_param(syn, P_TONE);
        jt_axis(3, 0), jt_axis(4, 0);
        rstick_apply();
        for (int i = 0; i < 40; i++) synth_process(syn, in0, in0, out0, 512);
        float m0 = synth_played_param(syn, P_MORPH), t0 = synth_played_param(syn, P_TONE);
        audio_unlock();
        JT_CHECK("right stick (3, 4) morphs to B and opens the filter, then springs back",
                 m1 > 0.99f && t1 > 0.99f && fabsf(m0 - 0.25f) < 1e-3f && fabsf(t0) < 1e-3f &&
                     synth_get_param(syn, P_MORPH) == 0.25f && synth_get_param(syn, P_TONE) == 0);
        synth_set_param(syn, P_MORPH, 0);
    }
    jt_button(8, 1), jt_button(8, 0);
    int running = ui.running;
    jt_button(8, 1), jt_button(8, 0);
    JT_CHECK("MENU (8) twice quits", running && !ui.running);
    setenv("RS_JOYMAP", "A=0,B=1", 1);
    joy_map_init();
    ui.screen = SCR_PLAY;
    jt_button(0, 1);
    int a = ui.held[B_A];
    jt_button(0, 0);
    JT_CHECK("RS_JOYMAP=A=0,B=1", a && joy_map[PAD_TRIMUI][1] == B_B);

    /* MustardOS on a Brick Pro: its "muOS-Keys" controller (vendor "mu", product 3) beside the TrimUI's own,
     * numbered as MustardOS's tui-brick-pro map, L2 and R2 as buttons; the TrimUI's controller is then ignored */
    unsetenv("RS_JOYMAP");
    joy_map_init();
    open_joysticks(); /* the TrimUI's, as the app opens it at start */
    SDL_VirtualJoystickDesc d;
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = 4, d.nbuttons = 18, d.nhats = 1;
    d.vendor_id = 0x756d, d.product_id = 3;
    d.name = "muOS-Keys";
    int midx = SDL_JoystickAttachVirtualEx(&d);
    jt_pump(); /* the device arrives: opened and recognised */
    SDL_Joystick *mj = NULL;
    for (int i = 0; i < npads; i++)
        if (pads[i].kind == PAD_MUOS_BRICKPRO) mj = SDL_JoystickFromInstanceID(pads[i].id);
    JT_CHECK("MustardOS's controller found (Brick Pro)", mj != NULL);
    if (mj) {
        static const struct { int n, b; const char *what; } mux[] = {
            {6, B_A, "MustardOS A (6)"}, {5, B_B, "MustardOS B (5)"}, {11, B_L2, "MustardOS L2 (11)"},
            {12, B_R2, "MustardOS R2 (12)"}, {13, B_SELECT, "MustardOS SELECT (13)"}};
        ui.screen = SCR_PLAY;
        for (int i = 0; i < 5; i++) {
            SDL_JoystickSetVirtualButton(mj, mux[i].n, 1), jt_pump();
            int held = ui.held[mux[i].b];
            SDL_JoystickSetVirtualButton(mj, mux[i].n, 0), jt_pump();
            JT_CHECK(mux[i].what, held && !ui.held[mux[i].b]);
        }
        SDL_JoystickSetVirtualAxis(mj, 1, -32767), jt_pump();
        int mup = sweep[1] == -1.0f && !ui.held[B_UP];
        SDL_JoystickSetVirtualAxis(mj, 1, 0), jt_pump();
        SDL_JoystickSetVirtualAxis(mj, 2, 32767), jt_pump();
        int mrx = rstick[0] == 1.0f;
        SDL_JoystickSetVirtualAxis(mj, 2, 0), jt_pump();
        JT_CHECK("MustardOS left stick (axis 1) sweeps, right stick (axis 2) plays",
                 mup && sweep[1] == 0.0f && mrx && rstick[0] == 0.0f);
        jt_button(1, 1); /* the TrimUI's A underneath: ignored, or a press would arrive twice */
        int twice = ui.held[B_A];
        jt_button(1, 0);
        JT_CHECK("the TrimUI's controller ignored under MustardOS's", !twice);
    }
    printf("joytest: %s\n", ok ? "all good" : "FAILED");
    SDL_JoystickDetachVirtual(midx);
    SDL_JoystickClose(jt);
    SDL_JoystickDetachVirtual(idx);
    SDL_Quit();
    return !ok;
}
#endif

/* --jumptest: the click check finds a click in a clean tone, and nothing in a tone, a square wave or a
 * crushed sound */
static int jump_test(void)
{
    static int16_t b[48000 * 2];
    int n = 48000;
    uint32_t seg[4];
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < n; i++) {
            double ph = 2 * M_PI * 220.0 * i / 48000.0, x;
            if (k == 2) x = sin(ph) > 0 ? 0.5 : -0.5;                    /* square */
            else if (k == 3) x = 0.5 * sin(ph);                           /* crushed below */
            else x = 0.4 * sin(ph);
            if (k == 3) x = floor(x * 8.0) / 8.0;
            if (k == 1 && i == 30000) x += 0.5;                          /* one sample off: a click */
            b[i * 2] = b[i * 2 + 1] = (int16_t)(x * 32767);
        }
        if (k >= 2) perf_audio_output(b, n); /* a second of it first: the switch to it is a jump of its own */
        uint32_t before = perf_jumps();
        perf_audio_output(b, n);
        seg[k] = perf_jumps() - before;
    }
    uint32_t j = seg[0] + seg[1] + seg[2] + seg[3];
    int ok = seg[0] == 0 && seg[1] == 1 && seg[2] == 0 && seg[3] == 0;
    printf("jumptest: a tone %u, the tone with a click %u, a square wave %u, a crushed tone %u (want 0 1 0 0): %s\n",
           seg[0], seg[1], seg[2], seg[3], ok ? "all good" : "FAILED");
    (void)j;
    return !ok;
}

int main(int argc, char **argv)
{
    resolve_paths();
    update_detect_package(app_dir); /* TrimUI OS's zip, or MustardOS's .muxapp */
    if (argc >= 4 && !strcmp(argv[1], "--render")) return offline_render(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--syncsim")) return sync_sim();
    if (argc >= 2 && !strcmp(argv[1], "--jumptest")) return jump_test();
#ifndef RS_DEVICE
    if (argc >= 2 && !strcmp(argv[1], "--joytest")) return joy_test();
#endif
    if (argc >= 2 && !strcmp(argv[1], "--synctest")) { /* --synctest LEAD|FOLLOW|AUTO [bpm] [seconds] */
        int mode = argc > 2 && !strcmp(argv[2], "LEAD") ? SYNC_LEAD : argc > 2 && !strcmp(argv[2], "FOLLOW") ? SYNC_FOLLOW : SYNC_AUTO;
        float bpm = argc > 3 ? (float)atof(argv[3]) : 120.0f;
        int secs = argc > 4 ? atoi(argv[4]) : 5;
        syn = synth_create(48000);
        synth_set_param(syn, P_SYNC, (float)mode);
        synth_set_param(syn, P_BPM, bpm);
        synth_set_running(syn, mode == SYNC_LEAD);
        sync_init(&sync_clock);
        static float zin[2 * 480], zout[2 * 480];
        uint64_t t0 = now_us();
        while (now_us() - t0 < (uint64_t)secs * 1000000ULL) {
            synth_process(syn, zin, zin, zout, 480);
            sync_poll(now_us());
            usleep(10000);
        }
        const SyncStatus *ss = sync_status();
        printf("RADIOSCAN %s: bpm %.1f playing %d peers %d leading %d following %d offset %+.1f ms pos %.2f\n",
               argc > 2 ? argv[2] : "AUTO", synth_get_param(syn, P_BPM), synth_running(syn), ss->peers, ss->leading,
               ss->following, ss->offset_ms, synth_position(syn));
        sync_close();
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--scan")) { /* --scan <0 tag|1 name|2 random> [query]: test the directory */
        char ca[PATH_MAX + 16], err[200];
        snprintf(ca, sizeof ca, "%s/cacert.pem", app_dir);
        net_init(ca);
        int rc = directory_search(atoi(argv[2]), argc > 3 ? argv[3] : "", &scan_tmp, err, sizeof err);
        if (rc < 0) { fprintf(stderr, "%s\n", err); return 1; }
        for (int i = 0; i < scan_tmp.count; i++)
            printf("%s | %s | %s | %s\n", scan_tmp.items[i].name, scan_tmp.items[i].url, scan_tmp.items[i].info,
                   category_names[scan_tmp.items[i].cat]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--update")) { /* --update: check GitHub (or RS_UPDATE_API) and install into the app folder */
        char ca[PATH_MAX + 16];
        snprintf(ca, sizeof ca, "%s/cacert.pem", app_dir);
        net_init(ca);
        const UpdateStatus *u = update_status();
        if (update_check()) { fprintf(stderr, "%s\n", u->err); return 1; }
        printf("this is v" RS_VERSION ", the latest release is v%s\n", u->latest);
        if (u->state != UP_AVAILABLE && !(argc > 2 && !strcmp(argv[2], "--force"))) return 0;
        if (update_install(app_dir)) { fprintf(stderr, "update failed: %s\n", u->err); return 1; }
        printf("installed v%s into %s\n", u->latest, app_dir);
        return 0;
    }

    /* --shot file.bmp <play|edit|radio|motion|kb|scan|update> [tab] : render one frame (for docs/tests) */
    const char *shot = NULL, *shot_view = "play";
    int shot_tab = TAB_VOICES;
    if (argc >= 3 && !strcmp(argv[1], "--shot")) {
        shot = argv[2];
        if (argc >= 4) shot_view = argv[3];
        if (argc >= 5) shot_tab = atoi(argv[4]);
    }

    fprintf(stderr, "radioscan v" RS_VERSION " for TrimUI\n");
    if (!shot) perf_init("RS", NULL); /* the performance log, in log.txt (RS_LOG); one build for both devices */
    ev_t0 = perf_now_us();
    perf_before_stat(ev_flush); /* the event log is written out with each STAT line, so the lines stay in order */
    /* 1 GB: the program and its buffers stay in memory, so the sound never waits for a page to be read back in */
#ifdef RS_DEVICE
    if (!shot && (!getenv("RS_MLOCK") || atoi(getenv("RS_MLOCK"))) && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "radioscan: could not lock memory\n");
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, getenv("RS_SCALE") ? getenv("RS_SCALE") : "linear");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!shot && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    screen = SDL_CreateRGBSurface(0, SCREEN_W, SCREEN_H, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (!screen) { fprintf(stderr, "SDL_CreateRGBSurface: %s\n", SDL_GetError()); return 1; }
    px = screen->pixels;
    pitch_px = screen->pitch / 4;
    if (!shot) {
        SDL_ShowCursor(SDL_DISABLE);
        if (!video_init()) { SDL_Quit(); return 1; }
        /* something on the screen while the stations connect */
        fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
        text_center(SCREEN_W / 2, SCREEN_H / 2 - FONT_H / 2, "RADIOSCAN", C_LEAF, 0);
        present();
        joy_map_init();
        open_joysticks();
    }

    audio_rate = getenv("RS_RATE") ? atoi(getenv("RS_RATE")) : 48000;
    if (getenv("RS_RT")) aud_rt_wanted = atoi(getenv("RS_RT")) != 0;
    /* ~21 ms: the A53s have the time for half the Miyoo's delay */
    if (getenv("RS_BUFFER")) audio_buffer = atoi(getenv("RS_BUFFER"));
    if (audio_buffer < 64 || audio_buffer > 8192) audio_buffer = 1024;
    if (getenv("RS_SYNC_LATENCY")) audio_latency = atoi(getenv("RS_SYNC_LATENCY"));
    int buffer = audio_buffer;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    syn = synth_create(audio_rate);
    st[0] = stream_create(audio_rate);
    st[1] = stream_create(audio_rate);
    st_next[0] = stream_create(audio_rate);
    st_next[1] = stream_create(audio_rate);
    if (!syn || !st[0] || !st[1]) { fprintf(stderr, "out of memory\n"); return 1; }
    init_common();
    int audio_ok = 0;
    if (!shot) {
        audio_ok = audio_open(pick_output());
        if (!audio_ok) toast("audio device unavailable");
    }

    if (shot) {
        ui.screen = SCR_EDIT;
        if (!strcmp(shot_view, "play")) { ui.screen = SCR_PLAY; ui.held[B_UP] = 1; }
        if (!strcmp(shot_view, "playmod")) { ui.screen = SCR_PLAY; ui.held[B_R2] = 1; ui.held[B_UP] = 0; }
        if (!strcmp(shot_view, "edit")) ui.tab = shot_tab;
        if (!strcmp(shot_view, "radio")) ui.tab = TAB_RADIO;
        if (!strcmp(shot_view, "rhythm")) ui.tab = TAB_RHYTHM;
        if (!strcmp(shot_view, "rhythmx")) { ui.tab = TAB_RHYTHM; ui.sel[TAB_RHYTHM] = 99; }
        if (!strcmp(shot_view, "clock")) { /* RHYTHM at CLOCK OUT, in mono + clock */
            int rows[256];
            const char *hdrs[256];
            int n = build_rows(TAB_RHYTHM, rows, hdrs);
            ui.tab = TAB_RHYTHM;
            synth_set_param(syn, P_OUTPUT, OUT_MONO_CLOCK);
            for (int r = 0; r < n; r++)
                if (rows[r] == P_OUTPUT) ui.sel[TAB_RHYTHM] = r;
        }
        if (!strcmp(shot_view, "engine")) ui.tab = TAB_ENGINE;
        if (!strcmp(shot_view, "mod")) ui.tab = TAB_MOD;
        if (!strcmp(shot_view, "mix")) ui.tab = TAB_MIX;
        if (!strcmp(shot_view, "raw")) { /* RAW, partway from A to B */
            ui.screen = SCR_RAW;
            raw_on = 1;
            raw_xf = 0.3f;
            raw_peak[0] = 0.5f;
            raw_peak[1] = 0.2f;
        }
        if (!strcmp(shot_view, "kb")) open_keyboard(KB_ADD_URL, "http://ice1.somafm.com/");
        if (!strcmp(shot_view, "scan")) ui.overlay = OV_SCANMENU;
        if (!strcmp(shot_view, "update")) { /* the prompt, as if v9.9.9 were out */
            UpdateStatus *u = (UpdateStatus *)update_status();
            snprintf(u->latest, sizeof u->latest, "9.9.9");
            u->state = UP_AVAILABLE;
            ui.overlay = OV_UPDATE;
        }
        /* let the engine run a moment so the rings and waves have something to show */
        static float fa[2048], fb[2048], fo[2048];
        for (int i = 0; i < 2048; i++) { /* A: noisy, bursty; B: a plain tone */
            fa[i] = 0.25f * sinf(i * 0.05f) * sinf(i * 0.002f) + 0.15f * ((float)rand() / (float)RAND_MAX - 0.5f) * (i % 700 < 200);
            fb[i] = 0.2f * sinf(i * 0.011f);
        }
        for (int k = 0; k < 40; k++) synth_process(syn, fa, fb, fo, 1024);
        synth_grab(syn, -1);
        for (int k = 0; k < 30; k++) {
            synth_process(syn, fa, fb, fo, 1024);
            update_blooms();
        }
        render();
        SDL_SaveBMP(screen, shot);
        stream_destroy(st[0]);
        stream_destroy(st[1]);
        stream_destroy(st_next[0]);
        stream_destroy(st_next[1]);
        SDL_Quit();
        return 0;
    }

    sync_init(&sync_clock);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
#ifdef RS_DEVICE
    update_auto = 1;
#endif
    if (getenv("RS_UPDATE")) update_auto = atoi(getenv("RS_UPDATE")) != 0;
    ui.update_next_check = SDL_GetTicks() + 5000;
    fprintf(stderr, "radioscan v" RS_VERSION ": update check %s, from %s\n", update_auto ? "on" : "off", update_asset());
    fprintf(stderr, "radioscan: audio %s at %d Hz, %d frames/buffer\n", audio_ok ? "open" : "FAILED", audio_rate, buffer);
    ui.running = 1;
    perf_log("START", "running after %.2f s", (perf_now_us() - ev_t0) / 1e6);
    ev("start (event times are seconds since the app started, as the performance log's)");
    while (ui.running && !got_signal) {
        SDL_Event e;
        Uint32 now = SDL_GetTicks();
        while (SDL_PollEvent(&e)) on_event(&e, now);
        held_keys_update(now);
        logic_update(now);
        sync_poll(now_us());
        double f0 = mono_ms();
        render();
        present();
        perf_frame((uint64_t)((mono_ms() - f0) * 1000.0));
        Uint32 spent = SDL_GetTicks() - now; /* ~30 frames per second, whatever the drawing cost */
        if (spent < 33) SDL_Delay(33 - spent);
    }

    ev("quit");
    ev_flush();
    perf_close();
    sync_close();
    save_settings();
    pthread_mutex_lock(&dev_mx);
    if (adev) SDL_CloseAudioDevice(adev);
    adev = 0;
    pthread_mutex_unlock(&dev_mx);
    stream_destroy(st[0]);
    stream_destroy(st[1]);
    stream_destroy(st_next[0]);
    stream_destroy(st_next[1]);
    synth_destroy(syn);
    SDL_Quit();
    return update_status()->state == UP_INSTALLED ? RELAUNCH_STATUS : 0; /* launch.sh starts the new version */
}
