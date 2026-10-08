#include "ui/instrument_view.hpp"

#include <stdio.h>
#include <string.h>

#include "ui/font.hpp"
#include "ui/piano_roll.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kRowX = theme::kSafeLeft + 6;
constexpr int kRowW = 330;
constexpr int kRowsY = kViewTop + 52;
constexpr int kRowH = 22;
constexpr int kRightX = kRowX + kRowW + 14;
} // namespace

const char* InstrumentView::hint() const
{
    return G_UP G_DOWN " ROW  " G_LEFT G_RIGHT " ADJUST  L1/R1 x10  " G_SQUARE " HEAR  " G_CIRCLE " RESET  " G_TRIANGLE " MENU  L2/R2 CH";
}

void InstrumentView::onEnter(UiContext&)
{
    sel_ = 0;
    scroll_ = 0;
}

int InstrumentView::buildRows(const UiContext& ctx, Row* rows) const
{
    const InstrumentData& in = ctx.session.project().channels[ctx.selectedChannel].inst;
    int n = 0;
    rows[n++] = {RowKind, 0};
    rows[n++] = {RowRoute, 0};
    if (in.kind == (uint8_t)InstrKind::Synth) {
        rows[n++] = {RowPreset, 0};
        for (int i = 0; i < instr::synthParamCount(); ++i)
            rows[n++] = {RowSynth, (uint8_t)i};
        for (int i = 1; i < cfg::kEnvParams; ++i) // a synth always runs its envelope
            rows[n++] = {RowEnv, (uint8_t)i};
    } else {
        for (int i = 0; i < cfg::kEnvParams; ++i)
            rows[n++] = {RowEnv, (uint8_t)i};
    }
    return n;
}

void InstrumentView::valueText(const Row& row, const UiContext& ctx, char* out, size_t cap, int* frac) const
{
    const Project& p = ctx.session.project();
    const int ch = ctx.selectedChannel;
    const InstrumentData& in = p.channels[ch].inst;
    *frac = -1;
    switch (row.type) {
    case RowKind:
        snprintf(out, cap, "%s", in.kind == (uint8_t)InstrKind::Synth ? "SYNTH" : "SAMPLER");
        break;
    case RowRoute: {
        const int r = p.channels[ch].route;
        snprintf(out, cap, "%s", r == 0 ? "MASTER" : p.tracks[r].name);
        *frac = r * 1000 / cfg::kMixTracks;
        break;
    }
    case RowPreset:
        snprintf(out, cap, "%s", presetIdx_[ch] >= 0 ? instr::synthPreset(presetIdx_[ch]).name : "CUSTOM");
        break;
    case RowSynth: {
        const ParamDesc& d = instr::synthParam(row.index);
        params::format(d, in.synth[row.index], out, cap);
        *frac = (in.synth[row.index] - d.min) * 1000 / (d.max - d.min);
        break;
    }
    case RowEnv: {
        const ParamDesc& d = instr::envParam(row.index);
        params::format(d, in.env[row.index], out, cap);
        *frac = (in.env[row.index] - d.min) * 1000 / (d.max - d.min);
        break;
    }
    }
}

void InstrumentView::adjust(const Row& row, int dir, int mult, UiContext& ctx)
{
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel;
    const Project& p = s.project();
    switch (row.type) {
    case RowKind:
        s.setChannelKind(ch, p.channels[ch].inst.kind == (uint8_t)InstrKind::Synth ? InstrKind::Sampler : InstrKind::Synth);
        sel_ = 0;
        break;
    case RowRoute: {
        int r = p.channels[ch].route + dir;
        r = r < 0 ? cfg::kMixTracks : (r > cfg::kMixTracks ? 0 : r);
        s.setRoute(ch, r);
        break;
    }
    case RowPreset: {
        const int n = instr::synthPresetCount();
        int i = presetIdx_[ch] < 0 ? (dir > 0 ? 0 : n - 1) : (presetIdx_[ch] + dir + n) % n;
        presetIdx_[ch] = (int8_t)i;
        s.applySynthPreset(ch, i);
        s.previewChannel(ch, auditionPitch_ - 60);
        break;
    }
    case RowSynth: {
        const ParamDesc& d = instr::synthParam(row.index);
        s.setSynthParam(ch, row.index, p.channels[ch].inst.synth[row.index] + dir * d.step * mult);
        presetIdx_[ch] = -1;
        break;
    }
    case RowEnv: {
        const ParamDesc& d = instr::envParam(row.index);
        s.setEnvParam(ch, row.index, p.channels[ch].inst.env[row.index] + dir * d.step * mult);
        break;
    }
    }
}

