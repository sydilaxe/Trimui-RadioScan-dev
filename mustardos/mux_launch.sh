#!/bin/sh
# HELP: RadioScan: a synthesizer playing internet radio. MENU twice quits.
# ICON: radioscan
# GRID: RadioScan

# MustardOS starts an app with this script, passing the app's folder. It sets up
# the app the way MustardOS's own apps do, then runs launch.sh, which is shared
# with the TrimUI OS package. The app sees this file and so updates itself from
# the MustardOS package.

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
./launch.sh
