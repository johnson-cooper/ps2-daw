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
| Engine: queue, grid, mixer buses and voices | ~120 KiB (in the ~210 KiB `App` object) |
| Effect memory pool (heap, once) | ~660 KiB |
| Project (`Project` incl. mixer, instruments, clips) | ~25 KiB x 2 (document + load buffer) |
| File buffers (save, verify) + export chunk | 128 KiB + 64 KiB |
| Piano-roll undo / redo | ~9 KiB |
| Imported samples | up to 12 MiB (budgeted) |
| Waveform summaries | 4 KiB |
| GS VRAM: 2 framebuffers + font | ~2.3 MiB of 4 MiB |
| SPU2 RAM: kit ADPCM | ~80 KiB of 2 MiB |

## Sample lifecycle (Milestone 2)

Imported samples live in `SampleBank` slots with a state machine
`Empty -> Ready -> Releasing -> Acked -> Empty` and a generation counter that
changes when a slot is reused.

1. UI `requestRelease(slot)`: `Ready -> Releasing`. `get()` returns null from
   then on, so the audio thread cannot start a new voice on it.
2. UI posts `ReleaseSample(slot)`. The audio thread applies it between blocks:
   it stops every voice reading the sample, forgets it in its channel and
   SPU2-ready tables, and only then marks the slot `Acked`.
3. UI `reap()` frees the PCM of `Acked` slots.

The audio thread never allocates, frees, locks or does I/O, and the UI never
frees memory the audio thread can still read. If the command queue is full the
release is retried from `Session::pumpReleases()`; with no audio thread running
the UI acknowledges itself. Imported PCM is capped at 12 MiB in total and 3 MiB
per sample; `SampleLibrary` frees unused imports and retries once when a load
would exceed the cap or the 32 slots.

`SampleLibrary` (UI thread) owns loading: a request queue and one job at a
time, advanced once per frame by one 32 KiB file read. It parses and converts
only after the whole file is in memory, publishes the slot in one step, then
performs the requested action (assign, preview, SPU2 upload). `.adp` files
become SPU2-only slots (no PCM in EE RAM). References are portable `samples:`
paths (see PROJECT_FORMAT.md) resolved against `mass0:` and `mass1:`.

## Audio output audit (Milestone 1 follow-up)

`audio_stream_queue_pcm()` never waits and returns fewer bytes than offered when
the stream is full. The render loop used to retry for up to 50 ms inside the
render thread and, on a partial accept, drop the tail while still advancing
`written_`, so `heard = written - queued` (the playhead and the SPU2 trigger
clock) could drift from what the stream really held. It now keeps the
unaccepted tail, resubmits it before rendering anything new, and advances
`written_` only by frames the stream accepted, so `heard` stays exact and no
audio is lost. `written_` and `heard` use unsigned wraparound arithmetic and
signed differences for comparisons. Triggers that overflow the pending list are
counted (`hwDropped`); notes for a sample unloaded in the meantime are counted
separately (`hwStale`). Underruns are counted once per empty-queue event.

## Pattern switching

`QueuePattern(pattern, mode)` makes the engine switch at the next beat (every
4 steps) or bar (every 16 steps), or at the loop point. A switch is taken only
when the target pattern has a step at that position, otherwise it waits for the
loop. Stop applies a pending switch at once. `Session::selectPattern` moves the
editing pattern immediately and queues the engine switch per
`Session::switchMode()`.

## Playlist and song mode (Milestone 4)

The UI owns the clips (`Project::clips`); `Session` mirrors them to the engine
with `SetClip`/`SetClipCount`/`SetSongMode`, so the audio thread keeps its own
copy (64 small structs) and never reads the project.

In song mode the transport wraps at the end of the last clip instead of at the
pattern length, so the same sample-accurate clock drives the timeline. At each
step the engine finds every clip covering the current bar, plays that pattern at
`(songStep - clipStart) % patternLength` and takes, per channel, the loudest
velocity among the active clips, so two clips hitting one channel never double
trigger. `StepMark` carries the absolute `songStep` for the playhead and
`EngineStatus::songMode`/`songBars` for the UI. Editing while playing is safe: if
the song shrinks below the playhead the transport wraps on the next step.
Leaving song mode re-selects the edited pattern.