void InstrumentView::resetRow(const Row& row, UiContext& ctx)
{
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel;
    if (row.type == RowSynth) {
        s.setSynthParam(ch, row.index, instr::synthParam(row.index).def);
        presetIdx_[ch] = -1;
    } else if (row.type == RowEnv) {
        s.setEnvParam(ch, row.index, instr::envParam(row.index).def);
    } else if (row.type == RowRoute) {
        s.setRoute(ch, 0);
    }
}

void InstrumentView::openMenu(UiContext& ctx)
{
    const ChannelData& c = ctx.session.project().channels[ctx.selectedChannel];
    char title[40];
    snprintf(title, sizeof(title), "INSTRUMENT CH%d: %.8s", ctx.selectedChannel + 1, c.name);
    ctx.menu.open(title);
    ctx.menu.add(MenuKind, c.inst.kind == (uint8_t)InstrKind::Synth ? "Switch to SAMPLER" : "Switch to SYNTH");
    ctx.menu.add(MenuSynthPreset, "Load synth preset...");
    ctx.menu.add(MenuEnvPreset, "Load envelope preset...");
    ctx.menu.add(MenuRoute, "Mixer track...");
    char buf[40];
    PianoRollView::noteName(auditionPitch_, buf, sizeof(buf));
    char line[80];
    snprintf(line, sizeof(line), "Audition note +1 octave (%s)", buf);
    ctx.menu.add(MenuAuditionUp, line, auditionPitch_ + 12 <= 127);
    ctx.menu.add(MenuAuditionDown, "Audition note -1 octave", auditionPitch_ - 12 >= 0);
    ctx.menu.add(MenuReset, "Reset instrument settings");
}

void InstrumentView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel;
    if (id >= MenuSynthPresetBase && id < MenuSynthPresetBase + 50) {
        const int i = id - MenuSynthPresetBase;
        s.applySynthPreset(ch, i);
        presetIdx_[ch] = (int8_t)i;
        s.previewChannel(ch, auditionPitch_ - 60);
        sel_ = 0;
        ctx.toast("Synth preset: %s", instr::synthPreset(i).name);
        return;
    }
    if (id >= MenuEnvPresetBase && id < MenuEnvPresetBase + 50) {
        s.applyEnvPreset(ch, id - MenuEnvPresetBase);
        s.previewChannel(ch, auditionPitch_ - 60);
        ctx.toast("Envelope: %s", instr::envPresetName(id - MenuEnvPresetBase));
        return;
    }
    if (id >= MenuRouteBase && id < MenuRouteBase + cfg::kMixBuses) {
        s.setRoute(ch, id - MenuRouteBase);
        ctx.toast("CH%d plays into %s", ch + 1, id == MenuRouteBase ? "MASTER" : s.project().tracks[id - MenuRouteBase].name);
        return;
    }
    switch (id) {
    case MenuKind:
        s.setChannelKind(ch, s.project().channels[ch].inst.kind == (uint8_t)InstrKind::Synth ? InstrKind::Sampler : InstrKind::Synth);
        sel_ = 0;
        break;
    case MenuSynthPreset:
        ctx.menu.open("SYNTH PRESETS");
        for (int i = 0; i < instr::synthPresetCount(); ++i)
            ctx.menu.add(MenuSynthPresetBase + i, instr::synthPreset(i).name);
        break;
    case MenuEnvPreset:
        ctx.menu.open("ENVELOPE PRESETS");
        for (int i = 0; i < instr::envPresetCount(); ++i)
            ctx.menu.add(MenuEnvPresetBase + i, instr::envPresetName(i));
        break;
    case MenuRoute:
        ctx.menu.open("MIXER TRACK");
        for (int t = 0; t < cfg::kMixBuses; ++t) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%s%s", t == 0 ? "MASTER (no insert)" : s.project().tracks[t].name,
                     s.project().channels[ch].route == t ? "  *" : "");
            ctx.menu.add(MenuRouteBase + t, buf);
        }
        break;
    case MenuAuditionUp:
        auditionPitch_ += 12;
        break;
    case MenuAuditionDown:
        auditionPitch_ -= 12;
        break;
    case MenuReset: {
        InstrumentData d;
        instr::setDefaults(d);
        s.setChannelKind(ch, InstrKind::Sampler);
        for (int i = 0; i < cfg::kEnvParams; ++i)
            s.setEnvParam(ch, i, d.env[i]);
        for (int i = 0; i < instr::synthParamCount(); ++i)
            s.setSynthParam(ch, i, d.synth[i]);
        presetIdx_[ch] = -1;
        sel_ = 0;
        ctx.toast("Instrument reset");
        break;
    }
    }
}

