# `.ps2daw` project format (version 2)

Binary, little-endian, chunked. No raw structs are written, so compiler
layout never reaches the file. Readers skip unknown chunks, so a newer file
stays loadable by an older build where possible. Maximum file size accepted:
64 KiB.

## Header (12 bytes)

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 8 bytes | `PS2DAWPJ` |
| 8 | u16 | format version (2; version 1 files load unchanged) |
| 10 | u16 | minimum reader version able to load the file (1) |

## Chunks

Each chunk: 4-byte tag, u32 payload length, payload. `str` = u8 length +
bytes (printable ASCII; anything else is replaced with `?` on load).

| Tag | Payload |
| --- | --- |
| `PROJ` | str name, u32 tempo (BPM x 100), u8 master volume, u8 current pattern, u8 channel count, optional u8 swing (0-50 %, default 0) |
| `CHAN` (x8) | u8 index, str name, str sample reference, u8 volume, s8 pan, u8 flags (bit0 mute, bit1 solo), u8 voice mode (0 software, 1 SPU2), u8 route (0 = master), optional u8 gate (1 = sustained instrument) |
| `PATT` (x8) | u8 index, str name, u8 length (steps), u8 channels, u8 steps, then channels x steps velocity bytes (0 = off, 1-127) |
| `NOTE` (0..64) | u8 pattern, u8 channel, u8 count (max 64), then per note 4 bytes: u8 step, u8 pitch (MIDI 0-127, 60 = native speed), u8 velocity (1-127), u8 length (steps, 1-64). Optional chunk: files without it have no piano-roll notes |
| `NOTX` | Sub-step timing for the notes of the preceding `NOTE` chunk: u8 pattern, u8 channel, u8 count, then per timed note: u8 step, u8 pitch, u8 tick (0-23 ticks into the step), u16 duration in ticks (0 = the note's whole-step length). Written only when a channel has such notes; older readers skip it and play those notes on whole steps |
| `INST` (x8) | u8 channel, u8 kind (0 sampler, 1 synth), u8 envelope parameter count, then that many s16 (enabled, attack ms, hold ms, decay ms, sustain %, release ms), u8 synth parameter count, then that many s16 (see `SynthParam` in `src/audio/instrument.hpp`: osc1 wave / level / fine, osc2 wave / level / coarse / detune, sub, noise, mode, FM amount, filter type / cutoff / resonance / envelope amount, LFO rate / depth / destination, transpose, level, 4 reserved). Counts make the chunk extensible: unknown trailing values are skipped, missing ones keep their defaults. Written before `PLST` |
| `MIXR` (x9) | u8 track (0 = master chain, 1-8 inserts), str name, u8 volume, s8 pan, u8 flags (bit0 mute, bit1 solo), u8 slot count, then per slot: u8 effect type (0 none, 1 gain, 2 filter, 3 distortion, 4 delay, 5 compressor, 6 eq, 7 reverb), u8 bypass, u8 parameter count, that many s16 (see `src/audio/fx.cpp` for each effect's parameters). Slot order is processing order |
| `PMSK` | u16 count, then per ungrouped pattern clip: u8 track, u16 start bar, u16 channel bit mask (bit n = rack channel n plays). Written before `PLST` only when a clip does not play all instruments; clips are matched by track and start bar. Older readers skip it and play the whole pattern on every clip |
| `TNAM` | u8 count, then that many str: playlist track names |
| `ACLP` | u8 source count (8), then that many str: sample references used by audio clips (same syntax as channel sample references); u16 clip count (max 16), then per clip: u8 track, u8 source index, u16 start bar, u16 length bars, u8 volume (0-100), u8 loop, u8 mixer insert (0 = master), u32 trim start (frames), u32 trim end (frames, 0 = to the end of the sample) |
| `PLST` | u16 clip count, then per clip: u8 track, u8 pattern, u16 start bar, u16 length bars; then optional trailing bytes: u8 song mode (1 = play the playlist), u8 track mute bits, u8 track solo bits (older files end earlier and read as 0) |
| `END ` | u32 CRC-32 (IEEE) of every byte before this chunk |

Sample references (at most 63 characters):

* `builtin:<NAME>` - the generated kit.
* `samples:<REL>` - a file under `<root>/PS2DAW/SAMPLES/`, e.g.
  `samples:DRUMS/KICK.WAV`. `<REL>` uses `/` separators and printable ASCII,
  with no `..`, no empty, absolute or device-prefixed components, none of
  `: \ * ? " < > |`, and no component ending in a space or `.`. References never
  name `mass0:` or `mass1:`, so a project is portable between USB ports.

References that break these rules stay in the file but are never resolved and
are reported as missing. Bank slots are resolved at load time and never stored.
A missing file leaves the channel's reference intact (so saving again does not
lose it) with no sample until the file returns.

Slot files are written with a `.TMP` + `.BAK` rotation (see
`src/project/slot_store.hpp`).

Validation on load: every length is bounds-checked against the real buffer,
values are clamped to legal ranges, a missing `PROJ` or `END ` chunk or a CRC
mismatch rejects the file with a readable reason and leaves the current song
untouched.

Playlist validation on load: clips with a track >= 6, pattern >= 8, length 0 or
start bar >= 128 are dropped, lengths are clamped so a clip ends by bar 128,
a clip overlapping an earlier clip on the same track is dropped, and the
survivors are sorted by (start bar, track). At most 64 clips.

Mixer, instrument and audio-clip data are validated on load: unknown effect types become
empty slots, every parameter is clamped into its range, volumes and pans are clamped, a
route above 8 becomes the master, audio clips with a bad track, source, length or an
empty / overlapping position are dropped (overlap with pattern clips as well), and a trim
end at or before the trim start means "to the end". A file written by version 1 has none
of these chunks and loads with the defaults (sampler channels, no envelope, empty
mixer, no audio clips). Files from this version load in older builds that skip the
unknown chunks (the new data is ignored, a channel's `route` byte keeps its value).

Notes are validated on load: a note with step >= 64, pitch > 127 or velocity 0
is dropped, velocity and length are clamped, at most 64 notes are kept per
pattern and channel, and a chunk whose declared count does not fit is rejected
as corrupt. Unknown chunks are skipped, so files with notes still load in
builds that predate the piano roll (the notes are ignored).
