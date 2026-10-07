// Playlist (arrangement): tracks are rows, bars are columns. A clip places a
// pattern on a track for some bars; the engine plays all clips covering the
// current bar together. Editing goes through Session, never the engine.
#pragma once

#include "ui/view.hpp"

class PlaylistView : public View {
public:
    static constexpr int kVisibleBars = 16;

    const char* tabName() const override { return "SONG"; }
    const char* hint() const override;
    void onEnter(UiContext& ctx) override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum MenuId { MenuMode = 1, MenuClearTrack, MenuClearAll, MenuClearAllConfirm };

    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);
    void scrollToCursor();

    int track_ = 0;
    int bar_ = 0;
    int scroll_ = 0;     // first visible bar
    int brushBars_ = 1;  // length used for newly placed clips
    bool brushInit_ = false;
};
