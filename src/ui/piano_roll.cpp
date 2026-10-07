#include "ui/piano_roll.hpp"

#include <stdio.h>

#include "audio/pitch.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kCellW = 32;
constexpr int kRowH = 20;
constexpr int kKeyW = 50;
constexpr int kGridX = theme::kSafeLeft + 6 + kKeyW;
constexpr int kGridY = kViewTop + 28;
const int kVelocities[] = {40, 70, 100, 127};

bool isBlackKey(int pitch)
{
    const int n = pitch % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}
} // namespace

const char* PianoRollView::noteName(int pitch, char* buf, size_t cap)
{
    static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    pitch = pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch);
    snprintf(buf, cap, "%s%d", kNames[pitch % 12], pitch / 12 - 1); // "C#-1" .. "G9"
    return buf;
}

const char* PianoRollView::hint() const
{
    return G_CROSS " NOTE " G_SQUARE " HEAR " G_CIRCLE " NEXT CH " G_TRIANGLE " MENU  L1/R1 LENGTH  L2/R2 PATTERN";
}

void PianoRollView::onEnter(UiContext& ctx)
{
    clampView(ctx);
}

void PianoRollView::clampView(const UiContext& ctx)
{
    const int len = ctx.session.project().pattern().length;
    if (step_ >= len)
        step_ = len - 1;
    if (step_ < 0)
        step_ = 0;
    if (pitch_ < 0)
        pitch_ = 0;
    if (pitch_ > 127)
        pitch_ = 127;
    if (step_ < scrollStep_)
        scrollStep_ = (step_ / kVisibleSteps) * kVisibleSteps;
    if (step_ >= scrollStep_ + kVisibleSteps)
        scrollStep_ = (step_ / kVisibleSteps) * kVisibleSteps;
    if (pitch_ > topPitch_)
        topPitch_ = pitch_;
    if (pitch_ <= topPitch_ - kVisibleRows)
        topPitch_ = pitch_ + kVisibleRows - 1;
    if (topPitch_ > 127)
        topPitch_ = 127;
    if (topPitch_ < kVisibleRows - 1)
        topPitch_ = kVisibleRows - 1;
}

void PianoRollView::openMenu(UiContext& ctx)
{
    const ChannelData& c = ctx.session.project().channels[ctx.selectedChannel];
    char buf[40];
    ctx.menu.open("PIANO ROLL");
    ctx.menu.add(MenuGate, c.gate ? "Instrument: SUSTAINED (length cuts)" : "Instrument: ONE-SHOT (plays through)");
    snprintf(buf, sizeof(buf), "New note velocity: %d", brushVel_);
    ctx.menu.add(MenuVelocity, buf);
    ctx.menu.add(MenuOctaveUp, "Shift all notes +1 octave");
    ctx.menu.add(MenuOctaveDown, "Shift all notes -1 octave");
    ctx.menu.add(MenuSemiUp, "Shift all notes +1 semitone");
    ctx.menu.add(MenuSemiDown, "Shift all notes -1 semitone");
    ctx.menu.add(MenuClear, "Clear notes of this channel...", ctx.session.project().pattern().noteCount[ctx.selectedChannel] > 0);
}

void PianoRollView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel, pat = s.project().currentPattern;
    switch (id) {
    case MenuGate:
        s.setChannelGate(ch, !s.project().channels[ch].gate);
        ctx.toast(s.project().channels[ch].gate ? "Sustained: note length cuts the sample" : "One-shot: samples play to the end");
        break;
    case MenuVelocity: {
        int next = kVelocities[0];
        for (int i = 0; i < 4; ++i)
            if (kVelocities[i] == brushVel_)
                next = kVelocities[(i + 1) % 4];
        brushVel_ = next;
        ctx.toast("New notes: velocity %d", brushVel_);
        break;
    }
    case MenuOctaveUp:
    case MenuOctaveDown:
    case MenuSemiUp:
    case MenuSemiDown: {
        const int d = id == MenuOctaveUp ? 12 : (id == MenuOctaveDown ? -12 : (id == MenuSemiUp ? 1 : -1));
        if (!s.transposeNotes(pat, ch, d))
            ctx.toast("Some notes would leave the 0-127 range");
        break;
    }
    case MenuClear:
        ctx.menu.open("CLEAR THESE NOTES?");
        ctx.menu.add(MenuClearConfirm, "Yes, clear the notes");
        break;
    case MenuClearConfirm:
        s.clearNotes(pat, ch);
        ctx.toast("Notes cleared");
        break;
    }
}