void InstrumentView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }
    Session& s = ctx.session;
    const int channels = s.project().channelCount;
    if (in.hit(btn::L2) || in.hit(btn::R2)) {
        ctx.selectedChannel = (ctx.selectedChannel + (in.hit(btn::R2) ? 1 : channels - 1)) % channels;
        sel_ = 0;
        ctx.toast("Channel %d: %s", ctx.selectedChannel + 1, s.project().channels[ctx.selectedChannel].name);
        return;
    }
    Row rows[40];
    const int n = buildRows(ctx, rows);
    if (sel_ >= n)
        sel_ = n - 1;
    if (in.rep(btn::Up) && sel_ > 0)
        --sel_;
    if (in.rep(btn::Down) && sel_ + 1 < n)
        ++sel_;
    if (sel_ < scroll_)
        scroll_ = sel_;
    if (sel_ >= scroll_ + kVisibleRows)
        scroll_ = sel_ - kVisibleRows + 1;

    const int mult = 1;
    if (in.rep(btn::Left))
        adjust(rows[sel_], -1, mult, ctx);
    if (in.rep(btn::Right))
        adjust(rows[sel_], +1, mult, ctx);
    if (in.rep(btn::L1))
        adjust(rows[sel_], -1, 10, ctx);
    if (in.rep(btn::R1))
        adjust(rows[sel_], +1, 10, ctx);
    if (in.hit(btn::Cross)) {
        if (rows[sel_].type == RowKind || rows[sel_].type == RowPreset)
            adjust(rows[sel_], +1, 1, ctx);
        else
            s.previewChannel(ctx.selectedChannel, auditionPitch_ - 60);
    }
    if (in.hit(btn::Square))
        s.previewChannel(ctx.selectedChannel, auditionPitch_ - 60);
    if (in.hit(btn::Circle))
        resetRow(rows[sel_], ctx);
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