## Piano roll, pitch and note length

Each pattern keeps, per channel, up to 32 `Note {step, pitch, velocity, length}`
beside the step grid; `Session` mirrors a channel's list to the engine
(`SetNote`/`SetNoteCount`) on every edit. At each step the engine plays the grid
hit at the root pitch plus every note starting at that step.

* **Pitch**: transposition resamples through the mixer's existing 16.16 playback
  increment, scaled by `2^(semitones/12)` from a shared table
  (`audio/pitch.cpp`, +-5 octaves, capped at 16 source frames per output frame),
  so pitched notes cost the same as native ones. SPU2 voices use
  `audio_channel_set_pitch()` with the same ratio.
* **Length / instrument mode**: a "sustained" channel gives each note a gate in
  output frames (`steps * 72,000,000 / bpmCenti`); `Mixer::mixSegment` splits the
  segment at the gate and starts the declick release there. One-shot channels
  ignore lengths and choke the previous voice like a drum pad; several notes
  landing on one step never choke each other.
* **Playlist extras**: `PlayFromBar` restarts the transport at an arbitrary
  step (`Transport::startAtStep`), and a track mute/solo mask is applied when the
  engine collects the clips covering a bar.

## Mixer routing and inserts (Milestone 5)

```
voices -> channel strip (volume, pan, mute/solo) -> route -> bus 1..8 (insert) -> master bus
                                                  \-> route 0 ----------------------^
audio-clip voices ---------------------------------> their own route ---------------^
```

* `ChannelData::route` (0 = master, 1-8 = insert) and `AudioClipData::route` pick the
  destination. The mixer keeps nine accumulator buses (`acc_[9][2][512]`, 36 KiB).
  A voice reads its route every segment, so a route change also redirects notes that
  are already ringing; any number of channels may share an insert.
* Each insert has a fader, pan, mute and solo (`MixTrackStrip`). The fader runs after
  the effects and is ramped across the block (no zipper noise on moves or mutes). A
  soloed insert silences every other route including the master-routed channels.
  Channel mute/solo (rack) and insert mute/solo are independent and both apply.
* Metering is post-fader peak per bus (instant attack, ~0.56 dB per block release) plus
  a latched clip flag that is set when an insert's peak exceeds full scale before the
  master saturates; the master's clip counter is unchanged. `EngineStatus::bus*` carries
  them to the UI.
* A bus that received no voice this block and has no live effect tail is skipped
  entirely (`busDirty_`), including its fader loop.
* SPU2 hardware voices cannot go through the EE buses. `triggerSample()` folds the
  insert's fader and pan into the hardware level and drops the trigger when the insert is
  muted or another insert is soloed; insert effects do not apply to them.

## Effects (Milestone 6)

`FxUnit` (`src/audio/fx.*`) is a small stereo processor working on float buffers in
the bus's int16 scale; each track has four slots and a bypass per slot (a bypass change
crossfades over one block). Implemented: gain (dB, pan, stereo width, invert), filter
(TPT state-variable low/high/band-pass, cutoff + resonance), distortion (cubic soft
clip, hard clip, fold-back, tone filter, mix, level), delay (up to 500 ms, feedback with
tone filter, ping-pong, slewed time changes, whole-frame fast path), compressor (peak
detector, gain computed every 32 frames with log2/exp2 approximations, makeup, gain
reduction meter), three-band EQ (RBJ shelves + peaking, bands at 0 dB cost nothing) and a
Freeverb-style reverb (8 combs, 4 all-passes, size / damping / width / mix).

* No allocation while rendering: delay and reverb memory is a pool in `FxMemory`
  (3 delays x 2 x 24000 floats, 2 reverbs x 13184 floats, ~660 KiB) allocated once in the
  `Mixer` constructor. `SetFxType` claims an entry (zeroing it, one-off cost) and
  releases the old one; an effect that finds the pool empty is `starved()`: it passes
  audio through and the UI says "FX MEMORY FULL".
