// Piano roll for the selected rack channel and the current pattern.
//
// Rows are pitches (MIDI note numbers; 60 plays the sample at its native
// speed), columns are 16th-note steps. Notes can be chords, any length, and
// are stored per pattern and channel next to the rack's step grid. For a
// "sustained" channel (menu: Instrument mode) a note's length cuts the sample
// so it behaves like a played instrument; otherwise samples play to the end.
#pragma once

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

private:
    enum MenuId {
        MenuGate = 1,
        MenuVelocity,
        MenuOctaveUp,
        MenuOctaveDown,
        MenuSemiUp,
        MenuSemiDown,
        MenuClear,
        MenuClearConfirm,
    };

    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);
    void clampView(const UiContext& ctx);

    int step_ = 0;
    int pitch_ = 60;
    int scrollStep_ = 0;
    int topPitch_ = 69;
    int brushLen_ = 1;
    int brushVel_ = 100;
};
