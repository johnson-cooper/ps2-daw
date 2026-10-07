#include "ui/channel_rack.hpp"

#include "core/strutil.hpp"

#include <stdio.h>
#include <string.h>

#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {

// Layout (logical 640x448 canvas).
constexpr int kPanelX = theme::kSafeLeft;
constexpr int kPanelW = theme::kSafeRight - theme::kSafeLeft;
constexpr int kRowsY = kViewTop + 28;
constexpr int kRowH = 32;
constexpr int kLedX = 30;
constexpr int kMuteX = 42;
constexpr int kNameX = 66;
constexpr int kNameW = 92;
constexpr int kVolX = 166;
constexpr int kVolW = 36;
constexpr int kPanX = 208;
constexpr int kPanW = 30;
constexpr int kStepsX = 248;
constexpr int kCellW = 19;
constexpr int kCellPitch = 22;
constexpr int kGroupGap = 4;
constexpr int kCellH = 22;

int stepX(int visibleIndex)
{
    return kStepsX + visibleIndex * kCellPitch + (visibleIndex / 4) * kGroupGap;
}

const int kLengths[] = {8, 12, 16, 24, 32, 48, 64};

} // namespace

const char* ChannelRackView::hint() const
{
    return editing_ ? G_LEFT G_RIGHT " ADJUST  L1/R1 x10  " G_CROSS "/" G_CIRCLE " DONE"
                    : G_CROSS " STEP  " G_SQUARE " MUTE  " G_TRIANGLE " MENU  " G_CIRCLE " PREVIEW  L1/R1 BEAT  L2/R2 PATTERN";
}

void ChannelRackView::clampCursor(const UiContext& ctx)
{
    const int len = ctx.session.project().pattern().length;
    if (scroll_ >= len)
        scroll_ = ((len - 1) / kVisibleSteps) * kVisibleSteps;
    if (scroll_ < 0)
        scroll_ = 0;
    const int visible = (len - scroll_) < kVisibleSteps ? (len - scroll_) : kVisibleSteps;
    if (col_ >= ColFirstStep + visible)
        col_ = ColFirstStep + visible - 1;
    if (col_ < 0)
        col_ = 0;
}

void ChannelRackView::adjustValue(int delta, UiContext& ctx)
{
    const int ch = ctx.selectedChannel;
    const ChannelData& c = ctx.session.project().channels[ch];
    if (col_ == ColVolume)
        ctx.session.setVolume(ch, c.volume + delta);
    else if (col_ == ColPan)
        ctx.session.setPan(ch, c.pan + delta * 2);
}

void ChannelRackView::openChannelMenu(UiContext& ctx)
{
    const int ch = ctx.selectedChannel;
    const ChannelData& c = ctx.session.project().channels[ch];
    char title[40];
    snprintf(title, sizeof(title), "CHANNEL %d: %s", ch + 1, c.name);
    ctx.menu.open(title);
    menuMode_ = 0;
    char buf[40];
    ctx.menu.add(MenuPreview, "Preview sound");
    ctx.menu.add(MenuSample, "Choose sample...");
    const bool spu = c.voiceMode == (uint8_t)VoiceMode::Spu2;
    snprintf(buf, sizeof(buf), "Voice: %s", spu ? "SPU2 hardware" : "Software mix");
    ctx.menu.add(MenuVoiceMode, buf, ctx.audio.stats().spuSounds > 0 || spu);
    ctx.menu.add(MenuSolo, c.solo ? "Solo: ON" : "Solo: off");
    ctx.menu.add(MenuFill4, "Fill every 4 steps");
    ctx.menu.add(MenuFill2, "Fill every 2 steps");
    ctx.menu.add(MenuFill1, "Fill every step");
    ctx.menu.add(MenuClearChannel, "Clear channel steps");
    ctx.menu.add(MenuLength, "Pattern length...");
    ctx.menu.add(MenuPatternMenu, "Pattern tools...");
    ctx.menu.add(MenuStop, "Stop transport");
}

