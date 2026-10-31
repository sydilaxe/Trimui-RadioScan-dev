/* The Brick Pro's LEDs, one by one, through TrimUI's led_anim driver
 * (/sys/class/led_anim). Shared by HI5 and RadioScan; keep the copies the
 * same (Columbia Street and Jaffedrone do the same in their own LED code).
 *
 * The driver's named zones (f1, f2, m, lr, l, r) are the Brick's: on the
 * Brick Pro they reach only the left stick's ring. frame_hex sets each of
 * its 23 LEDs instead, in this order (as mapped on a Brick Pro):
 *   1-8 the top bar, left to right;  9 F2;  10 F1
 *   11-14 the left stick's ring, 15-18 the right's (each upper left, lower
 *   left, lower right, upper right);  19 L1, 20 L2, 21 R2, 22 R1
 * While the app runs:
 *   top bar     a stereo level meter of the output, from the centre out:
 *               the left channel to the left, the right to the right, green
 *               rising to amber and red near full scale
 *   rings       each points where its stick is pushed, its corners brighter
 *               the further it goes, in a colour the app chooses
 *   L1 L2 R1 R2 the beat: a flash on each, orange on the bar's first (or
 *               as the app chooses, ringleds_shoulders)
 *   F1, F2      as the app chooses (dark by default)
 * The named effects are off while the frame shows (effect_enable 0); at the
 * end the frame goes dark and the settings found at start (colours,
 * effects, brightness) are put back. Until then ringleds_saved.txt in the
 * app folder keeps them, so a crash doesn't lose them.
 *
 * Settings (<P> is the app's prefix): <P>_LEDS=0 leaves the LEDs alone;
 * <P>_LED_BRIGHTNESS=1..60 (60 by default); <P>_LED_BAR=0 leaves the top
 * bar dark; <P>_LED_DIR another folder (the tests); <P>_LED_FRAME=1 drives
 * them on another device too (the tests). */
#pragma once
#include <stdint.h>

/* prefix: "HI5", "RS"; brickpro: 1 on a Brick Pro (elsewhere nothing
 * happens unless <P>_LED_FRAME=1). 1 if the LEDs are driven. */
int ringleds_open(const char *prefix, int brickpro);
/* audio thread: the 16-bit stereo just made, for the level meter */
void ringleds_audio(const int16_t *s, int frames);
/* main thread, about every frame: the sticks (x right, y up, -1 .. 1) and
 * their rings' colours; the beat (playing, the step in 16ths, the tempo);
 * F1's and F2's colours (0: dark). Written only when something changes. */
void ringleds_update(float lx, float ly, uint32_t lcol, float rx, float ry, uint32_t rcol, int playing, int step,
                     float bpm, uint32_t f1, uint32_t f2);
/* main thread: the shoulder LEDs' colours from the next ringleds_update on;
 * RINGLEDS_BEAT (the default) shows the beat there, as above */
#define RINGLEDS_BEAT 0xffffffffu
void ringleds_shoulders(uint32_t l1, uint32_t l2, uint32_t r1, uint32_t r2);
/* a colour at an intensity 0..1, on the same curve the LEDs use */
uint32_t ringleds_glow(uint32_t c, float t);
void ringleds_close(void);
