/* MIDI over USB, as in Columbia Street, HI5 and Jaffedrone: class-compliant
 * keyboards and controllers on the TrimUI's USB host port (the Brick's top
 * USB-C), read through ALSA's raw MIDI ports (/dev/snd/midiC*D*), so nothing
 * more is needed on the device. Ports are found as they are plugged in.
 *
 * In:
 *   notes   channel 1 plays voice A's pitch, channel 2 voice B's: middle C
 *           (60) is the voice's own PITCH, each key a semitone from it (up to
 *           two octaves either way). While keys are held the voice follows
 *           the last one; with all let go it goes back to its PITCH.
 *   CC      the important voice, engine and mix settings, on any channel
 *           (midi_cc_map; docs/MIDI.md is the chart)
 *   clock   the tempo follows it (fitted over the last four beats of ticks),
 *           its phase held on the audio clock by trimming the clock's speed;
 *           START, CONTINUE, STOP and song position start, stop and place the
 *           rhythms (not with MIDI Start set to IGNORE)
 * Out, to every port: clock (24 a quarter note), START and STOP while the
 * rhythms run and no clock is coming in, timed to the sound as it is heard.
 * Which way the clock goes is the RHYTHM page's MIDI Clock (P_MIDI: AUTO as
 * above, SEND always, FOLLOW only, OFF neither). Bricks also follow one
 * another over USB through a hub (link.h), as Columbia Street, HI5 and
 * Jaffedrone do.
 *
 * In launch.sh: RS_MIDI=0 turns it off, RS_MIDI_CLOCK=0 never sends clock,
 * RS_MIDI_FOLLOW=0 never follows it, RS_LINK=0 leaves the bottom port alone. */
#pragma once
#include <stdint.h>

/* the tempo clock the MIDI clock drives and is driven by, from the main
 * program: its calls take the audio lock themselves where they need it */
enum { MIDI_AUTO, MIDI_SEND, MIDI_FOLLOW, MIDI_OFF, NMIDIMODE };
typedef struct {
    int (*mode)(void);       /* MIDI_* */
    int (*clock_only)(void); /* follow the tempo, not START / STOP / song position */
    float (*bpm)(void);
    void (*set_bpm)(float bpm);
    double (*position)(void); /* in 16ths */
    void (*set_position)(double steps);
    void (*set_rate)(double rate); /* the speed trim, 1 = none */
    int (*playing)(void);
    void (*set_playing)(int on);
} MidiClock;
void midi_set_clock(const MidiClock *c);
void midi_set_latency(float seconds); /* from the clock's position to the sound heard */

void midi_start(void);
void midi_stop(void);
int midi_following(void); /* an outside clock leads */
int midi_ports(void);     /* the MIDI ports open, links included */
int midi_links(void);     /* Bricks following this one over the USB clock link (link.h) */
/* a message for the screen, any thread (fmt with one %s) */
void midi_notice(const char *fmt, const char *arg);
/* messages for the screen ("MIDI: <device>"), main thread */
int midi_take_notice(char *buf, int n);
/* the next note or CC for the main thread: kind 0x90 note on, 0x80 note off,
 * 0xb0 CC; 0 when there's none */
int midi_take(int *kind, int *ch, int *a, int *b);

/* the parser without the thread or devices, for the tests: bytes in, as if
 * from a port */
void midi_feed(const uint8_t *b, int n);
void midi_feed_at(const uint8_t *b, int n, uint64_t now_us); /* as heard at that time */
void midi_reset(void);
/* the clock out, stepped by hand: the bytes it would send now */
int midi_clock_step(uint64_t now_us, uint8_t *out, int max);
/* the leading Brick's position for the ones following over the link: its
 * bytes (F0 7D 'T' 'L' ... F7, see midi.c), and when they are due (every 20 ms
 * while the clock goes out; 0 when not) */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing);
int midi_link_step(uint64_t now_us, uint8_t *out, int max);
