/* Performance log for the TrimUI Brick, Brick Hammer and Brick Pro ports
 * (shared by HI5 and RadioScan; keep the copies the same). It writes to
 * log.txt (stderr), for troubleshooting and for tuning the audio settings on
 * either device:
 *
 *   SYS    at start: the firmware (TrimUI OS or MustardOS), the board, this
 *          build, the processor's governor and clock range, the memory
 *   STAT   every 10 s (every second at DETAIL): the audio callback's load
 *          (time spent making sound against the time a buffer lasts),
 *          dropouts (the device ran out of sound), the longest wait between
 *          two callbacks, the screen's frame time and rate, the processor's
 *          clock, temperature, cores online and the system load
 *   HINT   what to change when something is wrong (each at most once a
 *          minute): a bigger buffer, a lower rate, a slowed processor, the
 *          Brick build on a Brick Pro
 *   PROC   with each STAT: where the processor's time goes (the whole app
 *          and its busiest threads, as % of one core), the app's memory
 *          (now and its peak), page faults (major ones mean waiting for the
 *          SD card), the system's free memory, and how often the audio
 *          thread was made to wait for another (involuntary switches)
 *   JUMP   the app's own output jumped far more than the sound around it
 *          (a click made inside the app: a click heard without one comes
 *          from after it, the driver or the sound server); at most one a
 *          second, all counted in STAT
 *   END    at quit: the session's worst figures
 *
 * <PREFIX>_LOG sets the level: 0 off, 1 BASIC (the default), 2 DETAIL.
 * The audio thread only calls perf_audio_begin / perf_audio_end, which
 * update a few counters; everything else runs on the main thread. */
#pragma once
#include <stdint.h>

enum { PERF_OFF, PERF_BASIC, PERF_DETAIL };

/* prefix: the apps' settings prefix ("HI5", "RS"), for <prefix>_LOG and the
 * hints; build: "brick", "brickpro", or NULL for one build serving both */
void perf_init(const char *prefix, const char *build);
int perf_level(void);
/* the sound device opened (or reopened): rate, frames a buffer, its name */
void perf_audio_open(int rate, int frames, const char *device);
void perf_audio_rt(int on); /* the audio thread runs at real-time priority */

/* audio thread: first and last thing in the callback */
uint64_t perf_audio_begin(int frames);
void perf_audio_end(uint64_t t0);
/* audio thread, optional: the 16-bit stereo just made, for the JUMP check */
void perf_audio_output(const int16_t *s, int frames);

/* main thread */
void perf_frame(uint64_t us);                     /* one screen frame took this long */
void perf_tick(void);                             /* often: writes STAT and HINT lines when due */
void perf_before_stat(void (*fn)(void));          /* called before each STAT line: write out buffered lines */
void perf_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void perf_close(void);                            /* the END line */
uint64_t perf_now_us(void);                       /* the monotonic clock the log uses */
uint32_t perf_dropouts(void);                     /* this session's, so far (for the tests) */
uint32_t perf_jumps(void);                        /* the same for JUMP */