namespace {
const char* const kPatternNames[] = {"INTRO", "VERSE", "CHORUS", "BREAK", "DROP", "FILL", "BUILD", "OUTRO", "MAIN", "ALT"};
const char* switchModeName(SwitchMode m)
{
    return m == SwitchMode::NextBar ? "next bar" : (m == SwitchMode::NextBeat ? "next beat" : "immediate");
}
} // namespace

void ChannelRackView::openPatternMenu(UiContext& ctx)
{
    Session& s = ctx.session;
    const int p = s.project().currentPattern;
    char title[40], buf[40];
    snprintf(title, sizeof(title), "PATTERN %d: %s", p + 1, s.project().patterns[p].name);
    ctx.menu.open(title);
    menuMode_ = 3;
    ctx.menu.add(MenuDuplicate, "Duplicate to next empty");
    ctx.menu.add(MenuCopyTo, "Copy to pattern...");
    ctx.menu.add(MenuClearPattern, "Clear pattern");
    ctx.menu.add(MenuRename, "Name: next preset");
    snprintf(buf, sizeof(buf), "Switch while playing: %s", switchModeName(s.switchMode()));
    ctx.menu.add(MenuSwitchMode, buf);
}

void ChannelRackView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel;
    const ChannelData& c = s.project().channels[ch];

    if (id >= MenuSampleBase && id < MenuSampleBase + cfg::kMaxSamples) {
        s.setSample(ch, id - MenuSampleBase);
        s.previewChannel(ch);
        ctx.toast("Channel %d sample: %s", ch + 1, s.project().channels[ch].name);
        return;
    }
    if (id >= MenuCopyBase && id < MenuCopyBase + cfg::kMaxPatterns) {
        const int src = s.project().currentPattern, dst = id - MenuCopyBase;
        if (dst != src && !s.patternIsEmpty(dst) && !(confirmOverwrite_ && overwriteTarget_ == dst)) {
            // Destructive: ask once; picking the same target again confirms.
            confirmOverwrite_ = true;
            overwriteTarget_ = dst;
            ctx.menu.open("OVERWRITE PATTERN?");
            menuMode_ = 4;
            for (int i = 0; i < cfg::kMaxPatterns; ++i)
                if (i == dst)
                    ctx.menu.add(MenuCopyBase + i, "Yes, overwrite");
            return;
        }
        confirmOverwrite_ = false;
        s.copyPattern(src, dst);
        ctx.toast("Pattern %d copied to %d", src + 1, dst + 1);
        return;
    }
    if (id >= MenuLengthBase && id < MenuLengthBase + 100) {
        const int len = id - MenuLengthBase;
        s.setPatternLength(s.project().currentPattern, len);
        clampCursor(ctx);
        ctx.toast("Pattern length: %d steps", len);
        return;
    }

    switch (id) {
    case MenuPreview:
        s.previewChannel(ch);
        break;
    case MenuSample: {
        ctx.menu.open("CHOOSE SAMPLE");
        menuMode_ = 1;
        const SampleBank& bank = ctx.bank;
        int added = 0;
        for (int i = 0; i < bank.count() && added < ui::ContextMenu::kMaxItems; ++i) {
            const Sample* smp = bank.get(i);
            if (!smp)
                continue;
            char buf[40];
            if (smp->hwOnly)
                snprintf(buf, sizeof(buf), "%-10.10s  SPU2%s", smp->name, c.sampleSlot == i ? "  *" : "");
            else
                snprintf(buf, sizeof(buf), "%-10.10s %5lu ms%s", smp->name,
                         (unsigned long)((uint64_t)smp->frames * 1000u / smp->sampleRate), c.sampleSlot == i ? "  *" : "");
            ctx.menu.add(MenuSampleBase + i, buf);
            ++added;
        }
        if (bank.liveCount() > added)
            ctx.toast("More samples in BROWSER (menu shows %d)", added);
        break;
    }
    case MenuVoiceMode: {
        const bool toSpu = c.voiceMode != (uint8_t)VoiceMode::Spu2;
        s.setVoiceMode(ch, toSpu ? VoiceMode::Spu2 : VoiceMode::Software);
        ctx.toast("Channel %d now uses %s", ch + 1, toSpu ? "SPU2 hardware voice" : "software mixer");
        break;
    }
    case MenuSolo:
        s.setSolo(ch, !c.solo);
        break;
    case MenuFill4:
        s.fillChannelEvery(ch, 4);
        break;
    case MenuFill2:
        s.fillChannelEvery(ch, 2);
        break;
    case MenuFill1:
        s.fillChannelEvery(ch, 1);
        break;
    case MenuClearChannel:
        s.clearChannelSteps(ch);
        break;
    case MenuClearPattern:
        if (s.patternIsEmpty(s.project().currentPattern))
            break;
        ctx.menu.open("CLEAR THIS PATTERN?");
        menuMode_ = 4;
        ctx.menu.add(MenuClearConfirm, "Yes, clear all notes");
        break;
    case MenuClearConfirm:
        s.clearPattern(s.project().currentPattern);
        ctx.toast("Pattern %d cleared", s.project().currentPattern + 1);
        break;
    case MenuLength: {
        ctx.menu.open("PATTERN LENGTH");
        menuMode_ = 2;
        for (int len : kLengths) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%2d steps%s", len, s.project().pattern().length == len ? "  *" : "");
            ctx.menu.add(MenuLengthBase + len, buf);
        }
        break;
    }
    case MenuPatternMenu:
        openPatternMenu(ctx);
        break;
    case MenuDuplicate: {
        const int n = s.duplicatePattern(s.project().currentPattern);
        if (n < 0)
            ctx.toast("No empty pattern left to duplicate into");
        else
            ctx.toast("Duplicated into pattern %d", n + 1);
        clampCursor(ctx);
        break;
    }
    case MenuCopyTo: {
        ctx.menu.open("COPY TO PATTERN");
        menuMode_ = 4;
        confirmOverwrite_ = false;
        for (int i = 0; i < cfg::kMaxPatterns; ++i) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%d  %.12s%s", i + 1, s.project().patterns[i].name,
                     i == s.project().currentPattern ? "  (this)" : (s.patternIsEmpty(i) ? "" : "  *"));
            ctx.menu.add(MenuCopyBase + i, buf, i != s.project().currentPattern);
        }
        break;
    }
    case MenuRename: {
        const int p = s.project().currentPattern;
        int next = 0;
        for (int i = 0; i < (int)(sizeof(kPatternNames) / sizeof(kPatternNames[0])); ++i)
            if (str::equalsNoCase(s.project().patterns[p].name, kPatternNames[i]))
                next = (i + 1) % (int)(sizeof(kPatternNames) / sizeof(kPatternNames[0]));
        s.setPatternName(p, kPatternNames[next]);
        ctx.toast("Pattern %d: %s", p + 1, kPatternNames[next]);
        break;
    }
    case MenuSwitchMode: {
        const SwitchMode m = s.switchMode() == SwitchMode::Immediate ? SwitchMode::NextBeat
                             : s.switchMode() == SwitchMode::NextBeat ? SwitchMode::NextBar
                                                                      : SwitchMode::Immediate;
        s.setSwitchMode(m);
        ctx.toast("Pattern switch: %s", switchModeName(m));
        break;
    }
    case MenuStop:
        s.stop();
        break;
    default:
        break;
    }
}

