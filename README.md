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

**Milestone 1 (channel rack, real audio): implemented, awaiting first
real-hardware test.** The ELF builds cleanly with PS2Build v2026.10.02
(toolchain GCC 15.3) and the platform-independent core passes its host test
suite, but it has **not yet been run on a PS2**. Treat everything below as
"should work" until the hardware report comes back.

| Area | State |
| --- | --- |
| PS2Build project, embedded IRX drivers, IOP reset | done |
| GS UI (gsKit), original font, overscan-safe 640x448 layout, NTSC/PAL | done |
| DualShock 2: edge detection, key repeat, analog, hot-plug | done |
| Modern `audio` library: libsd.irx + audio.irx, 48 kHz stereo PCM stream | done |
| Realtime render thread, sample-clocked transport, no UI-rate timing | done |
| Channel Rack: 8 channels, 16-64 steps, 8 patterns, mute/solo/vol/pan | done |
| Built-in synthesized kit (kick, snare, hats, clap, tom, rim, bass, test tone) | done |
| SPU2 hardware voices (PS-ADPCM encoded at boot), per-channel choice | done |
| Mixer view with real peak meters (software bus) | basic |
| Sample browser (built-in bank; USB import next) | basic |
| Project save/load `.ps2daw` to USB (FAT via BDM) | done, experimental |
| Debug overlay, boot health screen, sticky errors | done |
| WAV / ADP import from USB | parser done, UI in Milestone 2 |
| Playlist / arrangement | Milestone 4 |
| Effects / DSP | Milestone 6 |
| Piano roll | Milestone 7 |

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
| SELECT | next view (Rack, Mixer, Browser, Project) |
| R3 | debug overlay |

Full per-view bindings: [docs/CONTROLS.md](docs/CONTROLS.md).

## Samples and storage

* Built in: nine sounds synthesised at boot (no files needed).
* Supported import formats (parser present, browser UI in Milestone 2):
  PCM WAV 8/16-bit, mono/stereo, 4-96 kHz; `.adp` (APCM) for SPU2.
* USB layout (FAT32 stick, created on first save):

  ```
  mass0:/PS2DAW/SLOT1.ps2daw ... SLOT8.ps2daw    projects
  mass0:/PS2DAW/SAMPLES/                         (Milestone 2)
  ```

  The first ready device among `mass0:`, `mass1:` is used; nothing depends on
  a fixed absolute path or on where the ELF was launched from.

## Hardware status

Not yet tested on hardware. See [docs/HARDWARE_TESTING.md](docs/HARDWARE_TESTING.md)
for the test plan and what to report.

## Known limitations

* Untested on real hardware (first test pending).
* Pattern changes take effect immediately, not at the end of the bar.
* One choke group per channel: retriggering a channel fades its previous note.
* SPU2-voiced channels are scheduled to within a few milliseconds of the
  stream (the software mixer is sample-accurate); they also bypass the
  software meters, which say "HW" instead of showing a fake level.
* Samples cannot be unloaded yet; the bank holds up to 32.
* Project slots only (`SLOT1..8`); no on-screen keyboard for names yet.

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
* `src/ui`: widgets and views.

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
file format: [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md).

## Roadmap

1. **Channel Rack + real audio** (this milestone)
2. WAV/ADP loading from USB, sample browser
3. Multiple patterns UX, project save/load polish
4. Playlist / arrangement
5. Mixer: routing, inserts, better meters
6. Lightweight DSP (gain, filters, delay, distortion, compressor)
7. Piano roll + pitched sample instruments
8. Offline render to WAV on USB
9. Optimisation, autosave/recovery, larger projects

Order may change based on hardware findings.

## Development checks

```sh
tests/host/run.sh           # core unit tests on a desktop (g++, ASan/UBSan)
tests/host/run.sh preview   # also render UI layout previews
```

## License

No license has been chosen yet; all rights reserved by the repository owner
until one is added.
