RadioScan, an internet-radio synthesizer for the TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS).

**Install**

Unzip `RadioScan-trimui.zip` at the root of the SD card (it contains `Apps/RadioScan/`), turn Wi-Fi on, then open
**Apps → RadioScan**. Press MENU twice to quit. The same zip works on all three devices.

Or, with the TrimUI's SSH server on, run this on a computer on the same network:

```sh
curl -fsSL https://raw.githubusercontent.com/sydilaxe/Trimui-RadioScan-dev/main/install.sh | bash
```

RadioScan checks for new releases when it starts and offers to update itself.

**New on TrimUI**
- Ported from the Miyoo Mini Plus: the same instrument and the same sound, on SDL2, the pages scaled 1.6x to fill
  the 1024x768 screen. Settings and station lists from the Miyoo load as they are.
- Half the Miyoo's audio delay (1024-frame buffers), memory locked.
- **USB audio:** a USB sound card or DAC takes over the output while it is plugged in.
- **Brick Pro:** the left stick sweeps the stations by hand, like a tuning dial (left / right voice A, up / down
  voice B; let go and each glides home to its selected station). The right stick plays morph (left / right) and
  the filter (up / down) by hand and springs back; in RAW it moves the crossfader. L3 taps the tempo, R3 grabs a
  fresh slice.
- Radio scan's crossfades now fade in from the new station's buffered audio, not from silence, and no longer stall
  after the first switch on a `.wav` station.
- **Updates itself** from this repo's releases.

If something is off, check `log.txt` in the app folder (it lists the controller, screen and sound device found, and
notes any late audio buffers), and try these in `launch.sh` (uncomment the line):
- Crackles or dropouts: `export RS_BUFFER=2048`.
- A turned picture: `export RS_ROTATE=180`.
- Buttons in the wrong place: `export RS_JOYMAP=A=1,B=0` (`log.txt` names any button it doesn't know).

MP3 streams only (AAC, Ogg and HLS stations are skipped). Your own `.wav` files can be voices too: put them in
`Apps/RadioScan/wav/`. Some preset station URLs may have changed; replace them from the RADIO page.

See the README for controls and features.
