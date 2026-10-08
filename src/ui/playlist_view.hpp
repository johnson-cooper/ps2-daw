// Playlist (arrangement): tracks are rows, bars are columns. A pattern clip
// places a pattern on a track for some bars; an audio clip places a sample
// (looped, trimmed, with its own volume and mixer track). The engine plays
// every clip covering the current bar together. Editing goes through Session,
// never the engine.
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
    enum MenuId {
        MenuMode = 1,
        MenuPlayFromBar,
        MenuMute,
        MenuSolo,
        MenuDuplicate,
        MenuMove,
        MenuClearTrack,
        MenuClearAll,
        MenuClearAllConfirm,
        MenuBrush,
        MenuChooseSample,
        MenuEditAudio,
        MenuTrackName,
        MenuPick,
        MenuUngroup,
        MenuTakeOut,
        MenuRegroup,
        MenuSampleBase = 100,
        MenuTakeOutBase = 300,
    };
    enum EditRow { EditVolume, EditLoop, EditTrimStart, EditTrimEnd, EditLength, EditRoute, EditRows };
    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);
    void scrollToCursor();
    void updateAudioEditor(const InputState& in, UiContext& ctx);
    void drawAudioEditor(Gfx& g, UiContext& ctx);
    void drawAudioClip(Gfx& g, UiContext& ctx, int index);
    int firstAudioSlot(const UiContext& ctx, int from, int dir) const;
    int audioBrushBars(const UiContext& ctx) const;

    int track_ = 0;
    int trackScroll_ = 0;  // first visible track (eight rows at a time)
    static constexpr int kVisibleTracks = 8;
    int rowOf(int track) const { const int r = track - trackScroll_; return (r < 0 || r >= kVisibleTracks) ? -1 : r; }
    int bar_ = 0;
    int scroll_ = 0;     // first visible bar
    int brushBars_ = 1;  // length used for newly placed pattern clips
    bool brushInit_ = false;
    bool audioBrush_ = false;   // Cross places the chosen sample instead of the pattern
    int audioSlot_ = -1;        // bank slot of the audio brush
    bool moving_ = false;        // a clip is picked up and follows the cursor
    bool movingAudio_ = false;
    int movingIndex_ = -1;       // audio clip being carried
    PlaylistClip held_ = {};
    bool editingAudio_ = false;  // clip editor open
    int editClip_ = -1;
    int editRow_ = 0;
};
