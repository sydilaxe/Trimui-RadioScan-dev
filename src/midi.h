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
 *
 * RS_MIDI=0 in launch.sh turns it off. */
#pragma once
#include <stdint.h>

void midi_start(void);
void midi_stop(void);
/* messages for the screen ("MIDI: <device>"), main thread */
int midi_take_notice(char *buf, int n);
/* the next note or CC for the main thread: kind 0x90 note on, 0x80 note off,
 * 0xb0 CC; 0 when there's none */
int midi_take(int *kind, int *ch, int *a, int *b);

/* the parser without the thread or devices, for the tests: bytes in, as if
 * from a port */
void midi_feed(const uint8_t *b, int n);
void midi_reset(void);
