# `.ps2daw` project format (version 1)

Binary, little-endian, chunked. No raw structs are written, so compiler
layout never reaches the file. Readers skip unknown chunks, so a newer file
stays loadable by an older build where possible. Maximum file size accepted:
64 KiB.

## Header (12 bytes)

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 8 bytes | `PS2DAWPJ` |
| 8 | u16 | format version (1) |
| 10 | u16 | minimum reader version able to load the file (1) |

## Chunks

Each chunk: 4-byte tag, u32 payload length, payload. `str` = u8 length +
bytes (printable ASCII; anything else is replaced with `?` on load).

| Tag | Payload |
| --- | --- |
| `PROJ` | str name, u32 tempo (BPM x 100), u8 master volume, u8 current pattern, u8 channel count |
| `CHAN` (x8) | u8 index, str name, str sample reference, u8 volume, s8 pan, u8 flags (bit0 mute, bit1 solo), u8 voice mode (0 software, 1 SPU2), u8 route (0 = master) |
| `PATT` (x8) | u8 index, str name, u8 length (steps), u8 channels, u8 steps, then channels x steps velocity bytes (0 = off, 1-127) |
| `PLST` | u16 clip count, then per clip: u8 track, u8 pattern, u16 start bar, u16 length bars (reserved for the playlist) |
| `END ` | u32 CRC-32 (IEEE) of every byte before this chunk |

Sample references: `builtin:<NAME>` for the generated kit; storage paths for
user samples (Milestone 2). Bank slots are resolved at load time and never
stored.

Validation on load: every length is bounds-checked against the real buffer,
values are clamped to legal ranges, a missing `PROJ` or `END ` chunk or a CRC
mismatch rejects the file with a readable reason and leaves the current song
untouched.