void ChannelRackView::update(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;

    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }

    const int channels = s.project().channelCount;
    int& row = ctx.selectedChannel;
    const int len = s.project().pattern().length;

    if (editing_) {
        if (in.rep(btn::Left) || in.rep(btn::Down))
            adjustValue(-1, ctx);
        if (in.rep(btn::Right) || in.rep(btn::Up))
            adjustValue(+1, ctx);
        if (in.rep(btn::L1))
            adjustValue(-10, ctx);
        if (in.rep(btn::R1))
            adjustValue(+10, ctx);
        if (in.hit(btn::Cross) || in.hit(btn::Circle))
            editing_ = false;
        return;
    }

    if (in.rep(btn::Up))
        row = row > 0 ? row - 1 : 0;
    if (in.rep(btn::Down))
        row = row < channels - 1 ? row + 1 : channels - 1;

    if (in.rep(btn::Left)) {
        if (col_ == ColFirstStep && scroll_ > 0) {
            scroll_ -= kVisibleSteps;
            col_ = ColFirstStep + kVisibleSteps - 1;
        } else if (col_ > 0) {
            --col_;
        }
    }
    if (in.rep(btn::Right)) {
        const int step = stepAtColumn();
        if (col_ >= ColFirstStep && step + 1 < len && col_ == ColFirstStep + kVisibleSteps - 1) {
            scroll_ += kVisibleSteps;
            col_ = ColFirstStep;
        } else if (col_ < ColFirstStep || step + 1 < len) {
            ++col_;
        }
    }
    // L1/R1: jump one beat (4 steps) through the grid.
    if (in.rep(btn::L1) || in.rep(btn::R1)) {
        int step = col_ >= ColFirstStep ? stepAtColumn() : 0;
        step += in.rep(btn::R1) ? 4 : -4;
        step = step < 0 ? 0 : (step >= len ? len - 1 : step);
        scroll_ = (step / kVisibleSteps) * kVisibleSteps;
        col_ = ColFirstStep + step - scroll_;
    }
    // L2/R2: previous / next pattern.
    if (in.hit(btn::L2) || in.hit(btn::R2)) {
        int p = s.project().currentPattern + (in.hit(btn::R2) ? 1 : -1);
        p = (p + cfg::kMaxPatterns) % cfg::kMaxPatterns;
        s.selectPattern(p);
        clampCursor(ctx);
        ctx.toast("Pattern %d", p + 1);
    }

    if (in.hit(btn::Cross)) {
        switch (col_) {
        case ColMute:
            s.setMute(row, !s.project().channels[row].mute);
            break;
        case ColName:
            s.previewChannel(row);
            break;
        case ColVolume:
        case ColPan:
            editing_ = true;
            break;
        default:
            s.toggleStep(row, stepAtColumn());
            break;
        }
    }
    if (in.hit(btn::Square))
        s.setMute(row, !s.project().channels[row].mute);
    if (in.hit(btn::Circle))
        s.previewChannel(row);
    if (in.hit(btn::Triangle))
        openChannelMenu(ctx);

    // Right stick: continuous volume (vertical) / pan (horizontal) for the
    // selected channel, rate-limited so it is controllable.
    static uint32_t lastStickMs = 0;
    if ((in.rx || in.ry) && ctx.nowMs - lastStickMs >= 50) {
        lastStickMs = ctx.nowMs;
        const ChannelData& c = s.project().channels[row];
        if (in.ry)
            s.setVolume(row, c.volume - in.ry / 48);
        if (in.rx)
            s.setPan(row, c.pan + in.rx / 24);
    }

    clampCursor(ctx);
}

