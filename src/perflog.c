/* Performance log: see perflog.h. Shared by HI5 and RadioScan; keep the
 * copies the same. */
#define _GNU_SOURCE
#include "perflog.h"

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AT(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define SET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define TAKE(x) __atomic_exchange_n(&(x), 0, __ATOMIC_RELAXED)
#define ADD(x, v) __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)

static int level = PERF_BASIC;
static char prefix[16] = "APP";
static const char *build_name;
static uint64_t t_start, t_last_stat;

/* written by the audio thread, taken (and reset) by the main thread */
static uint32_t a_calls, a_late, a_us_max, a_gap_max;
static uint64_t a_us_sum, a_frames;
/* the audio thread's own: the sound made against real time, for dropouts */
static int a_rate = 48000, a_bufsz;
static int a_reset = 1;
static uint64_t a_clock0, a_made_us, a_prev;
static int a_rt = -1;
static char a_dev[96];

/* the screen, main thread only */
static uint32_t s_frames;
static uint64_t s_us_sum, s_us_max, s_first; /* s_first: the first frame, so start-up isn't counted as slow */
static void (*before_stat)(void);

/* the session's worst, for the END line */
static uint32_t w_late, w_gap, w_mhz_min, w_mhz_max, w_jumps;
static double w_peak, w_load;
static long w_temp = -1;
static uint64_t w_frame;

/* when each kind of hint was last given */
enum { H_LOAD, H_LATE, H_CLOCK, H_HOT, H_SCREEN, H_COUNT };
static uint64_t hint_at[H_COUNT];
static long max_khz = -1;

uint64_t perf_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

int perf_level(void) { return level; }

static void vline(const char *tag, const char *fmt, va_list ap)
{
    fprintf(stderr, "%9.3f %-5s ", (perf_now_us() - t_start) / 1e6, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void perf_log(const char *tag, const char *fmt, ...)
{
    if (level == PERF_OFF)
        return;
    va_list ap;
    va_start(ap, fmt);
    vline(tag, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* a number from a system file, or -1 */
static long read_num(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

/* the first line of a system file, or "" */
static void read_line(const char *path, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(out, n, f))
        out[0] = 0;
    out[strcspn(out, "\n")] = 0;
    fclose(f);
}

/* the hottest of the thermal zones, in degrees (some kernels give millidegrees) */
static long temperature(void)
{
    long hot = -1;
    for (int z = 0; z < 8; z++) {
        char p[64];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        long t = read_num(p);
        if (t > 1000)
            t /= 1000;
        if (t > hot)
            hot = t;
    }
    return hot;
}

#define CPU "/sys/devices/system/cpu/"

static void system_line(void)
{
    int muos = access("/opt/muos", F_OK) == 0;
    char board[64], gov[32], model[64] = "";
    read_line("/opt/muos/device/config/board/name", board, sizeof board);
    read_line(CPU "cpu0/cpufreq/scaling_governor", gov, sizeof gov);
    const char *m = getenv("TRIMUI_MODEL");
    if (m)
        snprintf(model, sizeof model, ", model %s", m);
    max_khz = read_num(CPU "cpu0/cpufreq/scaling_max_freq");
    long min_khz = read_num(CPU "cpu0/cpufreq/scaling_min_freq");
    long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    char clock[48] = "clock ?", temp[16] = "? C";
    if (max_khz > 0)
        snprintf(clock, sizeof clock, "%ld-%ld MHz", min_khz / 1000, max_khz / 1000);
    if (temperature() >= 0)
        snprintf(temp, sizeof temp, "%ld C", temperature());
    perf_log("SYS", "%s%s%s%s, build %s, %ld cores, governor %s, %s, memory %ld MB (%ld free), %s",
             muos ? "MustardOS" : "TrimUI OS", board[0] ? ", board " : "", board, model,
             build_name ? build_name : "brick + brickpro", sysconf(_SC_NPROCESSORS_ONLN), gov[0] ? gov : "?", clock,
             pages > 0 ? pages * (psz / 1024) / 1024 : -1, avail > 0 ? avail * (psz / 1024) / 1024 : -1, temp);
    /* the Brick build on a Brick Pro has no sticks (and the other way round, the
     * sticks' axes are read as nothing): MustardOS names the board */
    if (build_name && board[0]) {
        int pro = strstr(board, "brick-pro") != NULL || strstr(board, "brickpro") != NULL;
        if (pro != !strcmp(build_name, "brickpro"))
            perf_log("HINT", "this is the %s build on a %s: install the %s package", build_name,
                     pro ? "Brick Pro" : "Brick", pro ? "brickpro" : "brick");
    }
}

void perf_init(const char *pfx, const char *build)
{
    snprintf(prefix, sizeof prefix, "%s", pfx);
    build_name = build;
    char name[32];
    snprintf(name, sizeof name, "%s_LOG", prefix);
    const char *v = getenv(name);
    if (v && *v)
        level = atoi(v) <= 0 ? PERF_OFF : atoi(v) >= 2 ? PERF_DETAIL : PERF_BASIC;
    t_start = t_last_stat = perf_now_us();
    w_mhz_min = UINT32_MAX;
    if (level == PERF_OFF)
        return;
    perf_log("LOG", "performance log %s (%s_LOG=0 off, 1 basic, 2 detail: a status line every second)",
             level == PERF_DETAIL ? "DETAIL" : "BASIC", prefix);
    system_line();
}

void perf_audio_open(int rate, int frames, const char *device)
{
    a_rate = rate > 0 ? rate : 48000;
    a_bufsz = frames;
    snprintf(a_dev, sizeof a_dev, "%s", device && *device ? device : "speaker / headphones");
    SET(a_reset, 1);
    perf_log("AUDIO", "%s, %d Hz, %d frames a buffer (%.1f ms)", a_dev, a_rate, frames, frames * 1000.0 / a_rate);
}

void perf_audio_rt(int on)
{
    if (on != a_rt)
        perf_log("AUDIO", "real-time priority %s", on ? "on" : "off");
    a_rt = on;
}

/* the audio thread's id, for its switches in PROC; callbacks that came late
 * (over one and a half buffers after the last), dropout or not */
static volatile int a_tid;
static uint32_t a_slow;

uint64_t perf_audio_begin(int frames)
{
    uint64_t t = perf_now_us();
    if (!AT(a_tid))
        SET(a_tid, (int)syscall(SYS_gettid));
    uint64_t buf_us = (uint64_t)frames * 1000000ULL / (uint64_t)a_rate;
    if (AT(a_reset)) {
        SET(a_reset, 0);
        a_clock0 = t, a_made_us = 0, a_prev = 0;
    }
    else {
        /* the sound made so far against the time gone by: more than three
         * buffers behind, the device ran dry (a dropout); start counting again */
        if (t - a_clock0 > a_made_us + 3 * buf_us) {
            ADD(a_late, 1);
            a_clock0 = t - a_made_us;
        }
    }
    if (a_prev) {
        uint32_t gap = (uint32_t)(t - a_prev);
        if (gap * 2 > buf_us * 3)
            ADD(a_slow, 1);
        if (gap > AT(a_gap_max))
            SET(a_gap_max, gap);
    }
    a_prev = t;
    a_made_us += buf_us;
    ADD(a_calls, 1);
    ADD(a_frames, (uint64_t)frames);
    return t;
}

void perf_audio_end(uint64_t t0)
{
    uint32_t spent = (uint32_t)(perf_now_us() - t0);
    ADD(a_us_sum, (uint64_t)spent);
    if (spent > AT(a_us_max))
        SET(a_us_max, spent);
}

/* JUMP: a click is an isolated sharp turn in the output: its second
 * difference (how sharply it turns) far above the sharpest of the last
 * ~50 ms, and the sound just after it smooth again (a step or a lone bad
 * sample, as a gap or a cut leaves). A drum, a burst of noise or a crushed
 * sound turns sharply and stays rough, and a square wave's edges come again
 * and again, so neither counts. Checked across buffers too, where a gap
 * would land. */
#define J_AFTER 72 /* samples looked at after a candidate: 1.5 ms at 48 kHz */
static float j_prev[2][2], j_pk = 1.f; /* the decaying peak (starts high: no jump at the very start) */
static float j_cand, j_cand_typ, j_after_sum;
static int j_after_left = -1, j_skip;
static uint32_t j_count, j_logged_at_s = UINT32_MAX;
static volatile float j_size, j_typ;
static volatile uint32_t j_when_ms, j_new;

static void jump_found(float size, float typ)
{
    ADD(j_count, 1);
    uint32_t ms = (uint32_t)((perf_now_us() - t_start) / 1000);
    if (ms / 1000 != j_logged_at_s) { /* at most one a second for the log */
        j_logged_at_s = ms / 1000;
        j_size = size, j_typ = typ, j_when_ms = ms;
        __atomic_store_n(&j_new, 1, __ATOMIC_RELEASE);
    }
}

void perf_audio_output(const int16_t *s, int frames)
{
    float pk = j_pk;
    for (int i = 0; i < frames; i++) {
        float d = 0.f; /* the sharper channel's turn */
        for (int c = 0; c < 2; c++) {
            float x = s[i * 2 + c] / 32768.f;
            float dc = fabsf(x - 2.f * j_prev[c][1] + j_prev[c][0]);
            j_prev[c][0] = j_prev[c][1], j_prev[c][1] = x;
            if (dc > d)
                d = dc;
        }
        if (j_after_left >= 0) { /* after a candidate: is the sound smooth again? */
            if (j_skip > 0)
                j_skip--; /* a step turns twice, the second the other way: not counted */
            else
                j_after_sum += d;
            if (--j_after_left < 0 && j_after_sum / (J_AFTER - 2) < j_cand * 0.06f)
                jump_found(j_cand, j_cand_typ);
        }
        else if (d > 0.2f && d > 4.f * pk) {
            j_cand = d, j_cand_typ = pk, j_after_sum = 0.f, j_after_left = J_AFTER, j_skip = 2;
        }
        pk = d > pk ? d : pk * 0.99971f; /* half its height in ~50 ms at 48 kHz */
    }
    j_pk = pk;
}

void perf_before_stat(void (*fn)(void)) { before_stat = fn; }

void perf_frame(uint64_t us)
{
    if (!s_first)
        s_first = perf_now_us();
    s_frames++;
    s_us_sum += us;
    if (us > s_us_max)
        s_us_max = us;
}

/* ------------------------------------------------------------------ PROC */

static long ticks_hz;
#define NTHREAD 48
static struct {
    int tid;
    unsigned long ticks;
} th_prev[NTHREAD];
static int th_nprev;
static unsigned long p_prev_ticks, p_prev_minflt, p_prev_majflt, a_prev_invol, a_prev_vol;

/* a thread's (or the process's) name and its user + system ticks; the faults
 * too for the process */
static int read_stat(const char *path, char *name, int n, unsigned long *ticks, unsigned long *minflt,
                     unsigned long *majflt)
{
    char b[512];
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t r = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[r] = 0;
    char *l = strchr(b, '('), *rp = strrchr(b, ')');
    if (!l || !rp)
        return 0;
    snprintf(name, (size_t)n, "%.*s", (int)(rp - l - 1), l + 1);
    /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
    unsigned long v[13];
    if (sscanf(rp + 2, "%*c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) < 12)
        return 0;
    if (minflt)
        *minflt = v[6];
    if (majflt)
        *majflt = v[8];
    *ticks = v[10] + v[11];
    return 1;
}

static long status_kb(const char *path, const char *key)
{
    char line[128];
    long v = -1;
    size_t k = strlen(key);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, k) && line[k] == ':') {
            v = atol(line + k + 1);
            break;
        }
    fclose(f);
    return v;
}

static void proc_line(double secs)
{
    if (secs <= 0)
        return;
    if (!ticks_hz)
        ticks_hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    double per = 100.0 / (ticks_hz * secs); /* ticks to % of one core */
    char name[32];
    unsigned long ticks, minflt = 0, majflt = 0;
    double app = -1;
    if (read_stat("/proc/self/stat", name, sizeof name, &ticks, &minflt, &majflt)) {
        if (p_prev_ticks)
            app = (ticks - p_prev_ticks) * per;
        p_prev_ticks = ticks;
    }
    /* the busiest threads */
    struct {
        double pc;
        char name[20];
    } top[4] = {{-1, ""}, {-1, ""}, {-1, ""}, {-1, ""}};
    int nnew = 0;
    typeof(th_prev[0]) now_th[NTHREAD];
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d)) && nnew < NTHREAD) {
        int tid = atoi(e->d_name);
        if (tid <= 0)
            continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        if (!read_stat(path, name, sizeof name, &ticks, NULL, NULL))
            continue;
        now_th[nnew].tid = tid, now_th[nnew].ticks = ticks, nnew++;
        for (int i = 0; i < th_nprev; i++)
            if (th_prev[i].tid == tid) {
                double pc = (ticks - th_prev[i].ticks) * per;
                for (int k = 0; k < 4; k++)
                    if (pc > top[k].pc) {
                        memmove(&top[k + 1], &top[k], sizeof top[0] * (size_t)(3 - k));
                        top[k].pc = pc;
                        snprintf(top[k].name, sizeof top[k].name, "%s%s", tid == AT(a_tid) ? "audio:" : "", name);
                        break;
                    }
            }
    }
    if (d)
        closedir(d);
    memcpy(th_prev, now_th, sizeof now_th[0] * (size_t)nnew);
    th_nprev = nnew;
    if (app < 0)
        return; /* the first: a baseline */
    char threads[160] = "";
    for (int k = 0; k < 4 && top[k].pc >= 0; k++)
        snprintf(threads + strlen(threads), sizeof threads - strlen(threads), "%s%s %.0f%%", k ? ", " : "",
                 top[k].name, top[k].pc);
    /* the audio thread's switches: voluntary (it waited for the device) and
     * involuntary (something else ran instead) */
    char sw[64] = "";
    if (AT(a_tid)) {
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/status", AT(a_tid));
        long vol = status_kb(path, "voluntary_ctxt_switches"), inv = status_kb(path, "nonvoluntary_ctxt_switches");
        if (vol >= 0 && inv >= 0) {
            if (a_prev_vol)
                snprintf(sw, sizeof sw, " | audio thread waited %ld, preempted %ld", vol - (long)a_prev_vol,
                         inv - (long)a_prev_invol);
            a_prev_vol = (unsigned long)vol, a_prev_invol = (unsigned long)inv;
        }
    }
    long rss = status_kb("/proc/self/status", "VmRSS"), hwm = status_kb("/proc/self/status", "VmHWM");
    long avail = status_kb("/proc/meminfo", "MemAvailable");
    perf_log("PROC", "cpu %.0f%% (%s) | memory %.1f MB, peak %.1f | faults %lu major, %lu minor | free %ld MB%s", app,
             threads, rss / 1024.0, hwm / 1024.0, p_prev_majflt ? majflt - p_prev_majflt : 0,
             p_prev_minflt ? minflt - p_prev_minflt : 0, avail / 1024, sw);
    p_prev_minflt = minflt, p_prev_majflt = majflt;
}

static int hint_due(int h, uint64_t now)
{
    if (hint_at[h] && now - hint_at[h] < 60000000ULL)
        return 0;
    hint_at[h] = now;
    return 1;
}

static void status(uint64_t now, double secs)
{
    uint32_t calls = TAKE(a_calls), late = TAKE(a_late), us_max = TAKE(a_us_max), gap = TAKE(a_gap_max);
    uint32_t slow = TAKE(a_slow), jumps = TAKE(j_count);
    uint64_t us_sum = TAKE(a_us_sum), frames = TAKE(a_frames);
    double made_us = frames * 1e6 / a_rate;                      /* the sound those callbacks made */
    double load = made_us > 0 ? 100.0 * us_sum / made_us : 0;    /* of the time it lasts */
    double buf_us = calls ? made_us / calls : a_bufsz * 1e6 / a_rate;
    double peak = buf_us > 0 ? 100.0 * us_max / buf_us : 0;
    long khz = read_num(CPU "cpu0/cpufreq/scaling_cur_freq"), temp = temperature();
    char cores[16], loadavg[32];
    read_line(CPU "online", cores, sizeof cores);
    read_line("/proc/loadavg", loadavg, sizeof loadavg);
    loadavg[strcspn(loadavg, " ")] = 0; /* the last minute's */
    char cpu[48] = "cpu ?";
    if (khz > 0 || temp >= 0)
        snprintf(cpu, sizeof cpu, "cpu %ld MHz %ld C", khz > 0 ? khz / 1000 : 0, temp);
    /* frames a second since the window opened, or since the last line */
    double fsecs = s_first > now - (uint64_t)(secs * 1e6) ? (now - s_first) / 1e6 : secs;
    double fps = fsecs > 0.5 ? s_frames / fsecs : 0, frame_avg = s_frames ? s_us_sum / 1000.0 / s_frames : 0;
    if (before_stat)
        before_stat();

    perf_log("STAT",
             "audio load avg %.0f%% peak %.0f%% | dropouts %u, late calls %u, jumps %u | gap max %.1f ms (buffer %.1f) | "
             "%.0f calls/s | screen %.0f fps, avg %.1f ms max %.1f ms | %s cores %s load %s",
             load, peak, late, slow, jumps, gap / 1000.0, buf_us / 1000.0, secs > 0 ? calls / secs : 0, fps, frame_avg,
             s_us_max / 1000.0, cpu, cores[0] ? cores : "?", loadavg[0] ? loadavg : "?");

    proc_line(secs);

    /* the session's worst */
    w_late += late;
    w_jumps += jumps;
    if (gap > w_gap)
        w_gap = gap;
    if (peak > w_peak)
        w_peak = peak;
    if (load > w_load)
        w_load = load;
    if (temp > w_temp)
        w_temp = temp;
    if (s_us_max > w_frame)
        w_frame = s_us_max;
    if (khz > 0) {
        if ((uint32_t)(khz / 1000) < w_mhz_min)
            w_mhz_min = (uint32_t)(khz / 1000);
        if ((uint32_t)(khz / 1000) > w_mhz_max)
            w_mhz_max = (uint32_t)(khz / 1000);
    }

    /* what to change */
    int slow_clock = khz > 0 && max_khz > 0 && khz < max_khz * 9 / 10;
    if (load > 60 && hint_due(H_LOAD, now))
        perf_log("HINT", "audio load %.0f%%: set %s_BUFFER=%d in launch.sh, or %s_RATE=44100", load, prefix,
                 a_bufsz * 2, prefix);
    if (late && hint_due(H_LATE, now))
        perf_log("HINT", "%u dropouts (crackles): set %s_BUFFER=%d in launch.sh%s%s", late, prefix, a_bufsz * 2,
                 load < 40 && !slow_clock ? "; the load is low, so something else held the sound up (see sysinfo.txt)"
                                          : "",
                 s_us_max > 100000 ? "; the screen stalled too" : "");
    if (slow_clock && hint_due(H_CLOCK, now))
        perf_log("HINT", "the processor runs at %ld of %ld MHz: the governor isn't on performance (launch.sh sets it)%s",
                 khz / 1000, max_khz / 1000, temp >= 75 ? ", or it is too hot" : "");
    if (temp >= 80 && hint_due(H_HOT, now))
        perf_log("HINT", "the processor is at %ld C: it slows down when hot; a case off or a cooler room helps", temp);
    if (s_frames && fps > 0 && fps < 20 && hint_due(H_SCREEN, now))
        perf_log("HINT", "the screen draws %.0f frames a second (avg %.1f ms a frame): the sound comes first, but "
                         "this may be the processor (see the cpu figures)",
                 fps, frame_avg);
    s_frames = 0, s_us_sum = s_us_max = 0;
}

void perf_tick(void)
{
    if (level == PERF_OFF)
        return;
    if (__atomic_exchange_n(&j_new, 0, __ATOMIC_ACQUIRE))
        perf_log("JUMP", "at %.2f s the output jumped %.3f (typically %.4f): a click made inside the app",
                 j_when_ms / 1000.0, j_size, j_typ);
    uint64_t now = perf_now_us(), every = level == PERF_DETAIL ? 1000000ULL : 10000000ULL;
    if (now - t_last_stat >= every) {
        status(now, (now - t_last_stat) / 1e6);
        t_last_stat = now;
    }
}

uint32_t perf_dropouts(void) { return w_late + AT(a_late); }
uint32_t perf_jumps(void) { return w_jumps + AT(j_count); }

void perf_close(void)
{
    if (level == PERF_OFF)
        return;
    uint64_t now = perf_now_us();
    status(now, (now - t_last_stat) / 1e6);
    char cpu[64] = "";
    if (w_mhz_max)
        snprintf(cpu, sizeof cpu, ", cpu %u-%u MHz", w_mhz_min, w_mhz_max);
    if (w_temp >= 0)
        snprintf(cpu + strlen(cpu), sizeof cpu - strlen(cpu), ", max %ld C", w_temp);
    perf_log("END", "session %.0f s: dropouts %u, jumps %u, audio load worst avg %.0f%% peak %.0f%%, gap max %.1f ms, "
                    "screen max %.1f ms%s",
             (now - t_start) / 1e6, w_late, w_jumps, w_load, w_peak, w_gap / 1000.0, w_frame / 1000.0, cpu);
}
