// Pure editing operations on one channel's piano-roll notes.
//
// A NoteSet is a working copy of a pattern channel's notes plus a selection
// bitmask. Every operation keeps the selection attached to the same notes
// (indices shift when notes are removed or merged). The view applies a result
// with Session::setNotes(); undo/redo is just keeping earlier NoteSets.
// Nothing here touches the engine, so it is fully host-testable.
//
// Positions and lengths are in ticks (cfg::kTicksPerStep = 24 per 16th step),
// so notes can sit between steps and be shorter than one step.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "project/project.hpp"

namespace noteedit {

constexpr int kTicks = cfg::kTicksPerStep;

struct Set {
    PianoNote notes[cfg::kMaxNotes];
    int count = 0;
    uint64_t sel = 0; // bit i = notes[i] selected
};

// Copy buffer. Positions are relative to the selection's first note and lowest pitch.
struct Clip {
    PianoNote notes[cfg::kMaxNotes];
    int count = 0;
    int spanTicks = 0; // distance from the first note's start to the last note's end
};

void load(Set& s, const PatternData& pattern, int channel);

// Sets a note's start / duration in ticks, keeping step, tick, length and lenTicks consistent.
void setStart(PianoNote& n, int ticks);
void setDuration(PianoNote& n, int ticks); // clamped to 1 tick .. a whole pattern

int indexAt(const Set& s, int startTicks, int pitch);       // note starting exactly there
// Index of the note covering the tick position at `pitch` (including its length), or -1.
int indexCovering(const Set& s, int ticks, int pitch);
int selectedCount(const Set& s);
bool isSelected(const Set& s, int index);
void toggleSelect(Set& s, int index);
void selectNone(Set& s);
void selectAll(Set& s);
void selectColumn(Set& s, int step);       // notes starting inside `step`
void selectRow(Set& s, int pitch);         // notes at `pitch`
void selectFrom(Set& s, int ticks);        // notes starting at or after the position

// Every operation acts on the selection, or on all notes when `all` is true.
// Moves/transposes are all-or-nothing: false (and no change) if a note would leave the grid.
bool move(Set& s, int dTicks, int dPitch, int patternTicks, bool all = false);
bool transpose(Set& s, int semitones, bool all = false);
// Rounds starts and lengths to multiples of `snapTicks` (1 length unit minimum).
void quantize(Set& s, int snapTicks, int patternTicks, bool all = false);
void setLength(Set& s, int steps, bool all = false);          // whole steps
void setLengthTicks(Set& s, int ticks, bool all = false);
void addLength(Set& s, int deltaTicks, bool all = false);
void setVelocity(Set& s, int velocity, bool all = false);
void addVelocity(Set& s, int delta, bool all = false);
int removeSelected(Set& s);

// Chop: splits every targeted note into `parts` equal consecutive notes (the last takes
// the rounding remainder). Notes too short to split (under 2 ticks per part) are left
// alone. All-or-nothing: false when the channel would exceed kMaxNotes. The pieces become
// the selection. This is how rolls, stutters and gated pads are made.
bool chop(Set& s, int parts, bool all = false);

// Copies the selection (all notes when nothing is selected). Returns the count.
int copy(const Set& s, Clip& out);
// Pastes `clip` with its first note at `ticks` and its lowest note on `pitch`. Notes that would
// fall outside the pattern or the 0..127 range are not pasted; the whole paste is refused
// (false) when the channel would exceed kMaxNotes. The pasted notes become the selection.
bool paste(Set& s, const Clip& clip, int ticks, int pitch, int patternTicks);
// Duplicates the selection directly after itself (offset = its length).
bool duplicate(Set& s, int patternTicks);

} // namespace noteedit
