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
solo, fill every 4/2/1 steps, clear channel, pattern length (8-64), clear
pattern, stop.

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

| Button | Action |
| --- | --- |
| ↑ ↓ | select sample |
| Cross | assign to target channel |
| Square | preview through the software mixer |
| Triangle | preview through an SPU2 hardware voice |
| L1 / R1 | change target channel |

## Project

| Button | Action |
| --- | --- |
| ↑ ↓ | select row |
| ← → | change value (tempo ±1 BPM, master ±5, latency ±512 frames) |
| L1 / R1 | fine change (tempo ±0.1 BPM, master ±1, latency ±128) |
| Cross | run the action on the row |

Rows: tempo, pattern, length, master volume, audio latency, file slot, save
to USB, load from USB, new demo project (press twice), test tone via PCM
stream, test tone via SPU2 voice, debug overlay, clear error.