void InstrumentView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int ch = ctx.selectedChannel;
    const ChannelData& c = p.channels[ch];
    const bool synth = c.inst.kind == (uint8_t)InstrKind::Synth;
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, nullptr);
    g.fillRect(x, kViewTop, w, 22, theme::kPanelHeader);
    g.textf(x + 8, kViewTop + 3, theme::kText, "INSTRUMENT  CH%d %.8s", ch + 1, c.name);
    g.text(x + 330, kViewTop + 3, synth ? "SYNTH" : "SAMPLER", synth ? theme::kHardware : theme::kAlive);
    g.textf(x + 430, kViewTop + 3, theme::kTextDim, "-> %s", c.route == 0 ? "MASTER" : p.tracks[c.route].name);
    // Channel strip row
    g.textf(x + 8, kViewTop + 28, theme::kTextDim, "VOL %d  PAN %d  %s  VOICES %lu", c.volume, c.pan,
            c.voiceMode == (uint8_t)VoiceMode::Spu2 ? "SPU2 (envelope/synth need SW)" : "SOFTWARE",
            (unsigned long)ctx.engine.status().voicesActive);

    Row rows[40];
    const int n = buildRows(ctx, rows);
    for (int i = 0; i < kVisibleRows && scroll_ + i < n; ++i) {
        const Row& r = rows[scroll_ + i];
        const int y = kRowsY + i * kRowH;
        char name[24], value[24];
        int frac;
        valueText(r, ctx, value, sizeof(value), &frac);
        switch (r.type) {
        case RowKind: snprintf(name, sizeof(name), "INSTRUMENT"); break;
        case RowRoute: snprintf(name, sizeof(name), "MIXER TRACK"); break;
        case RowPreset: snprintf(name, sizeof(name), "PRESET"); break;
        case RowSynth: snprintf(name, sizeof(name), "%s", instr::synthParam(r.index).name); break;
        default: snprintf(name, sizeof(name), "ENV %s", instr::envParam(r.index).name); break;
        }
        ui::paramRow(g, kRowX, y, kRowW, name, value, frac < 0 ? 0 : frac, scroll_ + i == sel_, false);
    }
    ui::scrollbar(g, kRowX + kRowW + 2, kRowsY, 6, kVisibleRows * kRowH, scroll_, kVisibleRows, n);

    // Right: envelope picture and what the selected row does.
    g.text(kRightX, kRowsY, synth ? "AMP ENVELOPE" : (c.inst.env[kEnvEnabled] ? "SAMPLE ENVELOPE" : "ENVELOPE OFF (ONE-SHOT)"), theme::kTextDim, 1, 2);
    ui::envelopeGraph(g, kRightX, kRowsY + 18, theme::kSafeRight - kRightX - 8, 92, c.inst.env, synth || c.inst.env[kEnvEnabled]);
    char line[64];
    const int a = c.inst.env[kEnvAttack], h = c.inst.env[kEnvHold], d = c.inst.env[kEnvDecay], r = c.inst.env[kEnvRelease];
    snprintf(line, sizeof(line), "A%d H%d D%d S%d R%d", a, h, d, c.inst.env[kEnvSustain], r);
    g.text(kRightX, kRowsY + 116, line, theme::kText, 1, 2);
    const Row& cur = rows[sel_ < n ? sel_ : 0];
    const char* help = "";
    switch (cur.type) {
    case RowKind: help = "Cross/Left/Right: sampler or synth"; break;
    case RowRoute: help = "Which mixer insert this plays into"; break;
    case RowPreset: help = "Left/Right: load a synth preset"; break;
    case RowSynth: help = "Tuning applies to the next note;\nthe rest changes live"; break;
    case RowEnv: help = c.inst.env[kEnvEnabled] || synth ? "Release starts when the note ends" : "ENVELOPE ON shapes the sample"; break;
    }
    int ly = kRowsY + 134;
    for (const char* s = help; *s;) {
        const char* e = strchr(s, '\n');
        char buf[48];
        const size_t len = e ? (size_t)(e - s) : strlen(s);
        snprintf(buf, sizeof(buf), "%.*s", (int)(len > 40 ? 40 : len), s);
        g.text(kRightX, ly, buf, theme::kTextDim, 1, 2);
        ly += 14;
        s += len + (e ? 1 : 0);
    }
    char nn[12];
    PianoRollView::noteName(auditionPitch_, nn, sizeof(nn));
    g.textf(kRightX, kViewBottom - 40, theme::kTextDim, G_SQUARE " HEAR %s", nn);
    if (synth && c.voiceMode == (uint8_t)VoiceMode::Spu2)
        g.text(kRightX, kViewBottom - 24, "Synths always run in software", theme::kWarning, 1, 2);
}
