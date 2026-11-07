/* The clock link between Bricks over USB (see link.h). */
#define _GNU_SOURCE
#include "link.h"
#include "midi.h"

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINK_NAME "TrimUI clock link"
#define GADGET "/sys/kernel/config/usb_gadget/trimuilink"
#define FFS_NAME "trimuilink"
#define FFS_DIR "/tmp/trimuilink-ffs"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------ descriptors */

typedef struct {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio out;
} __attribute__((packed)) LinkDescs;

int link_descriptors(uint8_t *buf, int max)
{
    struct {
        struct usb_functionfs_descs_head_v2 head;
        __le32 fs_count, hs_count;
        LinkDescs fs, hs;
    } __attribute__((packed)) d;
    memset(&d, 0, sizeof d);
    d.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    d.head.length = htole32(sizeof d);
    d.fs_count = d.hs_count = htole32(2);
    LinkDescs *s[2] = {&d.fs, &d.hs};
    for (int k = 0; k < 2; k++) {
        s[k]->intf.bLength = USB_DT_INTERFACE_SIZE;
        s[k]->intf.bDescriptorType = USB_DT_INTERFACE;
        s[k]->intf.bNumEndpoints = 1;
        s[k]->intf.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
        s[k]->intf.bInterfaceSubClass = LINK_SUBCLASS;
        s[k]->intf.bInterfaceProtocol = LINK_PROTOCOL;
        s[k]->intf.iInterface = 1;
        s[k]->out.bLength = USB_DT_ENDPOINT_SIZE;
        s[k]->out.bDescriptorType = USB_DT_ENDPOINT;
        s[k]->out.bEndpointAddress = 1 | USB_DIR_OUT;
        s[k]->out.bmAttributes = USB_ENDPOINT_XFER_BULK;
        s[k]->out.wMaxPacketSize = htole16(k ? 512 : 64);
    }
    if ((int)sizeof d > max)
        return 0;
    memcpy(buf, &d, sizeof d);
    return (int)sizeof d;
}

int link_strings(uint8_t *buf, int max)
{
    struct {
        struct usb_functionfs_strings_head head;
        __le16 code;
        char str[sizeof LINK_NAME];
    } __attribute__((packed)) s;
    memset(&s, 0, sizeof s);
    s.head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    s.head.length = htole32(sizeof s);
    s.head.str_count = htole32(1);
    s.head.lang_count = htole32(1);
    s.code = htole16(0x0409);
    memcpy(s.str, LINK_NAME, sizeof LINK_NAME);
    if ((int)sizeof s > max)
        return 0;
    memcpy(buf, &s, sizeof s);
    return (int)sizeof s;
}

/* ------------------------------------------------------------ files */

static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}

static int get(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t r = read(fd, buf, (size_t)n - 1);
    close(fd);
    if (r < 0)
        r = 0;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
        buf[--r] = 0;
    return r > 0;
}

/* ------------------------------------------------------------ leader */

