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

## Piano roll (ROLL)

Edits the selected rack channel in the current pattern. Rows are MIDI pitches
(the row marked ROOT, C4, plays the sample at its native speed), columns are
16th-note steps. Open it from the rack: Triangle > Piano roll, or SELECT to the
ROLL tab. Notes can be chords and any length; the rack's step hits show on the
ROOT row.

| Button | Action |
| --- | --- |
| D-pad | move the cursor (step / pitch) |
| Cross | place a note (current length and velocity, and you hear it); on a note: remove it; on a rack hit at ROOT: remove the hit |
| Square | hear the pitch under the cursor |
| L1 / R1 | on a note: shorten / lengthen it; on an empty cell: length of the next note |
| L2 / R2 | previous / next pattern |
| Circle | next channel |
| Triangle | menu: instrument mode, new-note velocity, shift all notes by octave or semitone, clear notes |

Instrument mode (also in the rack channel menu): **one-shot** samples always play
to the end and a new hit on the channel fades the previous one (drums);
**sustained** note lengths cut the sample and notes layer (basses, pads, leads).
Rack cells that only contain piano-roll notes are shown dim; clicking one in the
rack removes those notes.

## Song (Playlist)

Rows are tracks (6), columns are bars (up to 128, 16 shown, the view scrolls).
A clip puts a pattern on a track for a number of bars; the pattern loops while
the clip lasts. Clips on different tracks play together; clips on one track
never overlap (placing over one replaces it).

| Button | Action |
| --- | --- |
| D-pad | move the cursor (track / bar) |
| Cross | empty cell: place the current pattern (brush length); on a clip: remove it |
| Square | on a clip: pick up its pattern and length as the brush |
| L2 / R2 | previous / next pattern (the brush; same selection as the rack) |
| L1 / R1 | on a clip: shorten / lengthen it (stops at the next clip on the track); on an empty cell: brush length |
| Circle | toggle SONG mode (plays the playlist) / PATTERN mode (loops the current pattern) |
| Triangle | menu: mode, play song from the cursor bar, track mute / solo, duplicate clip, move clip, clear track, clear whole playlist (asks) |

Move clip: the menu picks the clip up, the D-pad carries it, Cross drops it
(replacing clips it lands on), Circle puts it back. Duplicate puts a copy right
after the clip and refuses rather than overwrite another clip. Muted tracks show
a red `M`, soloed tracks a yellow `S`.

In SONG mode START plays from bar 1 and loops at the end of the last clip; the
header shows `BAR b.beat` and a playhead runs across the grid (a red line marks
the song end). With an empty playlist the engine keeps looping the pattern.

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
