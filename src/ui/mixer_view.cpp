#include "ui/mixer_view.hpp"

#include <stdio.h>
#include <string.h>

#include "core/strutil.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kStrips = cfg::kMixBuses; // insert page: 8 inserts + master (the channel page scrolls)
constexpr int kStripW = 60;
constexpr int kStripGap = 4;
constexpr int kX0 = theme::kSafeLeft + 4;
constexpr int kY0 = kViewTop + 28;
constexpr int kMeterH = 150;

const char* const kInsertNames[] = {"DRUMS", "BASS", "LEAD", "PAD", "VOX", "FX", "BUS", "SEND"};

int trackOfStrip(int strip) { return strip == cfg::kMixTracks ? 0 : strip + 1; }

int channelsRoutedTo(const Project& p, int track)
{
    int n = 0;
    for (int ch = 0; ch < p.channelCount; ++ch)
        n += p.channels[ch].route == track;
    return n;
}
} // namespace

const char* MixerView::hint() const
{
    if (fx_)
        return focusParams_ ? G_UP G_DOWN " PARAM  " G_LEFT G_RIGHT " ADJUST  L1/R1 x10  " G_SQUARE " DEFAULT  " G_CIRCLE " SLOTS"
                            : G_UP G_DOWN " SLOT  " G_CROSS " EFFECT  " G_SQUARE " BYPASS  L1/R1 MOVE  " G_RIGHT " PARAMS  " G_CIRCLE " BACK";
    if (page_ == 0)
        return G_LEFT G_RIGHT " STRIP  " G_UP G_DOWN " VOL  L1/R1 PAN  " G_SQUARE " MUTE  " G_CROSS " SOLO  " G_TRIANGLE " ROUTE  L2 INSERTS";
    return G_LEFT G_RIGHT " TRACK  " G_UP G_DOWN " VOL  L1/R1 PAN  " G_SQUARE " MUTE  " G_CROSS " SOLO  " G_CIRCLE " EFFECTS  " G_TRIANGLE " MENU  L2 PAGE";
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void MixerView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }
    if (fx_) {
        updateFx(in, ctx);
        return;
    }
    if (in.hit(btn::L2)) {
        page_ ^= 1;
        strip_ = 0;
        ctx.toast(page_ ? "INSERTS: eight tracks with effects" : "CHANNELS: one strip per instrument");
    }
    if (page_ == 0)
        updateChannels(in, ctx);
    else
        updateInserts(in, ctx);
}