int link_find(const char *sysfs, LinkDev *out, int max)
{
    DIR *d = opendir(sysfs);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
            continue;
        char p[1024], v[32];
        snprintf(p, sizeof p, "%s/%s/idVendor", sysfs, e->d_name);
        if (!get(p, v, sizeof v) || strtol(v, NULL, 16) != LINK_VID)
            continue;
        snprintf(p, sizeof p, "%s/%s/idProduct", sysfs, e->d_name);
        if (!get(p, v, sizeof v) || strtol(v, NULL, 16) != LINK_PID)
            continue;
        /* the interface: ours, not some other gadget with the same ids */
        char in[800];
        snprintf(in, sizeof in, "%s/%s/%s:1.0", sysfs, e->d_name, e->d_name);
        int cls = -1, sub = -1, pro = -1;
        snprintf(p, sizeof p, "%s/bInterfaceClass", in);
        if (get(p, v, sizeof v))
            cls = (int)strtol(v, NULL, 16);
        snprintf(p, sizeof p, "%s/bInterfaceSubClass", in);
        if (get(p, v, sizeof v))
            sub = (int)strtol(v, NULL, 16);
        snprintf(p, sizeof p, "%s/bInterfaceProtocol", in);
        if (get(p, v, sizeof v))
            pro = (int)strtol(v, NULL, 16);
        if (cls != USB_CLASS_VENDOR_SPEC || sub != LINK_SUBCLASS || pro != LINK_PROTOCOL)
            continue;
        /* its bulk OUT endpoint, as the device controller numbered it */
        int ep = -1;
        DIR *di = opendir(in);
        struct dirent *f;
        while (di && (f = readdir(di)) && ep < 0) {
            if (strncmp(f->d_name, "ep_", 3))
                continue;
            char dir[16] = "", type[16] = "";
            snprintf(p, sizeof p, "%s/%s/direction", in, f->d_name);
            get(p, dir, sizeof dir);
            snprintf(p, sizeof p, "%s/%s/type", in, f->d_name);
            get(p, type, sizeof type);
            snprintf(p, sizeof p, "%s/%s/bEndpointAddress", in, f->d_name);
            if (!strcmp(dir, "out") && !strcmp(type, "Bulk") && get(p, v, sizeof v))
                ep = (int)strtol(v, NULL, 16);
        }
        if (di)
            closedir(di);
        int bus = 0, dev = 0;
        snprintf(p, sizeof p, "%s/%s/busnum", sysfs, e->d_name);
        if (get(p, v, sizeof v))
            bus = atoi(v);
        snprintf(p, sizeof p, "%s/%s/devnum", sysfs, e->d_name);
        if (get(p, v, sizeof v))
            dev = atoi(v);
        if (ep < 0 || bus <= 0 || dev <= 0)
            continue;
        out[n].bus = bus, out[n].dev = dev, out[n].ep = ep;
        snprintf(out[n].path, sizeof out[n].path, "%s/%s", sysfs, e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

/* ---------------------------------------------------------- follower */

static int ep0 = -1, ep1 = -1, set_up;
static volatile int running;
static volatile uint64_t last_in;
static pthread_t th;

int link_follow_state(void)
{
    if (!set_up)
        return 0;
    uint64_t t = last_in;
    return t && now_us() - t < 2000000 ? 2 : 1;
}

/* the gadget as it would be left by a run that didn't end cleanly, or ours now */
static void teardown(void)
{
    put(GADGET "/UDC", "\n");
    if (ep1 >= 0)
        close(ep1);
    if (ep0 >= 0)
        close(ep0);
    ep0 = ep1 = -1;
    umount(FFS_DIR);
    rmdir(FFS_DIR);
    unlink(GADGET "/configs/c.1/ffs." FFS_NAME);
    rmdir(GADGET "/configs/c.1/strings/0x409");
    rmdir(GADGET "/configs/c.1");
    rmdir(GADGET "/functions/ffs." FFS_NAME);
    rmdir(GADGET "/strings/0x409");
    rmdir(GADGET);
}

/* the device controller, if no other gadget (TrimUI's own) has it */
static int free_udc(char *udc, int n)
{
    DIR *d = opendir("/sys/class/udc");
    struct dirent *e;
    udc[0] = 0;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(udc, (size_t)n, "%s", e->d_name);
            break;
        }
    if (d)
        closedir(d);
    if (!udc[0])
        return 0;
    d = opendir("/sys/kernel/config/usb_gadget");
    int busy = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "trimuilink"))
            continue;
        char p[512], v[64];
        snprintf(p, sizeof p, "/sys/kernel/config/usb_gadget/%s/UDC", e->d_name);
        if (get(p, v, sizeof v)) {
            fprintf(stderr, "link: the bottom port is in use (gadget %s), not linking\n", e->d_name);
            busy = 1;
        }
    }
    if (d)
        closedir(d);
    return !busy;
}

static void on_wake(int s) { (void)s; }

