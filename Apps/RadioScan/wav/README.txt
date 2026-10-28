Put .wav files in this folder and they become stations in RadioScan.

Each file is added to the station list (RADIO page) the next time RadioScan starts, named after the file.
Send it to voice A or B like any station: it plays looped, through the same engines, rhythms and effects
as a radio stream. Radio scan can land on it too.

Supported: PCM 8, 16, 24 or 32-bit, or 32/64-bit float; mono, stereo or more channels (the first two are
used); any sample rate. Compressed WAVs (ADPCM and the like) are not supported.

You can also list a file anywhere on the card in stations.txt:
  My Loop|file://wav/my loop.wav|sound          (relative to the RadioScan folder)
  Drone|file:///mnt/SDCARD/Music/drone.wav|sound (absolute path)
