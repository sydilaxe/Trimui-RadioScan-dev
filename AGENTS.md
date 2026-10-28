# AGENTS.md

RadioScan is a two-voice internet-radio synthesizer for the TrimUI Brick, Brick Hammer and Brick Pro running
TrimUI OS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the Miyoo Mini Plus
version (sydilaxe/Miyoo-RadioScan-dev) the way Columbia Street and Jaffedrone were
(sydilaxe/Trimui-Columbiastreet-dev, sydilaxe/Trimui-Jaffedrone-dev).

`make` cross-compiles `build/trimui/radioscan` with zig (aarch64, glibc 2.28) against a stand-in for the device's
SDL2 (`tools/mksdlstub.py`; the headers are in `third_party/SDL2-2.30.9-headers`). `make package` zips it as
`build/RadioScan-trimui.zip` holding `Apps/RadioScan`. One build serves all three devices. `make desktop` builds
for this computer against the system SDL2; CI runs its tests (`.github/workflows/build.yml`). Without zig on the
path, `pip install ziglang==0.14.1`.

The platform layer lives in `src/main.c` (the screen, input and sound device sections and `main`): the 640x480
pages are drawn into a software surface as on the Miyoo and scaled to the screen with an SDL2 renderer; the TrimUI's
buttons come in as a joystick (`RS_JOYMAP` remaps them); the sound device follows a USB sound card when one is
plugged in. Everything else (the synth in `src/dsp.c`, streams, stations, Wi-Fi sync, the updater) is shared with
the Miyoo version: keep it the same so changes can be carried across in either direction.

When you change a control or a screen, update the README's Controls section and the footer hints in `src/main.c`.
When you change the input code, keep `radioscan --joytest` passing. When you change the updater or the package
layout, run the self-update test from the workflow (it serves a zip with `tools/fake_github.py`).

The app updates itself from the latest GitHub release of this repo (`src/update.c`, HTTPS through the Mbed TLS in
`third_party/mbedtls`). Releases are built as the version in their tag (`make VERSION=x.y.z`); `RS_VERSION` in
`src/update.h` is only the version of untagged builds.
