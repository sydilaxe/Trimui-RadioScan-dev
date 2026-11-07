/* The clock link between Bricks over USB, with no MIDI interface between
 * them: the leader's top (host) port goes to a hub, and each follower's
 * bottom port to the same hub. A follower presents its bottom port as a
 * "TrimUI clock link" device (USB gadget mode through configfs and
 * FunctionFS: a vendor interface with one bulk OUT endpoint, as TrimUI's
 * kernel has no USB MIDI gadget), and what arrives on it is MIDI (clock,
 * START, STOP, song position), fed to midi.c as if from a MIDI port. The
 * leader finds the followers on its USB bus (link_find) and sends them what
 * it sends its MIDI ports (midi.c), through usbfs. So the wiring decides who
 * leads, and any number of Bricks can follow one.
 *
 * RS_LINK=0 in launch.sh leaves the bottom port alone. */
#pragma once
#include <stdint.h>

#define LINK_VID 0x1d6b /* the Linux Foundation's composite gadget id */
#define LINK_PID 0x0104
#define LINK_SUBCLASS 0x42 /* 'B' */
#define LINK_PROTOCOL 0x4c /* 'L' */

/* the follower: set up the bottom port (1) or not (0: no device controller,
 * no configfs, the port in use, not root, or RS_LINK=0); and give it back */
int link_follow_start(void);
void link_follow_stop(void);
/* 0 not set up, 1 waiting for a leader, 2 a leader sending (in the last 2 s) */
int link_follow_state(void);

/* the leader: linked Bricks under a sysfs USB devices folder
 * (/sys/bus/usb/devices), at most max */
typedef struct {
    int bus, dev, ep; /* the usbfs device and its bulk OUT endpoint address */
    char path[300];   /* the sysfs folder, to tell when it goes */
} LinkDev;
int link_find(const char *sysfs, LinkDev *out, int max);

/* what the follower writes to FunctionFS, for the tests: the length */
int link_descriptors(uint8_t *buf, int max);
int link_strings(uint8_t *buf, int max);