static void *loop(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    while (running) {
        if (ep1 < 0 && (ep1 = open(FFS_DIR "/ep1", O_RDONLY | O_CLOEXEC)) < 0) {
            usleep(200000);
            continue;
        }
        /* blocks until a leader sends (or the port is let go, or link_follow_stop wakes it) */
        ssize_t r = read(ep1, buf, sizeof buf);
        if (r > 0) {
            uint64_t t = now_us();
            if (!last_in || t - last_in > 2000000)
                midi_notice("LINK: %.40s", "FOLLOWING THE LEADER BRICK");
            last_in = t;
            midi_feed_at(buf, (int)r, t);
        }
        else if (r < 0 && errno != EINTR) {
            close(ep1);
            ep1 = -1;
            usleep(200000);
        }
    }
    return NULL;
}

int link_follow_start(void)
{
    const char *v = getenv("RS_LINK");
    if (v && *v == '0') {
        fprintf(stderr, "link: off (RS_LINK=0)\n");
        return 0;
    }
    char udc[64];
    if (access("/sys/kernel/config/usb_gadget", W_OK) != 0) {
        fprintf(stderr, "link: no USB gadget configfs here, not linking\n");
        return 0;
    }
    teardown(); /* what a run that didn't end cleanly left */
    if (!free_udc(udc, sizeof udc)) {
        if (!udc[0])
            fprintf(stderr, "link: no USB device controller, not linking\n");
        return 0;
    }
    int ok = mkdir(GADGET, 0755) == 0 && put(GADGET "/idVendor", "0x1d6b") && put(GADGET "/idProduct", "0x0104") &&
             put(GADGET "/bcdUSB", "0x0200") && mkdir(GADGET "/strings/0x409", 0755) == 0 &&
             put(GADGET "/strings/0x409/manufacturer", "TrimUI") &&
             put(GADGET "/strings/0x409/product", LINK_NAME) && mkdir(GADGET "/configs/c.1", 0755) == 0 &&
             mkdir(GADGET "/configs/c.1/strings/0x409", 0755) == 0 &&
             put(GADGET "/configs/c.1/strings/0x409/configuration", "clock link") &&
             put(GADGET "/configs/c.1/MaxPower", "100") && mkdir(GADGET "/functions/ffs." FFS_NAME, 0755) == 0 &&
             symlink(GADGET "/functions/ffs." FFS_NAME, GADGET "/configs/c.1/ffs." FFS_NAME) == 0;
    if (ok) {
        mkdir(FFS_DIR, 0755);
        ok = mount(FFS_NAME, FFS_DIR, "functionfs", 0, NULL) == 0;
    }
    uint8_t b[256];
    int n;
    ok = ok && (ep0 = open(FFS_DIR "/ep0", O_RDWR | O_CLOEXEC)) >= 0 && (n = link_descriptors(b, sizeof b)) > 0 &&
         write(ep0, b, (size_t)n) == n && (n = link_strings(b, sizeof b)) > 0 && write(ep0, b, (size_t)n) == n;
    char nl[80];
    snprintf(nl, sizeof nl, "%s\n", udc);
    ok = ok && put(GADGET "/UDC", nl);
    if (!ok) {
        fprintf(stderr, "link: could not set up the bottom port (%s)\n", strerror(errno));
        teardown();
        return 0;
    }
    /* a signal (no restart) wakes the reader for link_follow_stop */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_wake;
    sigaction(SIGUSR2, &sa, NULL);
    set_up = running = 1;
    last_in = 0;
    if (pthread_create(&th, NULL, loop, NULL) != 0) {
        set_up = running = 0;
        teardown();
        return 0;
    }
    fprintf(stderr, "link: the bottom port is a %s on %s: a Brick with a hub in its top port can lead this one\n",
            LINK_NAME, udc);
    return 1;
}

void link_follow_stop(void)
{
    if (!set_up)
        return;
    running = 0;
    put(GADGET "/UDC", "\n"); /* the leader sees it go */
    pthread_kill(th, SIGUSR2);
    pthread_join(th, NULL);
    teardown();
    set_up = 0;
}