void MixerView::updateChannels(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;
    if (in.rep(btn::Left))
        strip_ = strip_ > 0 ? strip_ - 1 : 0;
    const int chans = s.project().channelCount;
    if (in.rep(btn::Right))
        strip_ = strip_ < chans ? strip_ + 1 : chans; // index `chans` is the master
    if (strip_ > chans)
        strip_ = chans;
    if (strip_ < chans) {
        ctx.selectedChannel = strip_;
        if (strip_ < chScroll_)
            chScroll_ = strip_;
        if (strip_ >= chScroll_ + 8)
            chScroll_ = strip_ - 7;
    }

    const int step = in.down(btn::R2) ? 5 : 1; // hold R2 for coarse moves
    int dv = 0;
    if (in.rep(btn::Up))
        dv += step;
    if (in.rep(btn::Down))
        dv -= step;

    const bool stick = (in.ry || in.rx) && ctx.nowMs - lastStickMs_ >= 50;
    if (stick)
        lastStickMs_ = ctx.nowMs;

    if (strip_ == chans) {
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
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

void MixerView::updateInserts(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;
    if (in.rep(btn::Left))
        strip_ = strip_ > 0 ? strip_ - 1 : 0;
    if (in.rep(btn::Right))
        strip_ = strip_ < kStrips - 1 ? strip_ + 1 : kStrips - 1;
    const int t = trackOfStrip(strip_);

    const int step = in.down(btn::R2) ? 5 : 1;
    int dv = 0;
    if (in.rep(btn::Up))
        dv += step;
    if (in.rep(btn::Down))
        dv -= step;
    const bool stick = (in.ry || in.rx) && ctx.nowMs - lastStickMs_ >= 50;
    if (stick) {
        lastStickMs_ = ctx.nowMs;
        if (in.ry)
            dv -= in.ry / 48;
    }
    if (t == 0) {
        if (dv)
            s.setMasterVolume(s.project().masterVolume + dv);
    } else {
        const MixerTrackData& m = s.project().tracks[t];
        if (dv)
            s.setMixVolume(t, m.volume + dv);
        int dp = 0;
        if (in.rep(btn::L1))
            dp -= 5;
        if (in.rep(btn::R1))
            dp += 5;
        if (stick && in.rx)
            dp += in.rx / 24;
        if (dp)
            s.setMixPan(t, m.pan + dp);
        if (in.hit(btn::Square))
            s.setMixMute(t, !m.mute);
        if (in.hit(btn::Cross))
            s.setMixSolo(t, !m.solo);
    }
    if (in.hit(btn::Circle)) {
        fx_ = true;
        slot_ = 0;
        param_ = 0;
        focusParams_ = false;
    }
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

void MixerView::updateFx(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;
    const int t = trackOfStrip(strip_);
    const FxData& f = s.project().tracks[t].fx[slot_];
    const int np = fx::paramCount((FxType)f.type);
    if (!focusParams_) {
        if (in.rep(btn::Up) && slot_ > 0)
            --slot_;
        if (in.rep(btn::Down) && slot_ + 1 < cfg::kFxSlots)
            ++slot_;
        if (in.hit(btn::Cross)) {
            ctx.menu.open("EFFECT");
            for (int ty = 0; ty < kFxTypeCount; ++ty) {
                char buf[40];
                snprintf(buf, sizeof(buf), "%s%s", fx::typeName((FxType)ty), f.type == ty ? "  *" : "");
                ctx.menu.add(MenuFxTypeBase + ty, buf);
            }
        }
        if (in.hit(btn::Square) && f.type != 0)
            s.setFxBypass(t, slot_, !f.bypass);
        if (in.rep(btn::L1) && s.moveFx(t, slot_, -1))
            --slot_;
        if (in.rep(btn::R1) && s.moveFx(t, slot_, +1))
            ++slot_;
        if (in.hit(btn::Right) && np > 0) {
            focusParams_ = true;
            param_ = param_ < np ? param_ : 0;
        }
        if (in.hit(btn::Circle))
            fx_ = false;
        return;
    }
    // Parameter list
    if (param_ >= np)
        param_ = np ? np - 1 : 0;
    if (in.rep(btn::Up) && param_ > 0)
        --param_;
    if (in.rep(btn::Down) && param_ + 1 < np)
        ++param_;
    const ParamDesc& d = fx::param((FxType)f.type, param_);
    const int step = d.step * (in.down(btn::R2) ? 5 : 1);
    if (in.rep(btn::Left))
        s.setFxParam(t, slot_, param_, f.p[param_] - step);
    if (in.rep(btn::Right))
        s.setFxParam(t, slot_, param_, f.p[param_] + step);
    if (in.rep(btn::L1))
        s.setFxParam(t, slot_, param_, f.p[param_] - d.step * 10);
    if (in.rep(btn::R1))
        s.setFxParam(t, slot_, param_, f.p[param_] + d.step * 10);
    if (in.hit(btn::Square))
        s.setFxParam(t, slot_, param_, d.def);
    if (in.hit(btn::Circle))
        focusParams_ = false;
    if (in.hit(btn::Cross))
        s.setFxBypass(t, slot_, !f.bypass);
}

// ---------------------------------------------------------------------------
// menus
// ---------------------------------------------------------------------------

void MixerView::openMenu(UiContext& ctx)
{
    const Project& p = ctx.session.project();
    if (page_ == 0) {
        if (strip_ >= p.channelCount)
            return;
        char title[40];
        snprintf(title, sizeof(title), "CH%d %.8s PLAYS INTO", strip_ + 1, p.channels[strip_].name);
        ctx.menu.open(title);
        for (int t = 0; t < cfg::kMixBuses; ++t) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%s%s", t == 0 ? "MASTER (no insert)" : p.tracks[t].name, p.channels[strip_].route == t ? "  *" : "");
            ctx.menu.add(MenuRouteBase + t, buf);
        }
        return;
    }
    const int t = trackOfStrip(strip_);
    char title[40];
    snprintf(title, sizeof(title), "%s", t == 0 ? "MASTER" : p.tracks[t].name);
    ctx.menu.open(title);
    ctx.menu.add(MenuRoute, "Assign channels to this track...", t != 0);
    ctx.menu.add(MenuRename, "Name: next preset", t != 0);
    ctx.menu.add(MenuClearFx, "Clear all effects on this track");
    ctx.menu.add(MenuClearLatch, "Reset clip indicators");
}

void MixerView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    const Project& p = s.project();
    if (id >= MenuRouteBase && id < MenuRouteBase + cfg::kMixBuses) {
        s.setRoute(strip_, id - MenuRouteBase);
        ctx.toast("CH%d plays into %s", strip_ + 1, id == MenuRouteBase ? "MASTER" : p.tracks[id - MenuRouteBase].name);
        return;
    }
    if (id >= MenuChannelBase && id < MenuChannelBase + cfg::kMaxChannels) {
        const int ch = id - MenuChannelBase, t = trackOfStrip(strip_);
        s.setRoute(ch, p.channels[ch].route == t ? 0 : t);
        // reopen so several channels can be toggled in a row
        handleMenu(MenuRoute, ctx);
        return;
    }
    if (id >= MenuFxTypeBase && id < MenuFxTypeBase + kFxTypeCount) {
        const int t = trackOfStrip(strip_);
        s.setFxType(t, slot_, (FxType)(id - MenuFxTypeBase));
        if (id != MenuFxTypeBase)
            ctx.toast("%s on %s", fx::typeName((FxType)(id - MenuFxTypeBase)), t == 0 ? "MASTER" : p.tracks[t].name);
        param_ = 0;
        return;
    }
    const int t = trackOfStrip(strip_);
    switch (id) {
    case MenuRoute: {
        ctx.menu.open("CHANNELS ON THIS TRACK");
        for (int ch = 0; ch < p.channelCount; ++ch) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%d %-8.8s %s", ch + 1, p.channels[ch].name, p.channels[ch].route == t ? "ON" : "--");
            ctx.menu.add(MenuChannelBase + ch, buf);
        }
        break;
    }
    case MenuRename: {
        int next = 0;
        for (int i = 0; i < 8; ++i)
            if (str::equalsNoCase(p.tracks[t].name, kInsertNames[i]))
                next = i + 1;
        char name[24];
        if (next >= 8)
            snprintf(name, sizeof(name), "INS %d", t);
        else
            snprintf(name, sizeof(name), "%s", kInsertNames[next]);
        s.setMixerTrackName(t, name);
        ctx.toast("Track %d: %s", t, name);
        break;
    }
    case MenuClearFx:
        for (int sl = 0; sl < cfg::kFxSlots; ++sl)
            s.setFxType(t, sl, FxType::None);
        ctx.toast("Effects cleared");
        break;
    case MenuClearLatch:
        s.clearClipLatch();
        break;
    }
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void MixerView::draw(Gfx& g, UiContext& ctx)
{
    if (fx_)
        drawFx(g, ctx);
    else if (page_ == 0)
        drawChannels(g, ctx);
    else
        drawInserts(g, ctx);
}

void MixerView::drawChannels(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const EngineStatus& st = ctx.engine.status();
    ui::panel(g, theme::kSafeLeft, kViewTop, theme::kSafeRight - theme::kSafeLeft, kViewBottom - kViewTop, "MIXER: CHANNELS");
    g.text(theme::kSafeLeft + 250, kViewTop + 3, "L2: INSERTS", theme::kTextDim);

    if (p.channelCount > 8) {
        char more[40];
        snprintf(more, sizeof(more), "CH %d-%d/%d", chScroll_ + 1, chScroll_ + 8, p.channelCount);
        g.text(theme::kSafeRight - 8 - Gfx::textWidth(more), kViewTop + 3, more, theme::kTextDim);
    }
    for (int pos = 0; pos < 9; ++pos) {
        const bool master = pos == 8;
        const int i = master ? p.channelCount : chScroll_ + pos;
        if (!master && i >= p.channelCount)
            continue;
        const int x = kX0 + pos * (kStripW + kStripGap) + (master ? 6 : 0);
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
            ui::vmeter(g, x + 6, my, 10, kMeterH + 20, st.meterL);
            ui::vmeter(g, x + 18, my, 10, kMeterH + 20, st.meterR);
        } else if (p.channels[i].voiceMode == (uint8_t)VoiceMode::Spu2) {
            g.fillRect(x + 6, my, 22, kMeterH + 20, theme::kPanelDark);
            g.text(x + 7, my + kMeterH / 2 - 8, "HW", theme::kHardware, 2, 2);
        } else {
            ui::vmeter(g, x + 8, my, 14, kMeterH + 20, st.meter[i]);
        }
        ui::vfader(g, x + 36, my, 16, kMeterH + 20, volume, 100, sel);

        char vol[8];
        snprintf(vol, sizeof(vol), "%3d", volume);
        g.text(x + 6, my + kMeterH + 26, vol, theme::kText, 2, 2);

        const int by = my + kMeterH + 46;
        if (!master) {
            const ChannelData& c = p.channels[i];
            ui::hslider(g, x + 4, by - 6, kStripW - 8, 8, c.pan, 100, true, false, false);
            ui::button(g, x + 4, by + 6, 26, 18, "M", c.mute != 0, false, theme::kMute);
            ui::button(g, x + 32, by + 6, 26, 18, "S", c.solo != 0, false, theme::kSolo);
            // The insert this channel plays into (Triangle changes it).
            if (c.route == 0)
                g.text(x + 4, by + 28, G_RIGHT "MST", theme::kTextDim, 1, 2);
            else
            {
                char rt[8];
                snprintf(rt, sizeof(rt), G_RIGHT "INS%d", c.route);
                g.text(x + 4, by + 28, rt, theme::kAccent, 1, 2);
            }
        } else {
            char db[16];
            const int l = ui::levelToDb(st.meterL > st.meterR ? st.meterL : st.meterR);
            if (l <= -96)
                snprintf(db, sizeof(db), " -inf");
            else
                snprintf(db, sizeof(db), "%+3ddB", l);
            g.text(x + 2, by - 4, db, theme::kTextDim, 1, 2);
            g.text(x + 2, by + 12, st.busClip[0] ? "CLIPPED" : "no clip", st.busClip[0] ? theme::kError : theme::kTextDim, 1, 2);
        }
    }
}

