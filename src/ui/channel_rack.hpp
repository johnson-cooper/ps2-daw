// Channel Rack / step sequencer: the primary editing screen.
//
// Rows are channels; columns are [mute] [name] [volume] [pan] [steps...].
// Patterns longer than 16 steps scroll in pages of 16 that follow the cursor.
#pragma once

#include "ui/view.hpp"

class ChannelRackView : public View {
public:
    const char* tabName() const override { return "RACK"; }
    const char* hint() const override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum Column { ColMute = 0, ColName, ColVolume, ColPan, ColFirstStep };
    static constexpr int kVisibleSteps = 16;

    enum MenuId {
        MenuPreview = 1,
        MenuSample,
        MenuVoiceMode,
        MenuSolo,
        MenuFill4,
        MenuFill2,
        MenuFill1,
        MenuClearChannel,
        MenuClearPattern,
        MenuLength,
        MenuStop,
        MenuSampleBase = 100,
        MenuLengthBase = 200,
    };

    void openChannelMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);
    void adjustValue(int delta, UiContext& ctx);
    int stepAtColumn() const { return col_ - ColFirstStep + scroll_; }
    void clampCursor(const UiContext& ctx);

    int col_ = ColFirstStep;
    int scroll_ = 0;     // first visible step
    bool editing_ = false;
    int menuMode_ = 0;   // 0 = channel menu, 1 = sample picker, 2 = length picker
};