void ChannelRackView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const PatternData& pat = p.pattern();
    const int heard = ctx.heardStep();
    const int playingPattern = ctx.engine.status().pattern;

    char title[64];
    const int lastVisible = (scroll_ + kVisibleSteps < pat.length ? scroll_ + kVisibleSteps : pat.length);
    snprintf(title, sizeof(title), "CHANNEL RACK   %s   STEPS %d-%d/%d", pat.name, scroll_ + 1, lastVisible, pat.length);
    const int panelH = 28 + p.channelCount * kRowH + 2;
    ui::panel(g, kPanelX, kViewTop, kPanelW, panelH, title);

    for (int ch = 0; ch < p.channelCount; ++ch) {
        const ChannelData& c = p.channels[ch];
        const int y = kRowsY + ch * kRowH;
        const bool rowSel = ch == ctx.selectedChannel;
        if (rowSel)
            g.fillRect(kPanelX + 2, y - 2, kPanelW - 4, kRowH - 4, theme::kPanelHeader);

        // Activity LED lights when the trigger is actually heard.
        ui::led(g, kLedX, y + 8, 8, ctx.channelActive(ch), c.voiceMode == (uint8_t)VoiceMode::Spu2 ? theme::kHardware : theme::kAlive);

        // Mute light: green = playing, dark = muted (red ring), gold = solo.
        const uint32_t muteColor = c.solo ? theme::kSolo : theme::kAlive;
        g.fillRect(kMuteX, y + 3, 16, 16, c.mute ? theme::kPanelDark : muteColor);
        if (c.mute)
            g.frameRect(kMuteX, y + 3, 16, 16, theme::kMute, 2);
        if (rowSel && col_ == ColMute)
            ui::selectOutline(g, kMuteX, y + 3, 16, 16, theme::kSelect);

        g.fillRect(kNameX, y + 1, kNameW, 20, rowSel ? theme::kAccent : theme::kCellOffAlt);
        char name[8];
        snprintf(name, sizeof(name), "%.7s", c.name);
        g.text(kNameX + 4, y + 3, name, rowSel ? theme::kTextDark : theme::kText);
        if (c.voiceMode == (uint8_t)VoiceMode::Spu2)
            g.fillRect(kNameX + kNameW - 5, y + 1, 5, 20, theme::kHardware);
        if (rowSel && col_ == ColName)
            ui::selectOutline(g, kNameX, y + 1, kNameW, 20, theme::kSelect);

        ui::hslider(g, kVolX, y + 7, kVolW, 8, c.volume, 100, false, rowSel && col_ == ColVolume, editing_ && rowSel && col_ == ColVolume);
        ui::hslider(g, kPanX, y + 7, kPanW, 8, c.pan, 100, true, rowSel && col_ == ColPan, editing_ && rowSel && col_ == ColPan);

        for (int i = 0; i < kVisibleSteps; ++i) {
            const int step = scroll_ + i;
            if (step >= pat.length)
                break;
            const bool playhead = heard == step && playingPattern == p.currentPattern;
            const bool sel = rowSel && col_ == ColFirstStep + i;
            ui::stepCell(g, stepX(i), y, kCellW, kCellH, pat.velocity[ch][step], ((step / 4) & 1) != 0, playhead, sel);
        }
    }

    // Step ruler under the grid: beat numbers.
    const int rulerY = kRowsY + p.channelCount * kRowH + 4;
    for (int i = 0; i < kVisibleSteps && scroll_ + i < pat.length; i += 4) {
        char n[12];
        snprintf(n, sizeof(n), "%d", (scroll_ + i) / 4 + 1);
        g.text(stepX(i), rulerY, n, theme::kTextDim, 1, 2);
    }

    // Selected channel detail line.
    const ChannelData& c = p.channels[ctx.selectedChannel];
    const int infoY = kViewBottom - 18;
    g.fillRect(kPanelX, infoY - 3, kPanelW, 21, theme::kPanelDark);
    char pan[8];
    if (c.pan == 0)
        snprintf(pan, sizeof(pan), "C");
    else
        snprintf(pan, sizeof(pan), "%c%d", c.pan < 0 ? 'L' : 'R', c.pan < 0 ? -c.pan : c.pan);
    g.textf(kPanelX + 6, infoY, theme::kTextDim, "CH%d %-7s VOL %3d  PAN %-4s %s%s%s", ctx.selectedChannel + 1, c.name, c.volume, pan,
            c.voiceMode == (uint8_t)VoiceMode::Spu2 ? "SPU2" : "SW",
            c.mute ? " MUTED" : "", c.solo ? " SOLO" : "");
}
