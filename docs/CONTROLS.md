# Controls (DualShock 2)

Basic operation never requires the analog sticks; a digital pad works.
Held buttons auto-repeat after 320 ms, then every 70 ms (navigation and value
changes only; destructive actions such as clearing never repeat).

## Global

| Button | Action |
| --- | --- |
| START | play / pause |
| L3 (press left stick) | stop and rewind to step 1 |
| SELECT | next view: RACK → MIXER → BROWSER → PROJECT |
| R3 (press right stick) | toggle debug overlay |
| Left stick | same as D-pad (speed follows deflection) |
| Hold SELECT at boot | skip USB storage drivers |

Global keys are disabled while a menu is open.

## Channel Rack

Columns: `[LED] [mute] [name] [volume] [pan] [steps 1..16]`.

| Button | Action |
| --- | --- |
| D-pad | move between channels and columns; scrolls to steps 17-64 on longer patterns |
| Cross on a step | toggle the step |
| Cross on mute | toggle mute |
| Cross on name | preview the channel |
| Cross on volume/pan | enter edit mode (value turns yellow) |
| ← → / ↑ ↓ (edit mode) | adjust by 1 (pan by 2) |
| L1 / R1 (edit mode) | adjust by 10 |
| Cross or Circle (edit mode) | leave edit mode |
| Square | mute / unmute selected channel |
| Circle | preview selected channel |
| Triangle | channel menu |
| L1 / R1 | jump cursor one beat (4 steps) |
| L2 / R2 | previous / next pattern (1-8) |
| Right stick ↑↓ / ←→ | selected channel volume / pan |

Channel menu: preview, choose sample, voice (software mixer / SPU2 hardware),
solo, fill every 4/2/1 steps, clear channel, pattern length (8-64), pattern
tools, stop. Pattern tools: duplicate to the next empty pattern, copy to a
chosen pattern (asks before overwriting), clear (asks), name (cycles presets),
switch mode while playing (immediate / next beat / next bar). A pending switch
shows in the header as `P1>2`; the editing view moves to the new pattern at once.

Indicators: LED flashes when the channel is heard (green = software, purple =
SPU2); mute box green = playing, gold = solo, dark with red border = muted;
purple bar on the name = SPU2 voice; pale bar under a step = playhead.

## Mixer

| Button | Action |
| --- | --- |
| ← → | select strip (8 channels + master) |
| ↑ ↓ | volume ±1 (hold R2 for ±5) |
| L1 / R1 | pan ±5 |
| Square | mute |
| Cross | solo |
| Circle | preview |
| Right stick | volume / pan |

## Browser

Two sources: **LOADED** (everything in memory: built-in kit and imports) and
**USB** (`PS2DAW/SAMPLES` and sub-folders on the first drive that has it).

| Button | Action |
| --- | --- |
| ↑ ↓ | select entry |
| ← → | page up / down |
| L2 | switch source (LOADED / USB) |
| Cross | folder: enter. WAV/ADP: load if needed, then assign to the target channel |
| Circle | parent folder (USB) |
| Square | preview through the software mixer (loads the file first if needed) |
| Triangle | preview on an SPU2 hardware voice (uploads mono samples first) |
| L1 / R1 | change target channel |
| R2 | action menu: load, assign, preview/upload/remove SPU2, unload, rescan, create folder |

The bottom lines show loader progress or the last result, imported PCM
(used / 12288 KB), SPU2 KB, slots used and missing samples. Rows show type
(DIR/WAV/ADP/---), name and size; `RAM` marks files already loaded. Entries
starting with `.` are hidden; only the first 96 entries of a folder are listed.

## Project

| Button | Action |
| --- | --- |
| ↑ ↓ | select row |
| ← → | change value (tempo ±1 BPM, master ±5, latency ±512 frames) |
| L1 / R1 | fine change (tempo ±0.1 BPM, master ±1, latency ±128) |
| Cross | run the action on the row |

Rows: name (Cross edits: ↑ ↓ change letter, ← → move, Cross done), tempo,
pattern (with its name), length, master volume, audio latency, file slot
(shows the slot's project name, `(empty)`, `DAMAGED` or `BAK`), missing
samples (Cross lists them), save to USB, load from USB (asks again when there
are unsaved changes), new demo project (press twice), test tone via PCM
stream, test tone via SPU2 voice, debug overlay, clear error.

Saving writes `SLOTn.TMP`, verifies it, rotates the old file to `SLOTn.BAK`,
then renames. A damaged `SLOTn.ps2daw` loads from the `.BAK` and says so.