* Stability is tested (`testFxStability`): every effect at minimum, maximum, default and
  random parameters, with noise, silence and full-scale DC, bypass toggled, must stay
  finite and bounded. States are flushed against denormals at block ends.
* An insert whose input is silent stops processing after its effects' tail time
  (4096 frames, or 10 s with a delay/reverb), so idle effects cost nothing.
* Everything is stored as `FxData` (type, bypass, six s16 parameters) in the project;
  `ParamDesc` tables describe range, step, display format and names for the generic
  editor, the file validator and the command handler alike.

## Instruments: envelopes and the native synthesizer

`ChannelData::inst` selects a sampler or a synth and holds the AHDSR envelope and the
synth patch (`src/audio/instrument.*`).

* **AHDSR**: per voice, Q24 level. Attack is linear, hold counts frames, decay and
  release are exponential (-60 dB in the set time), sustain follows live edits. The
  envelope advances every 16 frames and the gain is interpolated in between, so the
  per-sample loops contain no envelope logic. A note end (piano-roll note length)
  starts the release; a voice that is chokes or stopped uses a short declick instead.
  Voices are independent, so a chord or overlapping notes each run their own envelope.
  With the envelope disabled the original fast one-shot path is untouched.
* **Synth voice** (`Mixer::mixSynth`): two phase-accumulator oscillators (naive, not
  band-limited, sine from a 1024-entry table), sub, noise, FM, a TPT filter whose
  coefficient comes from a 1024-entry table (updated at control rate when the envelope or
  LFO modulates it), an LFO at control rate, the amp envelope. Per-channel voice cap 8
  (`kMaxSynthVoicesPerChannel`), 24 voices overall; when full the oldest voice of that
  channel (or the oldest releasing voice) is stolen. Notes need a length: step-grid hits
  on a synth sound for two steps.

## Ungrouped pattern clips

`PlaylistClip::chanMask` (default all channels) restricts a clip to some of its pattern's instruments. The
engine applies it in `fireSongStep` (steps) and `fireNotes` (notes); two clips on different tracks that play the
same pattern with disjoint masks reproduce the original clip exactly (tested bit for bit). `Session::ungroupClip`
and `splitClipChannel` only move channel bits into new clips on free tracks, so no pattern data is copied and the
8-pattern limit does not matter; track mute / solo, move and delete then work per instrument. The playlist has eight
tracks.

## Audio clips (playlist)

`AudioClipData` (track, source index, start bar, length, volume, loop, route, trim) is
mirrored into the engine with four small commands. At a clip's start step (or on the first
step of a play-from-bar that begins inside it) the engine starts one mixer voice
(`Mixer::triggerClip`): start frame = trim start plus the elapsed time converted to
source frames, loop region = trimmed region, gate = remaining clip length, so the clip
ends exactly with its last bar. The voice goes straight to the clip's route. Samples are
referenced by their stable sample reference (`Project::audioRefs`) and resolved to bank
slots at load, like channel samples; missing files show up under PROJECT > Missing
samples and the clip stays silent until the file returns. A sample used by a clip cannot
be released from memory.

Waveform previews (`ui/waveform.*`) keep 128 peak bytes per bank sample, built at most
32768 frames per UI frame, so a 3 MiB file never stalls the UI.

**Streaming (investigated, not implemented)**: the bank loads whole files into RAM (3 MiB per
file, 12 MiB total, 8 s of 48 kHz stereo at the cap). Streaming longer files would need a
ring buffer per clip filled from USB by a loader thread. USB 1.1 mass storage delivers
roughly 0.5-1 MB/s in practice against 192 KB/s per stereo 48 kHz clip, FAT reads are
issued through fileXio RPC with latencies of tens of milliseconds, and a late read would
have to be an audible gap or a stall; the audio thread must never wait. The safe design is
a UI-thread loader that keeps a 128 KiB ring per clip ahead of the playhead and pauses
the clip (silence, flagged on the clip) on underrun. It is not built because it needs
real-hardware USB measurements to size the rings.