void PianoRollView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel;
    const int pat = s.project().currentPattern;
    const int len = s.project().pattern().length;

    if (in.rep(btn::Left) && step_ > 0)
        --step_;
    if (in.rep(btn::Right) && step_ + 1 < len)
        ++step_;
    if (in.rep(btn::Up) && pitch_ < 127)
        ++pitch_;
    if (in.rep(btn::Down) && pitch_ > 0)
        --pitch_;
    clampView(ctx);

    const int idx = s.noteIndexAt(pat, ch, step_, pitch_);
    if (in.rep(btn::L1) || in.rep(btn::R1)) {
        const int d = in.rep(btn::R1) ? 1 : -1;
        if (idx >= 0)
            s.setNoteLength(pat, ch, idx, s.project().pattern().notes[ch][idx].length + d);
        else
            brushLen_ = brushLen_ + d < 1 ? 1 : (brushLen_ + d > 16 ? 16 : brushLen_ + d);
    }
    if (in.hit(btn::L2) || in.hit(btn::R2)) {
        const int np = (pat + (in.hit(btn::R2) ? 1 : cfg::kMaxPatterns - 1)) % cfg::kMaxPatterns;
        s.selectPattern(np);
        clampView(ctx);
        ctx.toast("Pattern %d: %s", np + 1, s.project().patterns[np].name);
    }
    if (in.hit(btn::Cross)) {
        if (idx >= 0) {
            s.removeNote(pat, ch, idx);
        } else if (pitch_ == pitch::kRootNote && s.project().pattern().velocity[ch][step_]) {
            s.setStep(pat, ch, step_, 0); // the rack's root-pitch hit at this step
        } else if (s.addNote(pat, ch, step_, pitch_, brushLen_, brushVel_)) {
            s.previewChannel(ch, pitch_ - pitch::kRootNote);
        } else {
            ctx.toast("Channel holds %d notes already", cfg::kMaxNotes);
        }
    }
    if (in.hit(btn::Square))
        s.previewChannel(ch, pitch_ - pitch::kRootNote);
    if (in.hit(btn::Circle)) {
        ctx.selectedChannel = (ch + 1) % s.project().channelCount;
        ctx.toast("Channel %d: %s", ctx.selectedChannel + 1, s.project().channels[ctx.selectedChannel].name);
    }
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

