# RadioScan MIDI implementation chart

RadioScan reads MIDI over USB: plug a class-compliant keyboard or controller into the TrimUI's USB host port (the
Brick's top USB-C, through an OTG adapter or hub). Ports are found as they are plugged in, and a message names each
one. Everything below works from any page.

## Summary

| Function | Transmitted | Recognized | Remarks |
|---|---|---|---|
| Basic channel | none | 1-2 notes, 1-16 CCs | Notes: channel 1 plays voice A, channel 2 voice B. CCs are heard on any channel. |
| Mode | none | Omni off for notes, omni on for CCs | |
| Note number | none | 0-127 | The voice's pitch: middle C (60) is its own PITCH setting, each key a semitone from it, up to two octaves either way. |
| Velocity | none | no | |
| Aftertouch | none | no | |
| Pitch bend | none | no | |
| Control change | none | yes | The settings in the CC table below. |
| Program change | none | no | |
| System exclusive | yes | yes | Only the Brick link's position (`F0 7D 'T' 'L' ... F7`, 14 bytes, about 50 a second, to linked Bricks only); others pass by. |
| Song position | none | yes | Places the rhythms, in 16ths (not with MIDI Start/Stop set to IGNORE). |
| Song select | none | no | |
| Clock | yes | yes | Out: 24 a quarter note while the rhythms run and no clock comes in, timed to the sound as it is heard. In: the tempo follows it, and its phase is held. RHYTHM's MIDI Clock picks the way (AUTO, SEND, FOLLOW, OFF). `RS_MIDI_CLOCK=0` sends none; `RS_MIDI_FOLLOW=0` ignores it. |
| Start, Continue, Stop | yes | yes | Out with the rhythms. In: START from the top, CONTINUE, STOP; ignored with MIDI Start/Stop set to IGNORE. |
| Active sensing, reset | none | no | |

`RS_MIDI=0` in `launch.sh` turns MIDI off.

## Notes

| Channel | Plays |
|---|---|
| 1 | Voice A's pitch |
| 2 | Voice B's pitch |

While keys are held, the voice plays at the last key's pitch; letting that key go returns to the key still held
before it. With every key let go, the voice goes back to its own PITCH setting, so playing never changes the song.
A key changes the pitch of whatever the voice plays (the station live, its loop, grains or the oscillator).

## Settings by CC

The important voice, engine and mix settings, on any channel. A CC's 0-127 spans the setting's whole range; a size
steps evenly in octaves, and a choice or an on / off takes the nearest. CC 14-25 are voice A, 102-113 the same
settings for voice B; 3 and 9 turn their rhythms on and off; 26-31, 85-90 and 114-119 are the engine and mix. The
numbers are ones MIDI leaves free. While another unit leads by Wi-Fi sync, it sets the tempo and CC 117 is ignored.

<!-- cc table -->
| CC | Setting | Range |
|---|---|---|
| 3 | Voice A Rhythm | 0-63 OFF, 64-127 ON |
| 9 | Voice B Rhythm | 0-63 OFF, 64-127 ON |
| 14 | Voice A Engine | 0-21 LIVE, 22-63 LOOP, 64-105 WAVE, 106-127 GRAIN |
| 15 | Voice A Pitch | -24 to 24 (st) |
| 16 | Voice A Size | 1 to 2700 (ms), in even steps of octaves |
| 17 | Voice A Texture | 0 to 1 |
| 18 | Voice A Direction | 0-31 FORWARD, 32-95 REVERSE, 96-127 BOTH |
| 19 | Voice A Focus | 0-31 ALL SOUND, 32-95 VOICES ONLY, 96-127 MUSIC ONLY |
| 20 | Voice A Level | 0 to 2 (lvl) |
| 21 | Voice A Mute | 0-63 OFF, 64-127 ON |
| 22 | Voice A Pan | -1 to 1 (pan) |
| 23 | Voice A Drive | 0 to 1 |
| 24 | Voice A Oscillator | 0-15 OFF, 16-47 SINE, 48-79 TRIANGLE, 80-111 SAW, 112-127 SQUARE |
| 25 | Voice A Osc Mix | 0 to 1 |
| 26 | Morph A>B | 0 to 1 (morph) |
| 27 | Filter | -1 to 1 (tone) |
| 28 | Resonance | 0 to 1 |
| 29 | FM | 0 to 1 |
| 30 | Ring Mod | 0 to 1 |
| 31 | Converse | 0 to 1 |
| 85 | Vocode | 0-31 OFF, 32-95 A SPEAKS THRU B, 96-127 B SPEAKS THRU A |
| 86 | Vocode Amount | 0 to 1 |
| 87 | Drive | 0 to 1 |
| 88 | Fold | 0 to 1 |
| 89 | Crush | 0 to 1 |
| 90 | Echo | 0 to 1 |
| 102 | Voice B Engine | 0-21 LIVE, 22-63 LOOP, 64-105 WAVE, 106-127 GRAIN |
| 103 | Voice B Pitch | -24 to 24 (st) |
| 104 | Voice B Size | 1 to 2700 (ms), in even steps of octaves |
| 105 | Voice B Texture | 0 to 1 |
| 106 | Voice B Direction | 0-31 FORWARD, 32-95 REVERSE, 96-127 BOTH |
| 107 | Voice B Focus | 0-31 ALL SOUND, 32-95 VOICES ONLY, 96-127 MUSIC ONLY |
| 108 | Voice B Level | 0 to 2 (lvl) |
| 109 | Voice B Mute | 0-63 OFF, 64-127 ON |
| 110 | Voice B Pan | -1 to 1 (pan) |
| 111 | Voice B Drive | 0 to 1 |
| 112 | Voice B Oscillator | 0-15 OFF, 16-47 SINE, 48-79 TRIANGLE, 80-111 SAW, 112-127 SQUARE |
| 113 | Voice B Osc Mix | 0 to 1 |
| 114 | Space | 0 to 1 |
| 115 | Stereo Width | 0 to 2 |
| 116 | Volume | -24 to 12 (dB) |
| 117 | Tempo (SELECT) | 20 to 300 (bpm) |
| 118 | Bass | -12 to 12 (dB) |
| 119 | Treble | -12 to 12 (dB) |
<!-- end cc table -->