void MixerView::drawInserts(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const EngineStatus& st = ctx.engine.status();
    ui::panel(g, theme::kSafeLeft, kViewTop, theme::kSafeRight - theme::kSafeLeft, kViewBottom - kViewTop, "MIXER: INSERTS");
    g.text(theme::kSafeLeft + 250, kViewTop + 3, "L2: CHANNELS", theme::kTextDim);
    if (st.fxStarved)
        g.text(theme::kSafeLeft + 380, kViewTop + 3, "FX MEMORY FULL", theme::kError);

    bool anySolo = false;
    for (int t = 1; t < cfg::kMixBuses; ++t)
        anySolo |= p.tracks[t].solo != 0;

    for (int i = 0; i < kStrips; ++i) {
        const int t = trackOfStrip(i);
        const bool master = t == 0;
        const int x = kX0 + i * (kStripW + kStripGap) + (master ? 6 : 0);
        const bool sel = i == strip_;
        const MixerTrackData& m = p.tracks[t];
        g.fillRect(x, kY0, kStripW, kViewBottom - kY0 - 6, sel ? theme::kPanelHeader : theme::kPanelDark);

        char name[7];
        snprintf(name, sizeof(name), "%.5s", master ? "MSTR" : m.name);
        g.text(x + 2, kY0 + 4, name, sel ? theme::kAccent : theme::kText, 1, 2);
        const int routed = master ? 0 : channelsRoutedTo(p, t);
        if (!master)
            g.textf(x + kStripW - 14, kY0 + 4, routed ? theme::kAlive : theme::kTextDim, "%d", routed);

        const int my = kY0 + 28;
        // clip lamp over the meters
        g.fillRect(x + 6, my - 8, 22, 5, st.busClip[t] ? theme::kError : theme::kCellOffAlt);
        ui::vmeter(g, x + 6, my, 10, kMeterH, st.busMeterL[t]);
        ui::vmeter(g, x + 18, my, 10, kMeterH, st.busMeterR[t]);
        const int volume = master ? p.masterVolume : m.volume;
        ui::vfader(g, x + 36, my, 16, kMeterH, volume, 100, sel);

        char vol[8];
        snprintf(vol, sizeof(vol), "%3d", volume);
        g.text(x + 6, my + kMeterH + 4, vol, theme::kText, 2, 2);

        const int by = my + kMeterH + 26;
        if (!master) {
            ui::hslider(g, x + 4, by, kStripW - 8, 8, m.pan, 100, true, false, false);
            const bool dim = anySolo && !m.solo;
            ui::button(g, x + 4, by + 12, 26, 18, "M", m.mute != 0, false, theme::kMute);
            ui::button(g, x + 32, by + 12, 26, 18, "S", m.solo != 0, false, dim ? theme::kTextDim : theme::kSolo);
        } else {
            char db[16];
            const int l = ui::levelToDb(st.meterL > st.meterR ? st.meterL : st.meterR);
            if (l <= -96)
                snprintf(db, sizeof(db), " -inf");
            else
                snprintf(db, sizeof(db), "%+3ddB", l);
            g.text(x + 2, by, db, theme::kTextDim, 1, 2);
            g.text(x + 2, by + 14, st.busClip[0] ? "CLIPPED" : "no clip", st.busClip[0] ? theme::kError : theme::kTextDim, 1, 2);
        }
        // effect chain: four squares, lit when an effect sits in the slot
        for (int sl = 0; sl < cfg::kFxSlots; ++sl) {
            const FxData& f = m.fx[sl];
            uint32_t c = theme::kCellOffAlt;
            if (f.type)
                c = f.bypass ? theme::kCellOnDim : theme::kAccent;
            g.fillRect(x + 4 + sl * 14, by + 34, 11, 9, c);
        }
        if (sel && st.busGr[t] > 0)
            g.textf(x + 2, by + 46, theme::kWarning, "GR%d", st.busGr[t] / 10);
    }
}

