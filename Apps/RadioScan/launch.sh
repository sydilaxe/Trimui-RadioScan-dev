#!/bin/sh
# RadioScan launcher for the TrimUI Brick, Brick Hammer and Brick Pro under MustardOS,
# run through mux_launch.sh.
# All four cores stay online, at full speed, while it plays.
cd "$(dirname "$0")"

CPU=/sys/devices/system/cpu
gov_was=$(cat $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null)
for c in 0 1 2 3; do
    [ -f $CPU/cpu$c/online ] && echo 1 > $CPU/cpu$c/online 2>/dev/null
done
echo performance > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null

export LD_LIBRARY_PATH="/usr/trimui/lib:/usr/lib:$LD_LIBRARY_PATH"
export RADIOSCAN_DIR="$(pwd)"

# Optional overrides (remove the # to use one):
# export RS_BUFFER=2048     # bigger audio buffers if you hear crackle (1024 by default)
# export RS_RATE=44100      # a lower sample rate: a little less work (48000 by default)
# export RS_RT=0            # don't give the audio thread real-time priority (to compare, if the sound stalls)
# export RS_UPDATE=0        # don't check GitHub for new versions
# export RS_MIDI=0          # no USB MIDI (and no clock link);  RS_MIDI_CLOCK=0 / RS_MIDI_FOLLOW=0: never send / follow clock
# export RS_LINK=0          # leave the bottom USB port alone (no clock link from a leading Brick)
# export RS_AUDIO=internal  # always the speaker and headphones (default: a USB audio device when one is plugged in)
# export RS_AUDIO=<name>    # the output device whose name contains <name> (see log.txt)
# export RS_ROTATE=180      # turn the picture, for a firmware whose screen is turned
# export RS_JOYMAP=A=1,B=0  # another firmware's button numbers (see log.txt for buttons it doesn't know)
# export RS_LEDS=0          # Brick Pro: leave the lights alone;  RS_LED_BAR=0: keep the top bar dark
# export RS_LED_BRIGHTNESS=30  # Brick Pro: the lights' brightness, 1..60 (60 by default)
# export RS_LOG=2           # the performance log in log.txt every second, and every button press (1: every 10 s,
#                           # the default; 0: off)
# export RS_SYSINFO=0       # no sysinfo.txt, the system report (see sysinfo.sh)

[ "${RS_SYSINFO:-1}" != 0 ] && ./sysinfo.sh start radioscan

# exit status 75: the app has just updated itself, so start the new version
while :; do
    ./radioscan > log.txt 2>&1
    [ $? -eq 75 ] || break
done

./sysinfo.sh stop radioscan
[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
