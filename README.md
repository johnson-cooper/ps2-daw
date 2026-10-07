# PS2 DAW

A pattern-based digital audio workstation that runs natively on the
PlayStation 2. The workflow is inspired by classic step-sequencer DAWs
(channel rack, patterns, playlist, mixer), but the program, its code and
its graphics are original and built around the PS2's hardware: the EE does
sample-accurate software mixing into the SPU2's PCM input, and short sounds
can also be played directly on SPU2 hardware voices.

> Not affiliated with, or a clone of, any commercial DAW. No third-party
> samples, fonts or artwork are included: the drum kit is synthesised at boot
> and the bitmap font is an original design (`tools/gen_font.py`).

## Status

Milestones 1-4 are implemented. Validation is tracked in three separate
columns, because they mean different things:

* **Builds**: `ps2build build` (PS2Build v2026.10.02, GCC 15.3) produces the ELF.
* **PCSX2**: run in a debugger-instrumented PCSX2 (PCSX2-MCP) and checked
  through the debugger (threads, IOP modules, memory, counters). This proves
  the code path executes; it does **not** prove hardware timing or SPU2 behaviour.
* **Hardware**: run on a real PS2. **Nothing has been confirmed on hardware yet.**

| Area | Builds | PCSX2 | Hardware |
| --- | --- | --- | --- |
| PS2Build project, embedded IRX drivers, IOP reset | yes | yes (modules listed by the debugger) | not yet |
| GS UI (gsKit), original font, 640x448 layout | yes | yes (60 FPS) | not yet |
| `audio` library, 48 kHz stereo PCM stream, render thread | yes | yes (see audit in ARCHITECTURE.md) | not yet |
| Channel Rack, patterns, mute/solo/vol/pan, transport | yes | yes | not yet |
| SPU2 hardware voices (built-in kit) | yes | code path runs | not yet |
| Sample browser: LOADED list, source switch, menu | yes | yes | not yet |
| Sample browser: USB folders, WAV/ADP listing | yes | **no** (PCSX2 has no USB mass storage) | not yet |
| WAV import: bounded, chunked load, budget, release | yes | yes, via the in-RAM self-test source | not yet |
| `.adp` import straight into SPU2 RAM | yes | **no** | not yet |
| Optional SPU2 upload of mono imports | yes | **no** | not yet |
| Queued pattern switching (beat / bar), copy, duplicate, names | yes | yes (switch observed) | not yet |
| Safe project saves (`.TMP` + `.BAK`), load recovery, slot info | yes | **no** (needs USB) | not yet |
| Playlist: 6 tracks x 128 bars, clips, song mode with looping playhead | yes | yes (clips placed, song played and looped) | not yet |
| Effects / DSP, piano roll, WAV export | later | | |

The platform-independent code (timing, formats, sample lifecycle, library,
pattern scheduling, slot store) also has host unit tests, which are not a
substitute for either column above.

## Screenshots

_Placeholder: real-hardware captures will go here after the first test._
Layout previews rendered on the host from the real view code are produced by
`tests/host/run.sh preview` (see [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md)).

## Building

Requires [PS2Build](https://ps2.techwritescode.dev/) (the PS2 SDK and build
tool). Nothing else: no Makefile, no CMake, no Docker image.

```sh
ps2build build
```

Output: **`build/bin/ps2daw.elf`** (stripped, all IOP drivers embedded).

Packages used (all from the PS2Build `core`/`world` tiers): `gskit`, `dmakit`,
`audio`, `pad`, `filexio`, `patches`, `debug`; embedded drivers `sio2man`,
`padman`, `libsd`, `audio`, `iomanx`, `filexio`, `usbd`, `bdm`, `bdmfs_fatfs`,
`usbmass_bd`.

## Running

Launch `ps2daw.elf` with any ELF launcher (wLaunchELF, OPL's app loader,
FMCB/PS2BBL, etc.) from a memory card or USB stick. The ELF resets the IOP
and loads its own drivers, so it does not depend on what the launcher had
loaded. First boot shows a health table for every subsystem; then the
Channel Rack opens with a demo beat. Press **START** to hear it.

Hold **SELECT** during boot to skip the USB storage drivers (recovery option).

## Controls (summary)

| Button | Channel Rack |
| --- | --- |
| D-pad / left stick | move cursor |
| Cross | toggle step / mute / preview / edit value (depends on column) |
| Square | mute selected channel |
| Circle | preview selected channel (or leave value edit) |
| Triangle | channel menu (sample, SPU2 voice, fills, length, solo) |
| L1 / R1 | jump one beat (4 steps) |
| L2 / R2 | previous / next pattern |
| Right stick | selected channel volume (up/down) and pan (left/right) |
| START | play / pause |
| L3 | stop and rewind |
| SELECT | next view (Rack, Song, Mixer, Browser, Project) |
| R3 | debug overlay |

Full per-view bindings: [docs/CONTROLS.md](docs/CONTROLS.md).

## Samples and storage

