RadioScan, an internet-radio synthesizer for the TrimUI Brick, Brick Hammer and Brick Pro running MustardOS.

**Install on MustardOS**

Copy `RadioScan-mustardos.muxapp` into the `ARCHIVE` folder of the first SD card, install it from **Applications →
Archive Manager**, turn Wi-Fi on, then open **Applications → RadioScan**. Press MENU twice to quit. The package works on all three devices.

Or, with the TrimUI's SSH server on, run this on a computer on the same network:

```sh
curl -fsSL https://raw.githubusercontent.com/sydilaxe/Trimui-RadioScan-dev/main/install.sh | bash
```

**Change: MustardOS only**
- RadioScan is now built only for MustardOS. `RadioScan-trimui.zip` is no longer released; a copy installed from it
  won't find its update, so install the `.muxapp` instead.

RadioScan checks for new releases when it starts and offers to update itself.

**New: USB MIDI**
- A MIDI keyboard plays the voices' pitches: channel 1 voice A, channel 2 voice B. Middle C is the voice's own
  PITCH, each key a semitone from it, and letting go of every key gives the PITCH back.
- The important voice, engine and mix settings each have a MIDI CC, on any channel and from any page: 14-25
  voice A, 102-113 voice B, 3 and 9 their rhythms, 26-31, 85-90 and 114-119 the engine and mix.
- `docs/MIDI.md` is the MIDI implementation chart.
**Fix: the black screen on MustardOS, and F1 / F2 on the Brick Pro**
- The picture is now sent to the screen opaque. Its pixels went out with their alpha (transparency) at 0, which
  TrimUI OS ignores but MustardOS's display shows, so on MustardOS nothing the app drew was visible.
- After MustardOS's screen wake the app makes its renderer again, and it rebuilds the picture whenever SDL reports
  it lost or drawing fails. The log's `SCREEN` lines record each rebuild.
- F1 and F2 (the two lit buttons under the screen) now work on a Brick Pro under MustardOS, which sends them as
  buttons 0 and 1; the sticks' clicks still do the same. Volume up and down (2 and 3) only change the volume
  again; they had been read as F1 and F2.

**New: deeper logs, to find clicks and tune the app**
- `log.txt` adds a **PROC** line every 10 s: the processor's time by thread (the audio thread marked), memory and
  its peak, page faults, free memory, and how often the audio thread was preempted.
- **JUMP** lines: the app watches its own output for clicks and logs each with its time. A click heard with no
  JUMP and no dropout comes from after the app, in the sound server.
- STAT counts late audio calls (dropout or not) and jumps; the log names SDL's audio driver and what the device took.
- `sysinfo.txt` records the sound path: PipeWire's devices, rate and quantum, the mixer, and every 10 s PipeWire's
  nodes with their xrun (ERR) counts.

**New: MustardOS, and logs for tuning**
- A package for MustardOS on the Brick, Brick Hammer and Brick Pro, installed from the Archive Manager. RadioScan
  reads MustardOS's controller, so every button (and the Brick Pro's stick) works, and it updates
  itself from the MustardOS package.
- The screen no longer stays dark on a Brick Pro under MustardOS.
- `log.txt` now has a performance line every 10 s (audio load, dropouts, the screen, the processor's clock and
  temperature) with hints on what to change, and `sysinfo.txt` a report of the system. `RS_LOG=2` in `launch.sh`
  logs every second and every button press.

**New on TrimUI**
- Ported from the Miyoo Mini Plus: the same instrument and the same sound, on SDL2, the pages scaled 1.6x to fill
  the 1024x768 screen. Settings and station lists from the Miyoo load as they are.
- Half the Miyoo's audio delay (1024-frame buffers), memory locked.
- **USB audio:** a USB sound card or DAC takes over the output while it is plugged in.
- **Brick Pro:** the left stick sweeps the stations by hand, like a tuning dial (left / right voice A, up / down
  voice B; let go and each glides home to its selected station). The right stick plays morph (left / right) and
  the filter (up / down) by hand and springs back; in RAW it moves the crossfader. Clicking the sticks does F1
  and F2.
- Radio scan's crossfades now fade in from the new station's buffered audio, not from silence, and no longer stall
  after the first switch on a `.wav` station.
- **Updates itself** from this repo's releases.

If something is off, check `log.txt` in the app folder (it lists the controller, screen and sound device found, and
every 10 s the audio load, dropouts and the processor's state, with hints) and `sysinfo.txt`, and try these in
`launch.sh` (uncomment the line):
- Crackles or dropouts: `export RS_BUFFER=2048`.
- A turned picture: `export RS_ROTATE=180`.
- Buttons in the wrong place: `export RS_JOYMAP=A=1,B=0` (`log.txt` names any button it doesn't know).

MP3 streams only (AAC, Ogg and HLS stations are skipped). Your own `.wav` files can be voices too: put them in
`wav/` in the app folder. Some preset station URLs may have changed; replace them from the RADIO page.

See the README for controls and features.
