#!/bin/sh
# HELP: RadioScan: a synthesizer playing internet radio. MENU twice quits.
# ICON: radioscan
# GRID: RadioScan

# MustardOS starts an app with this script, passing the app's folder. It sets up
# the app the way MustardOS's own apps do, then runs launch.sh.

APP_DIR="${1:-$(dirname "$0")}"

if [ -f /opt/muos/script/var/retro.sh ]; then
    . /opt/muos/script/var/retro.sh
    command -v SETUP_APP >/dev/null 2>&1 && SETUP_APP radioscan ""
elif [ -f /opt/muos/script/var/func.sh ]; then
    # older muOS releases
    . /opt/muos/script/var/func.sh
    echo app >/tmp/act_go
    command -v SETUP_SDL_ENVIRONMENT >/dev/null 2>&1 && SETUP_SDL_ENVIRONMENT
    command -v SET_VAR >/dev/null 2>&1 && SET_VAR "system" "foreground_process" "radioscan"
fi

cd "$APP_DIR" || exit 1
# the screen can stay dark once the app's window opens: the app runs this then
export RS_SCREEN_WAKE="$PWD/wake_screen.sh"
# Keep MustardOS awake while RadioScan is open. Two minutes without a press, its idle
# handler dims the screen, drops the processor to its slowest and (with Idle Mute on, the
# default) mutes the sound; later it can put the device to sleep. Music plays on with the
# hands off, so: idle_inhibit 1 (no dimming, no muting, no sleep), set again every 2 s as
# MustardOS's idle watcher clears it when the buttons go quiet, and caffeine (no sleep).
# Both are given back when RadioScan closes. Create no_keep_awake in this folder to leave
# MustardOS's idle as it is. (The same in Jaffedrone, Columbia Street, HI5, RadioScan and
# Conduit.)
INHIBIT() {
    if command -v SET_VAR >/dev/null 2>&1; then
        SET_VAR "system" "idle_inhibit" "$1"
    elif [ -d /opt/muos/config/system ]; then
        printf '%s' "$1" >/opt/muos/config/system/idle_inhibit
    fi
}
KEEP= CAFF=
if [ ! -e no_keep_awake ]; then
    # (a caffeine already on, the user's, stays on)
    [ -e /run/muos/caffeine ] || { : >/run/muos/caffeine 2>/dev/null && CAFF=1; }
    (while :; do INHIBIT 1; sleep 2; done) &
    KEEP=$!
fi

./launch.sh

if [ -n "$KEEP" ]; then
    kill "$KEEP" 2>/dev/null
    INHIBIT 0
    [ -n "$CAFF" ] && rm -f /run/muos/caffeine
fi
