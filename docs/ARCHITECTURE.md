# Architecture

PS2 DAW is split along the lines desktop DAWs such as Ardour use (session
model vs. realtime engine vs. UI), scaled down to what the PS2 can afford.
No code was taken from reference projects; only concepts.

## Source layout

| Directory | Contents | Depends on PS2 SDK? |
| --- | --- | --- |
| `src/core` | SPSC queue, bounded string helpers, `StatusLog` | no |
| `src/audio` | `Transport`, `Sequencer`, `Mixer`, `AudioEngine`, samples, drum synth, WAV parser, PS-ADPCM encoder | no |
| `src/project` | `Project` (song document), `.ps2daw` I/O, `Session` (edit API) | no |
| `src/platform` | IOP reset/modules, GS renderer, pad input, audio backend, USB storage | yes |
| `src/ui` | widgets, views, debug overlay, font | via `Gfx` only |
| `src/app.*`, `src/main.cpp` | boot sequence, main loop, chrome | yes |

Everything in `core`, `audio` and `project` compiles on a desktop and is
unit-tested by `tests/host/run.sh` (ASan + UBSan).

## Threads and priorities (EE)

| Priority | Thread | Work |
| --- | --- | --- |
| 63 | audio library event thread | created by `audio_init()` at caller-1; services stream events |
| 64 | render thread (`Ps2Audio::run`) | renders 512-frame blocks, queues PCM, dispatches SPU2 notes |
| 80 | main/UI thread | pad, storage polling, drawing, vsync |

The EE kernel schedules strictly by priority, so the render thread preempts
the UI whenever it becomes runnable. The UI thread never calls the audio
library; the render thread is the only caller after boot.

## Realtime rules (enforced by structure)

* `AudioEngine::render()` performs no allocation, no I/O, no locks, no
  printf. All buffers are members sized for `cfg::kMaxBlockFrames`.
* UI → audio: `Command` (8 bytes) through a 1024-entry lock-free SPSC ring.
  The engine applies at most 256 commands per block so a project load can
  never stall a block.
* Audio → UI: `EngineStatus` and `Ps2Audio::Stats`, plain aligned 32-bit
  fields with one writer. Step marks use a release/acquire serial.
* The audio thread owns its own copy of the pattern grid (`Sequencer`, 4 KiB)
  and strip settings; the UI's `Project` is never read by the audio thread.
* Samples are immutable once published in `SampleBank` (publish flag written
  last with release semantics). Slots are not freed while running.

## Clock and timing

The transport is clocked by rendered audio frames, never by UI frames.

```
acc += bpmCenti * PPQ          (per frame)
tick = acc / (6000 * 48000)    (exact integer, 96 PPQ, 16th = 24 ticks)
```

`render()` splits each block at every step boundary, so a software voice's
first frame lands exactly on the step's frame. Looping re-bases the
accumulator by the boundary while keeping the sub-tick remainder, so loops
do not drift. Tempo changes alter only the increment, so the position never
jumps. The host tests check that step *k* fires at exactly
`ceil(k * 24 * D / increment)` over 30 s at 120, 128, 133.33 and 174.37 BPM
with irregular block sizes.

## Output path 1: PCM stream (master bus)

* Format: 48 kHz, signed 16-bit, stereo: the SPU2's native output, so the
  library's converter is a straight copy.
* The render thread keeps `audio_stream_get_queued_bytes()` at a target
  depth (default 2048 frames, ~43 ms; adjustable 1024..8192 in PROJECT).
  Below target it renders and queues a block; above it sleeps for roughly
  the excess (500 us .. 8 ms).
* Underrun = queue observed empty after it was first filled (the SPU2 then
  plays silence). Counted and shown in the debug overlay.
* Mixing: Q15 gains, 32-bit accumulation, 32x32→64 master gain multiply,
  saturation to 16-bit with a clip counter. Channel strips use a squared
  volume law and a balance pan law; mute/solo/volume/pan apply live to
  ringing voices.

## Output path 2: SPU2 hardware voices

* At boot every mono bank sample is PS-ADPCM encoded on the EE
  (`adpcm::encodeMono`, ~56 dB SNR on test signals) and uploaded with
  `audio_sound_load_adpcm()`. Loop flags are left to the library.
* A channel in SPU2 mode produces `HwTrigger{frame, ...}` instead of a
  software voice. The render thread holds pending triggers and fires
  `audio_channel_play_sound()` when the *heard* frame reaches the trigger
  frame:

  ```
  heard = framesWritten - framesStillQueued
  ```

  so hardware notes line up with the stream to within one scheduler tick
  (worst lateness is reported as "late max" in the overlay). Rack channel
  *n* uses SPU2 voice *n*; previews use voice 23.
* Why both paths: the software mixer is sample-accurate, metered and is the
  base for future DSP; the SPU2 path costs almost no EE time and is the
  future home of pitched sample instruments (`audio_channel_set_pitch`).
  Defaults are software for all channels; switch per channel in the rack menu.

## UI

* gsKit, one-shot queue, one DMA transfer per frame, no Z buffer.
* 640x448 logical canvas (PAL centres it in 640x512), overscan-safe margins
  (24 px sides, 14 px top, ~16 px bottom).
* Font: original 5x7 glyphs in a 128x64 32-bit atlas (32 KiB VRAM), uploaded
  once, drawn at 2x so strokes are 2 px tall (no interlace flicker).
  Alpha blending only for text and the translucent overlay.
* Views: `ChannelRackView`, `MixerView`, `BrowserView`, `ProjectView`.
  They edit the song only through `Session`. `UiContext::heardStep()` and
  `channelActive()` use the heard frame, so the playhead and LEDs match what
  you hear rather than running ahead by the output latency.

## Boot sequence

1. IOP reset + `sbv_patch_enable_lmb()`.
2. GS (so later failures can be shown).
3. `sio2man.irx`, `padman.irx`, `padInit`, port 0.
4. `libsd.irx`, `audio.irx`, `audio_init()`, `audio_stream_start()`.
5. Synthesize the built-in kit; sync the demo song to the engine.
6. Encode + upload SPU2 copies.
7. 0.6 s pad window (hold SELECT to skip USB), then iomanX, fileXio, usbd,
   bdm, bdmfs_fatfs, usbmass_bd and `fileXioInit()` (newlib file calls are
   then routed through fileXio).
8. Start the render thread.
9. If any subsystem failed, the health table stays up until CROSS (20 s
   timeout); failures stay visible in PROJECT and the status bar.

If the GS cannot be initialised, the boot report is printed with the debug
text screen instead of leaving a black screen.

## Memory budget (approximate)

| Item | Size |
| --- | --- |
| ELF (text+data+bss) | ~630 KiB |
| gsKit queues | 1.25 MiB |
| Built-in kit PCM | ~280 KiB |
| Engine (queue, grid, mixer buffers) | ~20 KiB |
| GS VRAM: 2 framebuffers + font | ~2.3 MiB of 4 MiB |
| SPU2 RAM: kit ADPCM | ~80 KiB of 2 MiB |

## Planned extension points

* Deferred sample release: bank slots will carry a generation counter; the
  engine acknowledges via the status block before memory is freed.
* Mixer routing: `ChannelData::route` is already saved; inserts will be a
  fixed array of small DSP modules per strip.
* Playlist: `Project::clips` (pattern references, not copies) is already in
  the file format.