## Offline WAV export (Milestone 8)

`Exporter` (`audio/wav_export.*`) is a state machine driven from the UI loop: it parks the
render thread through `RenderHold` (`Ps2Audio::hold`: the thread acknowledges at the top of
its loop and sleeps), puts the engine in export mode (`SetExportMode`: software voices only,
pattern/song plays once and pauses at its end, metronome off), then calls
`AudioEngine::render` itself in 512-frame blocks (about 10 ms of CPU per UI frame), writes
them through `ExportFile` into `<name>.TMP` in 64 KiB chunks, finishes with the header patch,
read-back verification and an atomic rename. Failures at any step (no drive, write error,
header mismatch, rename failure, cancel) delete the temp file, restore transport / song mode /
pattern and release the audio thread. `RamExportFile` is the in-memory sink used by the host
tests and by a debug command that exercises the whole path under PCSX2 (which has no USB).

## Piano roll editing

`project/note_edit.*` holds the pure operations (selection bitmask over the channel's notes,
move, transpose, quantize, length / velocity, copy, paste, duplicate, delete) on a `Set`
working copy; `Session::setNotes` applies a whole list at once, validated and mirrored to the
engine. The view takes an undo snapshot (at most 16, per pattern and channel, coalescing
nothing) before each edit and writes the result back, so undo / redo are also plain
`setNotes` calls. The limit is 64 notes per pattern and channel (project 8 KiB, engine copy
8 KiB).

## Sub-step notes and chop

A note's start is `step * 24 + tick` and its duration is `lenTicks` (or `length` whole steps), 24 ticks per
16th step. `fireNotes` starts a note with tick 0 at the step boundary as before; a note with a tick offset
goes into a fixed 64-entry pending list (`AudioEngine::queueNote`) with an absolute due frame
(`3,000,000 / bpmCenti` frames per tick), and the render loop ends its segment at the next due frame, so the
voice starts sample-accurately and its gate length is also in ticks. Stop and Pause clear the list. Chop is
`noteedit::chop`: a pure, all-or-nothing split of the targeted notes into equal consecutive notes. Timing
travels in `SetNoteFine` commands and the `NOTX` file chunk, so whole-step notes and older files are unchanged.

## Swing and metronome

`Transport::setSwingTicks` delays every odd 16th step boundary by 0-12 of its 24 ticks; the
tick positions are exact integers so loops and tempo changes stay drift-free. The metronome
is two 50 ms click samples generated at start-up and triggered on every beat (accent on the
bar) when enabled; it is not part of the project and is never exported.

## Performance (Milestone 9)

Per-phase timers in `AudioEngine::render` (optional clock, microseconds per 512-frame
block) feed the debug mailbox, and `DbgFxBench` times each effect on the EE. PCSX2 numbers
(an interpreter-class emulator, so only relative costs are reliable): reverb 1231 -> 650 us per
block per instance after hoisting its offsets, compressor 244 -> 117 us with log2/exp2
approximations and 32-frame control chunks, delay 242 -> 141 us with an integer fast path,
distortion 286 -> 214 us with a division-free soft clip, synth voice about 550 -> 245 us after
moving the envelope, LFO and filter coefficients to control rate and inlining the oscillators.
The deliberately heavy `DbgStressProject` song (4 synths with 6-note pads and arpeggios, 4
sampler voices with envelopes, six inserts, eleven effects) averages about 6.7 ms of the
10.7 ms block period with a 7.3 ms maximum and no underruns. A real EE has a 16 KiB data
cache, so the delay and reverb buffers (192 KiB and 52 KiB) may cost more there. Hardware
measurement is outstanding.

## Extension points

* Wavetable oscillators and automation clips (the `ParamDesc` command path is ready for both).
* Time signatures other than 4/4: `kBeatsPerBar` is a constant used by the playlist and the
  transport; a per-project value needs the playlist ruler and clip lengths to follow.
* Streaming for long audio clips (see above).