* Built in: nine sounds synthesised at boot (no files needed).
* Import from USB: PCM WAV (8/16/24/32-bit integer, 32/64-bit float), mono/stereo, 4-96 kHz, converted to 16-bit on load, and `.adp`
  (APCM, loaded straight into SPU2 RAM). Limits: 3 MiB per WAV file and per
  converted sample, 12 MiB of imported PCM in total, 32 sample slots, 1 MiB per
  `.adp`. Anything else is rejected with a visible reason.
* USB layout (FAT32 stick):

  ```
  mass0:/PS2DAW/SLOT1.ps2daw ... SLOT8.ps2daw    projects (+ .BAK of the previous save)
  mass0:/PS2DAW/SAMPLES/                         your WAV / ADP files, sub-folders allowed
  ```

  The sample folder is created from the browser (R2 menu), never behind your back.
* Projects store `samples:DRUMS/KICK.WAV`, a path relative to
  `PS2DAW/SAMPLES/`, never `mass0:` or `mass1:`. A project therefore works
  when the stick moves between ports; both ports are searched. Missing files
  are listed under PROJECT > Missing samples and the project stays usable.
* Samples load in the background (32 KiB per frame) and never touch the audio
  thread. Imported samples nothing uses are freed on project load and when
  memory runs out.

## Hardware status

Not yet tested on hardware. See [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md)
for the test plan and what to report.

## Known limitations

* Untested on real hardware (first test pending).
* Pattern switching while playing is immediate by default; "next beat" and
  "next bar" are chosen in the channel menu > Pattern tools.
* One choke group per channel: retriggering a channel fades its previous note.
* SPU2-voiced channels are scheduled to within a few milliseconds of the
  stream (the software mixer is sample-accurate); they also bypass the
  software meters, which say "HW" instead of showing a fake level.
* The bank holds up to 32 samples (9 are the built-in kit). Directory
  listings show the first 96 entries of a folder.
* USB access runs on the UI thread (listing a folder, reading a file in
  chunks). Whether a large read disturbs the audio stream on real USB hardware
  is unverified: report underruns seen while loading.
* Stereo imports play in software only; SPU2 upload needs mono.
* Project names are edited letter by letter (PROJECT > Name); patterns choose
  from a preset name list.

## Architecture

```
 UI thread (prio 80, vsync paced)          Render thread (prio 64)
 ┌───────────────────────────────┐          ┌──────────────────────────────┐
 │ Views ─► Session ─► Project    │ Command  │ AudioEngine                   │
 │            │  (UI-owned model) │ ───────► │  Transport (sample clock)     │
 │            └──── SPSC queue ───┼──────────┤  Sequencer (pattern mirror)   │
 │ reads EngineStatus/Stats only │ ◄─────── │  Mixer (int32 bus, saturate)  │
 └───────────────────────────────┘ status   └──────┬──────────────┬────────┘
                                                    │ PCM 48k s16  │ HW notes
                                             audio_stream_*   audio_channel_*
                                                    └──── audio.irx / libsd.irx (IOP) ──► SPU2
```

* `src/audio`: platform-independent engine (host-testable).
* `src/project`: song model, `.ps2daw` format, Session (edit API).
* `src/platform`: PS2 specifics (IOP, GS, pad, audio backend, storage).
* `src/ui`: widgets and views (Rack, Song/Playlist, Mixer, Browser, Project).

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
file format: [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md).

## Roadmap

1. **Channel Rack + real audio** (done, awaiting hardware report)
2. **WAV/ADP loading from USB, sample browser** (done, awaiting hardware report)
3. **Multiple patterns UX, project save/load polish** (done, awaiting hardware report)
4. **Playlist / arrangement** (done, awaiting hardware report)
5. Mixer: routing, inserts, better meters
6. Lightweight DSP (gain, filters, delay, distortion, compressor)
7. Piano roll + pitched sample instruments
8. Offline render to WAV on USB
9. Optimisation, autosave/recovery, larger projects

Order may change based on hardware findings.

## Development workflow

Development uses three layers; only the last one is authoritative:

```
PS2Build MCP (validate ps2.yaml, resolve packages, build the ELF)
    -> PCSX2 MCP (boot the ELF, inspect threads/modules/memory, drive input)
        -> real PS2 hardware (timing, SPU2, USB, performance, controller)
```

Host checks (desktop g++; ASan/UBSan are used where the toolchain provides
them. MinGW does not, and the script says so when it falls back):

```sh
tests/host/run.sh           # core unit tests
tests/host/run.sh preview   # also render UI layout previews
```

### Debug mailbox

`g_debugMailbox` (see `src/platform/debug_mailbox.hpp`) is a fixed block of RAM
for a debugger: write `inputMask` to inject a button press, write `command` to
start the in-RAM sample self-test or switch views, and read the 24 `telemetry`
words (frame counter, samples loaded/failed, PCM bytes, underruns, per-channel
slots, ...). Find its address with `nm` on
`build/obj/ps2daw/ps2daw.unstripped.elf`. It is how the PCSX2 checks were driven
without a controller. It has no user-visible behaviour.

## License

No license has been chosen yet; all rights reserved by the repository owner
until one is added.