void MixerView::drawFx(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const EngineStatus& st = ctx.engine.status();
    const int t = trackOfStrip(strip_);
    const MixerTrackData& m = p.tracks[t];
    char title[48];
    snprintf(title, sizeof(title), "EFFECTS: %s", t == 0 ? "MASTER" : m.name);
    ui::panel(g, theme::kSafeLeft, kViewTop, theme::kSafeRight - theme::kSafeLeft, kViewBottom - kViewTop, title);

    const int sx = theme::kSafeLeft + 8, sy = kViewTop + 34, sw = 190, sh = 40;
    for (int sl = 0; sl < cfg::kFxSlots; ++sl) {
        const FxData& f = m.fx[sl];
        const int y = sy + sl * (sh + 6);
        const bool on = slot_ == sl;
        g.fillRect(sx, y, sw, sh, on ? theme::kPanelHeader : theme::kPanelDark);
        if (on)
            g.fillRect(sx, y, 4, sh, focusParams_ ? theme::kTextDim : theme::kSelect);
        g.textf(sx + 10, y + 4, f.type ? theme::kText : theme::kTextDim, "%d %s", sl + 1, fx::typeName((FxType)f.type));
        if (f.type) {
            g.text(sx + 10, y + 22, f.bypass ? "BYPASSED" : "ACTIVE", f.bypass ? theme::kWarning : theme::kAlive, 1, 2);
            if (f.type == (uint8_t)FxType::Compressor && st.busGr[t] > 0 && !f.bypass)
                {
                char gr[24];
                snprintf(gr, sizeof(gr), "GR %d.%d dB", st.busGr[t] / 10, st.busGr[t] % 10);
                g.text(sx + 100, y + 22, gr, theme::kWarning, 1, 2);
            }
        } else {
            g.text(sx + 10, y + 22, G_CROSS " add effect", theme::kTextDim, 1, 2);
        }
    }

    const int px = sx + sw + 14, pw = theme::kSafeRight - px - 8;
    const FxData& f = m.fx[slot_];
    const int np = fx::paramCount((FxType)f.type);
    g.text(px, sy, f.type ? fx::typeName((FxType)f.type) : "EMPTY SLOT", theme::kAccent);
    for (int i = 0; i < np; ++i) {
        const ParamDesc& d = fx::param((FxType)f.type, i);
        char val[24];
        params::format(d, f.p[i], val, sizeof(val));
        ui::paramRow(g, px, sy + 24 + i * 22, pw, d.name, val, (f.p[i] - d.min) * 1000 / (d.max - d.min), focusParams_ && i == param_,
                     focusParams_ && i == param_);
    }
    if (!f.type)
        g.text(px, sy + 28, "Press " G_CROSS " to choose an effect.", theme::kTextDim, 1, 2);

    // Level of this track while you tweak.
    const int my = kViewBottom - 34;
    g.text(sx, my - 14, "LEVEL", theme::kTextDim, 1, 2);
    ui::hmeter(g, sx + 50, my - 14, 220, 8, st.busMeterL[t], st.busClip[t]);
    ui::hmeter(g, sx + 50, my - 4, 220, 8, st.busMeterR[t], st.busClip[t]);
    if (st.fxStarved)
        g.text(sx + 290, my - 14, "FX MEMORY FULL: delay/reverb passes audio", theme::kError, 1, 2);
    else if (f.type == (uint8_t)FxType::Delay || f.type == (uint8_t)FxType::Reverb)
        g.text(sx + 290, my - 14, "Tail rings on after the notes stop", theme::kTextDim, 1, 2);
}
