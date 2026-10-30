#!/bin/sh
# Wakes the screen once RadioScan's window is up, the way MustardOS does
# when it wakes from sleep. On the Brick Pro the screen can stay dark after an
# app opens its window (the app runs, nothing shows) until the power button
# sleeps and wakes the device; this does the waking part without the sleep.
# The app runs it through RS_SCREEN_WAKE, which mux_launch.sh sets.

BLANK=/sys/class/graphics/fb0/blank
BRIGHT_SH=/opt/muos/script/device/bright.sh
BRIGHT=$(cat /opt/muos/config/settings/general/brightness 2>/dev/null)

wake() {
    # off and on again: the framebuffer's blank state, as sleep and wake set it
    [ -w "$BLANK" ] && { echo 4 >"$BLANK"; echo 0 >"$BLANK"; } 2>/dev/null
    echo 0 >/tmp/fb_blank 2>/dev/null
    rm -f /tmp/mux_blank
    # the saved brightness, applied again (transient: the setting isn't rewritten)
    [ -x "$BRIGHT_SH" ] && [ -n "$BRIGHT" ] && "$BRIGHT_SH" "$BRIGHT" transient
}

wake
# once more a little later, in case the display was still settling
sleep 1
wake
