#include "ui/mixer_view.hpp"

#include <stdio.h>

#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kStrips = cfg::kMaxChannels + 1;
constexpr int kStripW = 60;
constexpr int kStripGap = 4;
constexpr int kX0 = theme::kSafeLeft + 4;
constexpr int kY0 = kViewTop + 28;
constexpr int kMeterH = 170;
} // namespace

const char* MixerView::hint() const
{
    return G_LEFT G_RIGHT " STRIP  " G_UP G_DOWN " VOLUME  L1/R1 PAN  " G_SQUARE " MUTE  " G_CROSS " SOLO";
}

void MixerView::update(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;
    if (in.rep(btn::Left))
        strip_ = strip_ > 0 ? strip_ - 1 : 0;
    if (in.rep(btn::Right))
        strip_ = strip_ < kStrips - 1 ? strip_ + 1 : kStrips - 1;
    if (strip_ < cfg::kMaxChannels)
        ctx.selectedChannel = strip_;

    const int step = in.down(btn::R2) ? 5 : 1; // hold R2 for coarse moves
    int dv = 0;
    if (in.rep(btn::Up))
        dv += step;
    if (in.rep(btn::Down))
        dv -= step;

    static uint32_t lastStickMs = 0;
    const bool stick = (in.ry || in.rx) && ctx.nowMs - lastStickMs >= 50;
    if (stick)
        lastStickMs = ctx.nowMs;

    if (strip_ == cfg::kMaxChannels) {
        if (stick && in.ry)
            dv -= in.ry / 48;
        if (dv)
            s.setMasterVolume(s.project().masterVolume + dv);
        return;
    }

    const ChannelData& c = s.project().channels[strip_];
    if (stick && in.ry)
        dv -= in.ry / 48;
    if (dv)
        s.setVolume(strip_, c.volume + dv);
    int dp = 0;
    if (in.rep(btn::L1))
        dp -= 5;
    if (in.rep(btn::R1))
        dp += 5;
    if (stick && in.rx)
        dp += in.rx / 24;
    if (dp)
        s.setPan(strip_, c.pan + dp);
    if (in.hit(btn::Square))
        s.setMute(strip_, !c.mute);
    if (in.hit(btn::Cross))
        s.setSolo(strip_, !c.solo);
    if (in.hit(btn::Circle))
        s.previewChannel(strip_);
}

void MixerView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const EngineStatus& st = ctx.engine.status();
    ui::panel(g, theme::kSafeLeft, kViewTop, theme::kSafeRight - theme::kSafeLeft, kViewBottom - kViewTop, "MIXER");

    for (int i = 0; i < kStrips; ++i) {
        const bool master = i == cfg::kMaxChannels;
        const int x = kX0 + i * (kStripW + kStripGap) + (master ? 6 : 0);
        const bool sel = i == strip_;
        g.fillRect(x, kY0, kStripW, kViewBottom - kY0 - 6, sel ? theme::kPanelHeader : theme::kPanelDark);

        char name[6];
        if (master)
            snprintf(name, sizeof(name), "MSTR");
        else
            snprintf(name, sizeof(name), "%.5s", p.channels[i].name);
        g.text(x + 2, kY0 + 4, name, sel ? theme::kAccent : theme::kText, 1, 2);
        if (!master)
            ui::led(g, x + kStripW - 12, kY0 + 8, 8, ctx.channelActive(i), theme::kAlive);

        const int my = kY0 + 26;
        const int volume = master ? p.masterVolume : p.channels[i].volume;
        if (master) {
            ui::vmeter(g, x + 6, my, 10, kMeterH, st.meterL);
            ui::vmeter(g, x + 18, my, 10, kMeterH, st.meterR);
        } else if (p.channels[i].voiceMode == (uint8_t)VoiceMode::Spu2) {
            g.fillRect(x + 6, my, 22, kMeterH, theme::kPanelDark);
            g.text(x + 7, my + kMeterH / 2 - 8, "HW", theme::kHardware, 2, 2);
        } else {
            ui::vmeter(g, x + 8, my, 14, kMeterH, st.meter[i]);
        }
        ui::vfader(g, x + 36, my, 16, kMeterH, volume, 100, sel);

        char vol[8];
        snprintf(vol, sizeof(vol), "%3d", volume);
        g.text(x + 6, my + kMeterH + 6, vol, theme::kText, 2, 2);

        const int by = my + kMeterH + 28;
        if (!master) {
            const ChannelData& c = p.channels[i];
            ui::hslider(g, x + 4, by, kStripW - 8, 8, c.pan, 100, true, false, false);
            ui::button(g, x + 4, by + 14, 26, 20, "M", c.mute != 0, false, theme::kMute);
            ui::button(g, x + 32, by + 14, 26, 20, "S", c.solo != 0, false, theme::kSolo);
            g.text(x + 4, by + 40, G_RIGHT "MST", theme::kTextDim, 1, 2); // routing: master bus
        } else {
            char db[16];
            const int l = ui::levelToDb(st.meterL > st.meterR ? st.meterL : st.meterR);
            if (l <= -96)
                snprintf(db, sizeof(db), " -inf");
            else
                snprintf(db, sizeof(db), "%+3ddB", l);
            g.text(x + 2, by + 2, db, theme::kTextDim, 1, 2);
            g.text(x + 2, by + 22, st.clipSamples ? "CLIPPED" : "no clip", st.clipSamples ? theme::kError : theme::kTextDim, 1, 2);
        }
    }
}
