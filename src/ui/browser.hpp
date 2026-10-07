// Sample browser.
//
// Two sources, switched with L2:
//  * LOADED: every sample currently in the bank (built-in kit + imports).
//  * USB:    <root>/PS2DAW/SAMPLES and its sub-folders on mass0:/mass1:.
//
// Directory listings are bounded (kMaxEntries) and hold metadata only; a
// file is read only when the user acts on it, and then incrementally by the
// SampleLibrary, never on the audio thread.
#pragma once

#include "platform/ps2_filesystem.hpp"
#include "ui/view.hpp"

class BrowserView : public View {
public:
    static constexpr int kMaxEntries = 96;

    const char* tabName() const override { return "BROWSER"; }
    const char* hint() const override;
    void onEnter(UiContext& ctx) override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum Source { SrcLoaded = 0, SrcUsb = 1 };
    enum MenuId {
        MenuLoad = 1,
        MenuAssign,
        MenuPreviewSpu2,
        MenuUploadSpu2,
        MenuRemoveSpu2,
        MenuUnload,
        MenuRescan,
        MenuCreateDir,
    };

    int itemCount(const UiContext& ctx) const;
    void rebuildLoadedList(const UiContext& ctx);
    void scan(UiContext& ctx);
    void clampSel(const UiContext& ctx);
    // Reference for the selected USB entry, or false (with a toast).
    bool selectedRef(UiContext& ctx, char* ref, size_t cap, bool toastErrors);
    int selectedSlot(const UiContext& ctx) const;       // bank slot of the selection, or -1
    void act(UiContext& ctx, int action);               // Assign / Preview / ... on the selection
    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);
    void describe(const UiContext& ctx, char lines[3][80]) const;

    int source_ = SrcLoaded;
    int sel_[2] = {0, 0};
    int first_[2] = {0, 0};

    // USB directory state
    char dirRel_[64] = "";
    Storage::DirEntry entries_[kMaxEntries];
    int entryCount_ = 0;
    bool truncated_ = false;
    bool scanned_ = false;
    bool scanFailed_ = true;
    uint32_t lastRootMask_ = 0;

    // LOADED source: bank slots in display order
    int loadedSlots_[cfg::kMaxSamples];
    int loadedCount_ = 0;
};
