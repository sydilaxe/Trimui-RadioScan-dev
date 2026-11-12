# AGENTS.md

RadioScan is a two-voice internet-radio synthesizer for the TrimUI Brick, Brick Hammer and Brick Pro running
MustardOS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the Miyoo Mini Plus
version (sydilaxe/Miyoo-RadioScan-dev) the way Columbia Street and Jaffedrone were
(sydilaxe/Trimui-Columbiastreet-dev, sydilaxe/Trimui-Jaffedrone-dev).

`make` cross-compiles `build/trimui/radioscan` with zig (aarch64, glibc 2.28) against a stand-in for the device's
SDL2 (`tools/mksdlstub.py`; the headers are in `third_party/SDL2-2.30.9-headers`). `make package` zips it as
`build/RadioScan-mustardos.muxapp`, the only package built, holding the app folder (`Apps/RadioScan` in this repo)
as `RadioScan/` plus `mustardos/` (`mux_launch.sh`; `wake_screen.sh`, run through `RS_SCREEN_WAKE` once the window is
up, as the Brick Pro's screen can stay dark on MustardOS; the list glyph). One build serves all three devices. Only
MustardOS is built for: no TrimUI OS or Knulli packages. `make desktop` builds
for this computer against the system SDL2; CI runs its tests (`.github/workflows/build.yml`). Without zig on the
path, `pip install ziglang==0.14.1`.

`mux_launch.sh` holds MustardOS's idle off while the app is open (its Idle Mute would stop the sound two
minutes after the last press): `idle_inhibit` re-set every 2 s and `caffeine`, given back on exit. The block
is the same in Jaffedrone, Columbia Street, HI5, RadioScan and Conduit; keep the copies the same.

The platform layer lives in `src/main.c` (the screen, input and sound device sections and `main`): the 640x480
pages are drawn into a software surface as on the Miyoo and scaled to the screen with an SDL2 renderer; the TrimUI's
buttons come in as MustardOS's "muOS-Keys" joystick (any other controller is ignored), with the Brick or
Brick Pro numbering told apart by the controller's product id, else MustardOS's board name (`RS_JOYMAP` remaps
them); the sound device follows a USB sound card when one is
plugged in. Everything else (the synth in `src/dsp.c`, streams, stations, Wi-Fi sync, the updater) is shared with
the Miyoo version: keep it the same so changes can be carried across in either direction. The updater differs in
one place: it fetches the release file `RadioScan-mustardos.muxapp`.

`src/midi.c` is USB MIDI: notes and CCs for the main thread, and the MIDI clock in and out, run from the MIDI thread
through the main program's clock (`MidiClock`, the same calls Wi-Fi sync makes). `src/link.c` is the clock link
between Bricks (a follower's bottom port as a USB gadget through configfs and FunctionFS, found and driven by the
leader through usbfs); keep it and the link's messages in `midi.c` the same as Columbia Street's, HI5's and
Jaffedrone's (`link.c` differs only in `RS_LINK` and calling `midi_feed_at`), so the apps link to each other.
`radioscan --miditest` checks the clock and the link.

`src/perflog.c` (the performance log in `log.txt`, `RS_LOG`) and `Apps/RadioScan/sysinfo.sh` (`sysinfo.txt`) are
shared with HI5 (and `sysinfo.sh` with Jaffedrone); keep the copies the same. So is `src/ringleds.c`, the Brick
Pro's lights. The audio callback only calls
`perf_audio_begin` / `perf_audio_end`; the event log is written out with each STAT line.

When you change a control or a screen, update the README's Controls section and the footer hints in `src/main.c`.
When you change the input code, keep `radioscan --joytest` passing (it covers MustardOS's numbering too). When you change the updater or the package
layout, run the self-update test from the workflow (it serves the .muxapp with `tools/fake_github.py`).

The app updates itself from the latest GitHub release of this repo (`src/update.c`, HTTPS through the Mbed TLS in
`third_party/mbedtls`). Releases are built as the version in their tag (`make VERSION=x.y.z`); `RS_VERSION` in
`src/update.h` is only the version of untagged builds.
