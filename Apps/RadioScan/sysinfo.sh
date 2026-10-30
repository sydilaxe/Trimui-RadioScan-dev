#!/bin/sh
# A report of the system, for troubleshooting and tuning the app on a TrimUI Brick, Brick Hammer
# or Brick Pro (TrimUI OS or MustardOS): the firmware and board, the processor, memory, the sound
# cards, USB and input devices and what is running at start, then a sample every 10 s while the
# app runs (up to an hour): the system load, the processor's clock and temperature, and the
# busiest processes. Written to sysinfo.txt in the app folder; the app's own figures (audio load,
# dropouts, the screen) are in log.txt.
#
#   sysinfo.sh start NAME    write the report and start sampling (launch.sh, before the app)
#   sysinfo.sh stop NAME     stop sampling (launch.sh, after the app)
#
# Shared by the TrimUI ports (Jaffedrone, HI5, RadioScan); keep the copies the same.

cmd=$1
name=${2:-app}
flag=/tmp/${name}_running
out=sysinfo.txt
CPU=/sys/devices/system/cpu

if [ "$cmd" = stop ]; then
    rm -f "$flag"
    exit 0
fi

{
    echo "== $name start $(date)"
    if [ -d /opt/muos ]; then
        echo "firmware: MustardOS $(cat /opt/muos/config/system/version 2>/dev/null)" \
            "$(cat /opt/muos/config/version.txt 2>/dev/null | head -n 2 | tr '\n' ' ')"
        echo "board: $(cat /opt/muos/device/config/board/name 2>/dev/null)"
    else
        echo "firmware: TrimUI OS $(cat /etc/version 2>/dev/null) $(cat /usr/trimui/version 2>/dev/null)"
    fi
    echo "DEVICE=$DEVICE TRIMUI_MODEL=$TRIMUI_MODEL"
    echo "build: $(cat DEVICE 2>/dev/null || echo "one for every device") v$(cat VERSION 2>/dev/null)," \
        "$([ -f mux_launch.sh ] && echo "MustardOS package" || echo "TrimUI OS package")"
    uname -a
    grep -i "processor\|model name\|bogomips\|hardware\|features" /proc/cpuinfo
    echo "cores online: $(cat $CPU/online)"
    for f in scaling_governor scaling_cur_freq scaling_min_freq scaling_max_freq scaling_available_frequencies \
        scaling_available_governors; do
        echo "$f: $(cat $CPU/cpu0/cpufreq/$f 2>/dev/null)"
    done
    for z in /sys/class/thermal/thermal_zone*; do
        [ -d "$z" ] && echo "$z: $(cat "$z/type" 2>/dev/null) $(cat "$z/temp" 2>/dev/null)"
    done
    echo "-- memory"; free; cat /proc/loadavg
    echo "-- display"; cat /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/blank 2>/dev/null
    echo "-- sound cards"; cat /proc/asound/cards 2>/dev/null; ls /dev/snd 2>/dev/null
    echo "-- sound: environment"
    echo "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR PIPEWIRE_RUNTIME_DIR=$PIPEWIRE_RUNTIME_DIR" \
        "SDL_AUDIODRIVER=$SDL_AUDIODRIVER ALSA_CONFIG=$ALSA_CONFIG PIPEWIRE_LATENCY=$PIPEWIRE_LATENCY"
    if command -v wpctl >/dev/null 2>&1; then
        echo "-- sound: PipeWire"
        XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run} wpctl status 2>&1 | head -n 60
        echo "default sink: $(XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run} wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>&1)"
        # the graph's clock: its rate and quantum (the frames it moves at a time) and their limits
        XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run} pw-metadata -n settings 2>&1 | head -n 20
    fi
    echo "-- sound: mixer"; amixer -c 0 2>/dev/null | grep -E "^Simple|Playback|Mono:|Front" | head -n 40
    echo "-- usb"; cat /sys/kernel/debug/usb/devices 2>/dev/null | grep -E "^[TPS]:"; ls /sys/bus/usb/devices 2>/dev/null
    echo "-- input"; grep -E "^[NH]:" /proc/bus/input/devices 2>/dev/null
    echo "-- network"; ip -o addr 2>/dev/null | grep -v " lo " || ifconfig 2>/dev/null | grep -E "^[a-z]|inet "
    echo "-- processes"; ps
    echo "-- interrupts"; cat /proc/interrupts
} > "$out" 2>&1

touch "$flag"
(
    n=0
    while [ -f "$flag" ] && [ $n -lt 360 ]; do
        sleep 10
        n=$((n + 1))
        {
            echo "== sample $n at $(cut -d' ' -f1 /proc/uptime) s: load $(cat /proc/loadavg)," \
                "cpu $(cat $CPU/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) kHz" \
                "($(cat $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null))," \
                "temp $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)," \
                "cores $(cat $CPU/online)"
            top -b -n 1 2>/dev/null | head -14
            # PipeWire's nodes: quantum, rate, how long each cycle took (BUSY / WAIT) and ERR, its xruns (the
            # sound server itself ran short: a click heard with no JUMP or dropout in log.txt is one of these)
            if command -v pw-top >/dev/null 2>&1; then
                echo "-- PipeWire nodes (ERR: xruns so far)"
                XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run} timeout 5 pw-top -b -n 2 2>&1 | tail -n 12
            fi
            if [ $n -eq 1 ]; then
                # what the app's sound goes through, once it is playing
                pid=$(pidof "$name" 2>/dev/null | cut -d' ' -f1)
                [ -n "$pid" ] && echo "-- $name's sound libraries:" &&
                    grep -oE "/[^ ]*(SDL2|asound|pipewire|pulse|spa)[^ ]*" /proc/$pid/maps 2>/dev/null | sort -u
                if command -v wpctl >/dev/null 2>&1; then
                    echo "-- PipeWire while playing"
                    XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run} wpctl status 2>&1 | head -n 60
                fi
            fi
        } >> "$out" 2>&1
    done
) >/dev/null 2>&1 &