void PianoRollView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int ch = ctx.selectedChannel;
    const ChannelData& c = p.channels[ch];
    const PatternData& pat = p.pattern();
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, nullptr);
    g.fillRect(x, kViewTop, w, 22, theme::kPanelHeader);
    g.textf(x + 8, kViewTop + 3, theme::kText, "ROLL  CH%d %.7s", ch + 1, c.name);
    g.textf(x + 270, kViewTop + 3, theme::kTextDim, "P%d %.8s", p.currentPattern + 1, pat.name);
    g.text(x + 430, kViewTop + 3, c.gate ? "SUSTAIN" : "ONE-SHOT", c.gate ? theme::kAlive : theme::kTextDim);

    const int len = pat.length;
    // Rows.
    for (int r = 0; r < kVisibleRows; ++r) {
        const int pitchRow = topPitch_ - r;
        if (pitchRow < 0)
            break;
        const int y = kGridY + r * kRowH;
        const bool black = isBlackKey(pitchRow);
        const bool root = pitchRow == pitch::kRootNote;
        g.fillRect(x + 6, y, kKeyW - 2, kRowH - 1, black ? theme::kPanelDark : 0xdadce0);
        char nm[12];
        noteName(pitchRow, nm, sizeof(nm));
        g.text(x + 10, y + 2, root ? "ROOT" : nm, black ? theme::kTextDim : theme::kTextDark, 1, 2);
        for (int i = 0; i < kVisibleSteps; ++i) {
            const int step = scrollStep_ + i;
            const bool beatAlt = (step / 4) & 1;
            uint32_t col = black ? 0x25282e : theme::kCellOff;
            if (beatAlt)
                col = black ? 0x1f2227 : theme::kCellOffAlt;
            if (step >= len)
                col = theme::kPanelDark;
            g.fillRect(kGridX + i * kCellW, y, kCellW - 1, kRowH - 1, col);
        }
        if (root)
            g.frameRect(kGridX, y, kVisibleSteps * kCellW - 1, kRowH - 1, theme::kTextDim, 1);
    }
    // Root-pitch hits placed in the rack's step grid show on the ROOT row.
    for (int i = 0; i < kVisibleSteps; ++i) {
        const int step = scrollStep_ + i;
        const int r = topPitch_ - pitch::kRootNote;
        if (step < len && pat.velocity[ch][step] && r >= 0 && r < kVisibleRows)
            g.fillRect(kGridX + i * kCellW + 1, kGridY + r * kRowH + 3, kCellW - 3, kRowH - 7, theme::kCellOnDim);
    }
    // Notes.
    for (int n = 0; n < pat.noteCount[ch] && n < cfg::kMaxNotes; ++n) {
        const PianoNote& note = pat.notes[ch][n];
        const int r = topPitch_ - note.pitch;
        if (r < 0 || r >= kVisibleRows)
            continue;
        const int s0 = note.step, s1 = note.step + note.length;
        if (s1 <= scrollStep_ || s0 >= scrollStep_ + kVisibleSteps)
            continue;
        const int v0 = s0 < scrollStep_ ? scrollStep_ : s0;
        const int v1 = s1 > scrollStep_ + kVisibleSteps ? scrollStep_ + kVisibleSteps : s1;
        const uint32_t col = note.velocity >= 90 ? theme::kCellOn : theme::kCellOnDim;
        g.fillRect(kGridX + (v0 - scrollStep_) * kCellW, kGridY + r * kRowH + 1, (v1 - v0) * kCellW - 2, kRowH - 3, col);
        g.fillRect(kGridX + (v0 - scrollStep_) * kCellW, kGridY + r * kRowH + 1, 3, kRowH - 3, theme::kPlayhead);
    }
    // Playhead.
    const int heard = ctx.heardStep();
    if (heard >= scrollStep_ && heard < scrollStep_ + kVisibleSteps && ctx.engine.status().pattern == p.currentPattern)
        g.fillRect(kGridX + (heard - scrollStep_) * kCellW + kCellW / 2 - 1, kGridY, 2, kVisibleRows * kRowH, theme::kPlayhead);
    // Cursor.
    const int cr = topPitch_ - pitch_;
    if (cr >= 0 && cr < kVisibleRows)
        ui::selectOutline(g, kGridX + (step_ - scrollStep_) * kCellW, kGridY + cr * kRowH, kCellW - 1, kRowH - 1, theme::kSelect);

    // Details.
    const int iy = kGridY + kVisibleRows * kRowH + 6;
    char nm[12], line[128];
    noteName(pitch_, nm, sizeof(nm));
    const int idx = ctx.session.noteIndexAt(p.currentPattern, ch, step_, pitch_);
    if (idx >= 0) {
        const PianoNote& n = pat.notes[ch][idx];
        snprintf(line, sizeof(line), "Step %d  %s  length %d  velocity %d  (%+d semitones)", step_ + 1, nm, n.length, n.velocity,
                 n.pitch - pitch::kRootNote);
    } else {
        snprintf(line, sizeof(line), "Step %d  %s  empty. Cross places: length %d, velocity %d  (%+d semitones)", step_ + 1, nm,
                 brushLen_, brushVel_, pitch_ - pitch::kRootNote);
    }
    g.text(x + 8, iy, line, theme::kText, 1, 2);
    snprintf(line, sizeof(line), "%d/%d notes in this channel. Pattern length %d steps.%s", pat.noteCount[ch], cfg::kMaxNotes, len,
             c.voiceMode == (uint8_t)VoiceMode::Spu2 ? " SPU2 voice: pitch yes, lengths ignored." : "");
    g.text(x + 8, iy + 16, line, theme::kTextDim, 1, 2);
}
