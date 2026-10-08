# Controls (DualShock 2)

Basic operation never requires the analog sticks; a digital pad works.
Held buttons auto-repeat after 320 ms, then every 70 ms (navigation and value
changes only; destructive actions such as clearing never repeat).

## Global

| Button | Action |
| --- | --- |
| START | play / pause |
| L3 (press left stick) | stop and rewind to step 1 |
| SELECT | next view: RACK → ROLL → INST → SONG → MIXER → BROWSER → PROJECT |
| R3 (press right stick) | toggle debug overlay |
| Left stick | same as D-pad (speed follows deflection) |
| Hold SELECT at boot | skip USB storage drivers |

Global keys are disabled while a menu is open. Menus with more than 13 entries scroll (the title bar shows the position).

## Channel Rack

Columns: `[LED] [mute] [name] [volume] [pan] [steps 1..16]`. A project starts with 8 channels and can hold 16 (the mixer's CHANNELS page scrolls to match).

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
| L1 / R1 | jump cursor one beat (4 steps); **on the name column: previous / next loaded sample for the channel (each is previewed)** |
| L2 / R2 | previous / next pattern (1-8) |
| Right stick ↑↓ / ←→ | selected channel volume / pan |

Channel menu: preview, piano roll, **add channel** (sampler with the first loaded sample, or a synth; up to 16 rows, the rack scrolls past 8) and **remove this channel** (asks; later rows move up with their steps and notes; playback stops), choose loaded sample, **browse USB samples** (opens the BROWSER aimed at this channel; Cross there assigns the file),  instrument settings (INST view),
mixer track (which insert the channel plays into; a small number at the right of
the channel name shows it), voice (software mixer / SPU2 hardware),
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
(the row marked ROOT, C4, plays the sample at its native speed; for a synth it
is middle C), columns are 16th-note steps (pages of 16, scrolling with the
cursor). Open it from the rack (Triangle > Piano roll) or SELECT to the ROLL tab.
Notes can be chords, any length and **shorter than a step**: every step has 24 ticks, the snap grid (menu) goes from 2 steps down to 1/8 of a step (3 ticks), the cursor and Grab move in snap units, L1 / R1 resize in snap units (whole steps at the default snap), and a note can start between steps. **Chop** (menu, or in SELECT mode) splits the selection - or the note under the cursor - into 2, 3, 4, 6 or 8 equal consecutive parts for rolls, stutters and gated pads. Up to 64 notes per channel and pattern; the
rack's step hits show on the ROOT row. Notes that are sounding glow, selected
notes have a cyan frame, the bar under a note shows its velocity.

**DRAW mode** (default; these bindings are unchanged from earlier versions):

| Button | Action |
| --- | --- |
| D-pad | move the cursor (horizontal steps follow the snap grid) |
| Cross | place a note (current length and velocity, and you hear it); on a note (any part of it): remove it; on a rack hit at ROOT: remove the hit |
| Square | hear the pitch under the cursor |
| L1 / R1 | on a note: shorten / lengthen it (this is the confirmed note-resize binding); on an empty cell: length of the next note |
| Right stick up / down | velocity of the note under the cursor, or of the next note |
| L2 / R2 (tap) | previous / next pattern (applied when the button is released) |
| R2 held + Up / Down | octave up / down |
| R2 held + Left / Right | previous / next page of 16 steps |
| L2 held + Cross | **undo** (16 levels, per pattern and channel) |
| L2 held + Circle | **redo** |
| Circle | next channel |
| Triangle | menu |

**SELECT mode** (menu: Mode > SELECT notes): Cross toggles the note under the cursor
into the selection, Square **grabs** the selection (D-pad moves it by the snap grid
horizontally and by semitones vertically, R2 + Up / Down by octaves, Cross drops,
Circle cancels and restores), L1 / R1 and the right stick change length / velocity
of the whole selection, Circle clears the selection (and leaves SELECT mode when
nothing is selected).

Menu: mode, undo, redo, copy (selection, or everything when nothing is selected),
cut, paste at the cursor (the clipboard keeps its shape; the lowest note lands on
the cursor pitch; refuses rather than overflow 64 notes), duplicate right after,
delete, select all / this step / this pitch / from here on, note length presets
(1/8 step up to 16 steps), velocity presets, snap grid (2 steps down to 1/8 step,
used by the cursor, Grab and quantize), **chop**, quantize, transpose by +-1 semitone / octave,
instrument mode and clear notes (undoable).

Instrument mode (also in the rack channel menu): **one-shot** samples always play
to the end and a new hit on the channel fades the previous one (drums);
**sustained** note lengths cut the sample and notes layer (basses, pads, leads).
Synth channels are always sustained. Rack cells that only contain piano-roll notes
are shown dim; clicking one in the rack removes those notes.

## Instrument (INST)

Settings for the selected rack channel (L2 / R2 changes channel). Every row is
`name / bar / value`.

| Button | Action |
| --- | --- |
| Up / Down | select row |
| Left / Right | adjust (the step is chosen per parameter) |
| L1 / R1 | adjust x10 |
| Cross | on INSTRUMENT / PRESET: switch / next preset; otherwise hear |
| Square | hear the instrument (pitch from the menu, C4 by default) |
| Circle | reset the row to its default |
| Triangle | menu: switch sampler / synth, synth presets, envelope presets, mixer track, audition octave, reset |

A **sampler** has ENVELOPE on/off and ATTACK / HOLD / DECAY / SUSTAIN / RELEASE
(milliseconds, sustain in percent) with a picture of the curve. With the envelope on,
each note owns an envelope: the release starts when the note ends (the piano-roll
note length, which needs the instrument set to *sustained*); one-shot channels hold
the sustain level until the sample ends. With the envelope off a sample behaves
exactly as before. A **synth** adds two oscillators (sine, square, saw, triangle,
noise) with coarse / fine tuning and detune, a sub oscillator, noise, FM mode
(oscillator 2 modulates oscillator 1), a low-pass / high-pass / band-pass filter
with resonance and envelope amount, an LFO (pitch, filter or amplitude), transpose,
level and nine presets. At most 8 voices per synth channel; tuning changes apply to
the next note, everything else changes sounding notes live. MIXER TRACK chooses the
insert the channel plays into.

## Song (Playlist)

Rows are tracks (16, eight shown at a time: the grid scrolls vertically with the cursor), columns are bars (up to 128, 16 shown, the view scrolls).
A clip puts a pattern on a track for a number of bars; the pattern loops while
the clip lasts. Clips on different tracks play together; clips on one track
never overlap (placing over one replaces it).

| Button | Action |
| --- | --- |
| D-pad | move the cursor (track / bar) |
| Cross | empty cell: place the current pattern (brush length); on a clip: remove it |
| Square | on a clip: **pick it up** and carry it (D-pad moves it, R2 held moves four bars at a time, L1 / R1 resize it, Cross drops it, Circle puts it back); the menu's *Use this clip as the brush* does what Square used to |
| L2 / R2 | previous / next pattern (the brush; same selection as the rack) |
| L1 / R1 | on a clip: shorten / lengthen it (stops at the next clip on the track); on an empty cell: brush length |
| Circle | toggle SONG mode (plays the playlist) / PATTERN mode (loops the current pattern) |
| Triangle | menu: mode, play song from the cursor bar, brush (pattern / audio sample), choose sample, edit audio clip, track mute / solo, track name, duplicate clip, move clip, clear track, clear whole playlist (asks) |

**Audio clips**: Triangle > Brush: AUDIO SAMPLE switches Cross to placing a bank sample
(built-in or imported; choose it with Triangle > Sample, or with L2 / R2 while the
audio brush is active). The clip's default length is the sample's duration at the
current tempo, rounded up to bars (1-32). Audio clips sit on the same six tracks as
pattern clips, never overlap them (placing a pattern clip over an audio clip replaces
it) and show a waveform of the trimmed region at the real time scale. Cross on an
audio clip removes it, Square picks its sample, L1 / R1 change its length.
Triangle > Edit audio clip opens the clip editor: VOLUME, LOOP, TRIM START / END
(10 ms steps, L1 / R1 x10), LENGTH and MIXER TRACK; Square hears the sample, Circle
closes. Duplicate and Move work on audio clips too (a move onto another clip is
refused). Audio plays at its native rate (no time-stretch) and is started or joined at
the right offset when you play from a bar in the middle of a clip.

**Ungroup a pattern clip**: Triangle on a pattern clip > *Ungroup: one track per instrument* gives every
instrument that has steps or notes in the pattern a clip of its own on the next free track (the first stays in the
original clip; it stops when the tracks run out). *Take one instrument out...* moves just one. Each split clip plays the
same pattern with a narrower instrument mask, so the song sounds exactly the same, but you can now move, trim, mute
(track M/S), delete or re-place each instrument separately. Narrow clips show the pattern number and the instrument's
name; clips with several instruments show `P1 +3`. *Regroup* sets a clip back to all instruments (delete the split
clips you no longer need). Duplicate and Move keep the mask. A clip with one instrument is the end of the line.

Move clip: the menu picks the clip up, the D-pad carries it, Cross drops it
(replacing pattern clips it lands on), Circle puts it back. Duplicate puts a copy right
after the clip and refuses rather than overwrite another clip. Muted tracks show
a red `M`, soloed tracks a yellow `S`.

In SONG mode START plays from bar 1 and loops at the end of the last clip; the
header shows `BAR b.beat` and a playhead runs across the grid (a red line marks
the song end). With an empty playlist the engine keeps looping the pattern.

## Mixer

Two pages (L2 switches) and an effect editor.

**CHANNELS page**: one strip per rack channel plus the master.

| Button | Action |
| --- | --- |
| Left / Right | select strip (8 channels + master) |
| Up / Down | volume +-1 (hold R2 for +-5) |
| L1 / R1 | pan +-5 |
| Square | mute |
| Cross | solo |
| Circle | preview |
| Triangle | choose the insert this channel plays into |
| Right stick | volume / pan |

**INSERTS page**: eight insert tracks plus the master. Each strip has stereo peak
meters (post-fader, -48 dB to 0 dB), a clip lamp above them that latches when the
track goes over full scale (Triangle > Reset clip indicators), fader, pan, mute /
solo, four squares for the effect chain (orange = active, dim = bypassed), the
number of channels routed to it and, for the selected strip, the compressor's gain
reduction. A soloed insert silences every other route, including channels routed
straight to the master.

| Button | Action |
| --- | --- |
| Left / Right | select track |
| Up / Down, L1 / R1 | volume, pan (as above) |
| Square / Cross | mute / solo |
| Circle | open the **effect editor** for the track |
| Triangle | menu: assign channels to this track (toggle several), name (cycles presets), clear effects, reset clip lamps |

**Effect editor**: four slots per track, processed in order from slot 1.
Left column (slots): Up / Down select, Cross opens the effect list (gain, filter,
distortion, delay, compressor, EQ, reverb or empty), Square bypasses (crossfaded, no
click), L1 / R1 move the slot up / down (the order is saved), Right edits the
parameters. Parameters: Up / Down select, Left / Right adjust (R2 for x5, L1 / R1
for x10), Square resets, Cross bypasses, Circle goes back. The master's chain runs
before the master fader. Delay and reverb draw from a shared memory pool (3 delays,
2 reverbs in the whole project); an effect that finds no memory passes audio through
and the mixer shows "FX MEMORY FULL".

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
| ← → | change value (tempo ±1 BPM, swing ±5 %, master ±5, latency ±512 frames) |
| L1 / R1 | fine change (tempo ±0.1 BPM, swing ±1 %, master ±1, latency ±128) |
| Cross | run the action on the row |

Rows: name (Cross edits: ↑ ↓ change letter, ← → move, Cross done), tempo,
swing (delays every odd 16th step by up to half a step; saved with the project),
metronome (a click on every beat, higher on the bar; never rendered into exports),
pattern (with its name), length, master volume, audio latency, file slot
(shows the slot's project name, `(empty)`, `DAMAGED` or `BAK`), missing
samples (Cross lists them), save to USB, load from USB (asks again when there
are unsaved changes), new demo project (press twice), **export WAV** (renders the
song, or the current pattern when the playlist is empty, to
`PS2DAW/EXPORT/<NAME>.WAV`; a progress box shows the rendered time, Circle cancels
and leaves nothing behind), **recover autosave** (loads `AUTOSAVE.ps2daw`; asks
first when there are unsaved changes), test tone via PCM
stream, test tone via SPU2 voice, debug overlay, clear error.

Saving writes `SLOTn.TMP`, verifies it, rotates the old file to `SLOTn.BAK`,
then renames. A damaged `SLOTn.ps2daw` loads from the `.BAK` and says so.

## Export and autosave

The exporter parks the realtime audio thread, renders the song through the same
engine you hear (routing, effects, envelopes, synths, audio clips, note timing), and
writes 16-bit stereo 48 kHz PCM to `<NAME>.WAV.TMP`. It stops when the song ends plus
a ring-out tail: 250 ms at least, until 200 ms of silence, and up to 6 s when a
delay or reverb is in use. The header is then patched, read back and checked, and only
then is the file renamed over the final name (an older export of the same name is kept
as `.OLD` until the rename succeeds). A write error (drive removed, disk full) or
Cancel deletes the partial file and restores playback. SPU2 hardware voices cannot be
captured: SPU2 channels with a software copy are rendered in software, `.adp`
samples are skipped and counted.

A changed project is written to `AUTOSAVE.ps2daw` (same TMP / BAK rotation as the
slots) every 90 seconds, never while a sample is loading or an export runs. The
PROJECT screen offers it again after a crash or power loss.
