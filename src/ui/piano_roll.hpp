// Piano roll for the selected rack channel and the current pattern.
//
// Rows are pitches (MIDI note numbers; 60 plays the sample at its native
// speed), columns are 16th-note steps. Notes can be chords, any length, and
// are stored per pattern and channel next to the rack's step grid. For a
// "sustained" channel (menu: Instrument mode) a note's length cuts the sample
// so it behaves like a played instrument; otherwise samples play to the end.
//
// Time is finer than the step grid: every step has 24 ticks, the snap grid
// (menu) goes down to 1/8 of a step, notes can start between steps and be
// shorter than a step, and Chop splits a note into equal parts for rolls.
//
// Editing model (everything works with the DualShock 2 alone):
//  * DRAW mode (default): Cross places/removes, L1/R1 length, right stick
//    velocity. These bindings are unchanged from earlier versions.
//  * SELECT mode (menu): Cross toggles notes into a selection; Square grabs
//    the selection and the D-pad moves it; the menu copies, cuts, pastes,
//    duplicates, chops, transposes, quantizes and resizes the selection.
//  * Chords: L2+Cross undo, L2+Circle redo, R2+Up/Down octave, R2+Left/Right a
//    page of steps. L2 / R2 alone still switch pattern (on release).
#pragma once

#include "project/note_edit.hpp"
#include "ui/view.hpp"

class PianoRollView : public View {
public:
    static constexpr int kVisibleRows = 12;
    static constexpr int kVisibleSteps = 16;
    const char* tabName() const override { return "ROLL"; }
    const char* hint() const override;
    void onEnter(UiContext& ctx) override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

    static const char* noteName(int pitch, char* buf, size_t cap);
    // "1 step", "1/4 step", "2 steps", "3/24"... for a length or snap in ticks.
    static const char* tickText(int ticks, char* buf, size_t cap);

private:
    enum class Mode : uint8_t { Draw, Select, Grab };
    enum MenuId {
        MenuMode = 1,
        MenuUndo,
        MenuRedo,
        MenuCopy,
        MenuCut,
        MenuPaste,
        MenuDuplicate,
        MenuDelete,
        MenuSelectAll,
        MenuSelectNone,
        MenuSelectColumn,
        MenuSelectRow,
        MenuSelectFrom,
        MenuLength,
        MenuVelocity,
        MenuSnap,
        MenuQuantize,
        MenuTranspose,
        MenuChop,
        MenuGate,
        MenuClear,
        MenuClearConfirm,
        MenuLengthBase = 1000,   // + ticks
        MenuVelocityBase = 2000, // + velocity
        MenuTransposeBase = 3000, // + semitones + 24
        MenuChopBase = 4000,     // + parts
        MenuSnapBase = 5000      // + ticks
    };
    struct Snapshot {
        uint8_t pattern, channel, count;
        PianoNote notes[cfg::kMaxNotes];
    };
    static constexpr int kUndoDepth = 16;

    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);
    void clampView(const UiContext& ctx);
    void refresh(UiContext& ctx);               // reload the working set (and selection) from the project
    void beginEdit(UiContext& ctx);             // push an undo snapshot of the current notes
    void commit(UiContext& ctx);                // write the working set back to the project
    void undo(UiContext& ctx, bool redo);
    void moveCursorH(int dir, int lenSteps);
    int pos() const { return step_ * noteedit::kTicks + tick_; }
    void setPos(int ticks);
    bool targetAll() const { return noteedit::selectedCount(set_) == 0; }

    int step_ = 0;             // cursor: step ...
    int tick_ = 0;             // ... plus ticks into it (multiples of the snap grid)
    int pitch_ = 60;
    int scrollStep_ = 0;
    int topPitch_ = 69;
    int brushTicks_ = noteedit::kTicks; // length of the next note
    int brushVel_ = 100;
    int snapTicks_ = noteedit::kTicks;  // cursor / quantize grid
    Mode mode_ = Mode::Draw;

    noteedit::Set set_;
    uint64_t sel_ = 0;         // selection of (selPattern_, selChannel_)
    int selPattern_ = -1, selChannel_ = -1;
    noteedit::Clip clip_;

    Snapshot undo_[kUndoDepth];
    int undoCount_ = 0, redoCount_ = 0;
    Snapshot redo_[kUndoDepth];

    bool l2Down_ = false, r2Down_ = false, l2Used_ = false, r2Used_ = false;
    uint32_t lastStickMs_ = 0;
};
