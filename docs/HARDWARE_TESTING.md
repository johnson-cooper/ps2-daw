# Hardware testing

Real PS2 hardware is the final authority. The development loop is
PS2Build MCP (build) -> PCSX2 MCP (debugger-driven validation) -> you, on
hardware. PCSX2 results are never reported as hardware results; the README
keeps the columns separate. You do not need PCSX2 to test.

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

## Milestone 2 + 3 test plan (USB samples, patterns, safe saves)

Prepare a FAT32 stick: copy a few WAV files (mono and stereo, 44.1/48 kHz,
8 or 16 bit) into `PS2DAW/SAMPLES/` and a sub-folder such as `DRUMS/`. Put one
non-audio file and one WAV over 3 MiB there too. If you have a `.adp` file, add it.

1. **Browser, USB** - BROWSER > L2 (USB). Folders first, then files with type and
   size; the non-audio file is dimmed. If the folder is missing the screen says
   so; R2 > "Create PS2DAW/SAMPLES folder" creates it.
2. **Load + preview** - on a WAV: Square previews (the first press loads it; the
   bottom line shows progress). Watch the debug overlay (R3) while a large file
   loads **with the demo beat playing**: note any `underruns` increase. This is
   the key hardware unknown of this milestone.
3. **Assign** - Cross assigns to the target channel (L1/R1 changes it). The
   rack shows the new name; the beat plays it.
4. **Rejections** - the oversized WAV and the non-audio file give a readable,
   sticky error line, and nothing else breaks.
5. **SPU2** - R2 menu on a mono sample: "Upload to SPU2 RAM", then Triangle
   previews it on a hardware voice; the footer shows SPU2 KB used. A stereo
   sample is refused politely. A `.adp` loads straight to SPU2.
6. **Unload** - R2 > "Unload sample" frees RAM (the PCM total in the footer
   drops); it is refused while a channel uses the sample.
7. **Save/load round trip** - assign two imports, PROJECT > Name (edit), Save to
   USB, then Load. PROJECT > File slot shows `<n> NAME` for the slot.
8. **Missing sample** - rename one WAV on the stick, Load the slot: the song
   still loads and plays, and PROJECT > Missing samples lists the file. Rename
   it back, Load again: it binds.
9. **Port independence** - move the stick to the other USB port, Load: same result.
10. **Safe save** - Save twice (a `SLOTn.BAK` appears). Unplug the stick during a
    save if you dare; the previous save must still load. Corrupting
    `SLOTn.ps2daw` makes Load use the `.BAK` and say so.
11. **Patterns** - rack Triangle > Pattern tools: Duplicate, Copy to (asks before
    overwriting), Clear (asks), Name, "Switch while playing: next bar". With the
    beat playing, press R2 to change pattern: the header shows `P1>2` and the
    switch lands on the bar line.
12. **Load confirmation** - edit something, then Load: it asks for a second Cross.

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
* For Milestone 2: the `underruns` / `rpc err` / `tailRetry` counters from the
  overlay before and after loading a large WAV during playback, and the stick's
  brand, size and filesystem.

## Host-side checks (no PS2 needed)

```sh
tests/host/run.sh           # engine/timing/file-format unit tests
tests/host/run.sh preview   # renders the real views to PNG for layout review
```
