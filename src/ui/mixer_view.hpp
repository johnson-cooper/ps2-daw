// Mixer: two pages and an effect editor.
//
//  * CHANNELS page: one strip per rack channel (volume, pan, mute, solo,
//    meter) and the insert the channel plays into.
//  * INSERTS page: the real mixing board. Eight insert tracks plus the master,
//    each with fader, pan, mute/solo, stereo peak meters with clip indicators
//    and a four-slot effect chain.
//  * EFFECTS editor (Circle on an insert): choose, reorder, bypass and tweak
//    the effects of that track.
#pragma once

#include "ui/view.hpp"

class MixerView : public View {
public:
    const char* tabName() const override { return "MIXER"; }
    const char* hint() const override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum MenuId {
        MenuRoute = 1,
        MenuRename,
        MenuClearFx,
        MenuClearLatch,
        MenuPage,
        MenuRouteBase = 100,   // + track (channel page: choose the insert)
        MenuChannelBase = 200, // + channel (insert page: toggle channels onto this insert)
        MenuFxTypeBase = 300,  // + FxType
    };
    void updateChannels(const InputState& in, UiContext& ctx);
    void updateInserts(const InputState& in, UiContext& ctx);
    void updateFx(const InputState& in, UiContext& ctx);
    void drawChannels(Gfx& g, UiContext& ctx);
    void drawInserts(Gfx& g, UiContext& ctx);
    void drawFx(Gfx& g, UiContext& ctx);
    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);

    int page_ = 0;      // 0 channels, 1 inserts
    int chScroll_ = 0;  // channel page: first visible channel
    int strip_ = 0;     // channel page: 0..7 (8 = master); insert page: 0..7 = insert 1..8, 8 = master
    bool fx_ = false;   // effect editor open
    int slot_ = 0;
    int param_ = 0;
    bool focusParams_ = false;
    uint32_t lastStickMs_ = 0;
};
