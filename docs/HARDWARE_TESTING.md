# Hardware testing

Real PS2 hardware is the reference. PCSX2 is not required.

## Build

```sh
ps2build build
```

Copy `build/bin/ps2daw.elf` to a FAT32 USB stick or memory card and launch it
with your usual ELF launcher.

## Milestone 1 test plan

1. **Boot screen** - a health table appears within a second or two. Note any
   line that is not `OK` (yellow `WARN` for "no USB drive" is expected when
   no stick is inserted).
2. **Rack appears** - Channel Rack with 8 channels and a demo beat.
3. **Navigation** - D-pad moves the cyan cursor; held D-pad repeats.
4. **Audio, stream path** - PROJECT view → "Test tone SW" → Cross: a short
   1 kHz beep through the TV.
5. **Audio, SPU2 path** - PROJECT → "Test tone SPU2" → Cross: the same beep
   from an SPU2 hardware voice.
6. **Transport** - START plays the demo beat; START again pauses; L3 stops.
   Check the tempo sounds steady (120 BPM = 2 beats per second) and the pale
   playhead bar moves in time with the sound.
7. **Editing** - toggle steps with Cross while playing; changes are heard on
   the next pass. Square mutes a channel (LED stops, sound stops).
8. **Tempo** - PROJECT → Tempo ← → while playing: tempo changes without a
   jump or glitch.
9. **SPU2 channel** - Rack → Triangle → "Voice: ..." on a channel, toggle to
   SPU2; it should keep playing in time (purple LED).
10. **Mixer** - SELECT to MIXER: meters move with the beat; volume/pan work.
11. **Debug overlay** - R3. Leave it playing for 2+ minutes and note
    underruns, render avg/max, queue, FPS, "late max".
12. **USB (optional)** - with a FAT32 stick inserted at boot: PROJECT shows
    `mass0: ready`; Save to USB, change something, Load from USB restores it.
13. **Controller hot-plug (optional)** - unplug and replug the pad.

## What to report

* Console model (e.g. SCPH-30001, 70012, 90001), region, how you launched it
  (launcher + device).
* A photo of the boot health table, especially any `FAIL` lines.
* Debug overlay photo after ~2 minutes of playback (underruns, render times,
  queue, late max, audio err).
* Audio behaviour: silence, clicks, stutter, wrong speed/pitch, one path
  working but not the other (stream vs SPU2 test tone).
* Exact build errors from `ps2build build`, if any.
* For a hang: the last message shown on the boot screen.

## Host-side checks (no PS2 needed)

```sh
tests/host/run.sh           # engine/timing/file-format unit tests
tests/host/run.sh preview   # renders the real views to PNG for layout review
```
