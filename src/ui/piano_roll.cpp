#include "ui/piano_roll.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/pitch.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kCellW = 32;
constexpr int kRowH = 20;
constexpr int kKeyW = 50;
constexpr int kGridX = theme::kSafeLeft + 6 + kKeyW;
constexpr int kGridY = kViewTop + 28;
constexpr int kTicks = noteedit::kTicks;
constexpr int kMinTicks = 3; // shortest note the roll draws and places (1/8 of a step)

// Menu lists (ticks): 24 ticks make one 16th step.
const int kLengthTicks[] = {3, 4, 6, 8, 12, 24, 48, 72, 96, 144, 192, 288, 384};
const int kSnapTicks[] = {24, 12, 8, 6, 4, 3, 48, 96};
const int kChopParts[] = {2, 3, 4, 6, 8};

bool isBlackKey(int pitch)
{
    const int n = pitch % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

inline uint64_t bit(int i) { return (uint64_t)1 << i; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

const char* PianoRollView::noteName(int pitch, char* buf, size_t cap)
{
    static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    pitch = pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch);
    snprintf(buf, cap, "%s%d", kNames[pitch % 12], pitch / 12 - 1); // "C#-1" .. "G9"
    return buf;
}

const char* PianoRollView::tickText(int ticks, char* buf, size_t cap)
{
    if (ticks % kTicks == 0) {
        const int n = ticks / kTicks;
        snprintf(buf, cap, "%d step%s", n, n == 1 ? "" : "s");
    } else if (kTicks % ticks == 0) {
        snprintf(buf, cap, "1/%d step", kTicks / ticks); // 12 ticks = 1/2 step, 8 = 1/3, 6 = 1/4 ...
    } else {
        snprintf(buf, cap, "%d/%d step", ticks, kTicks);
    }
    return buf;
}

const char* PianoRollView::hint() const
{
    switch (mode_) {
    case Mode::Select:
        return G_CROSS " SELECT " G_SQUARE " GRAB " G_CIRCLE " CLEAR/EXIT " G_TRIANGLE " MENU>CHOP  L1/R1 LENGTH  L2" G_CROSS " UNDO";
    case Mode::Grab:
        return G_UP G_DOWN G_LEFT G_RIGHT " MOVE  R2+" G_UP G_DOWN " OCTAVE  " G_CROSS " DROP  " G_CIRCLE " CANCEL";
    default:
        return G_CROSS " NOTE " G_SQUARE " HEAR " G_CIRCLE " NEXT CH " G_TRIANGLE " MENU  L1/R1 LEN  L2/R2 PATTERN  L2" G_CROSS " UNDO";
    }
}

void PianoRollView::onEnter(UiContext& ctx)
{
    refresh(ctx);
    clampView(ctx);
}

void PianoRollView::setPos(int ticks)
{
    ticks = ticks < 0 ? 0 : ticks;
    step_ = ticks / kTicks;
    tick_ = ticks % kTicks;
}

void PianoRollView::clampView(const UiContext& ctx)
{
    const int len = ctx.session.project().pattern().length;
    if (step_ >= len) {
        step_ = len - 1;
        tick_ = 0;
    }
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

// ---------------------------------------------------------------------------
// working set, undo
// ---------------------------------------------------------------------------

static void capture(const Project& p, int pattern, int channel, uint8_t* count, PianoNote* notes)
{
    const PatternData& pd = p.patterns[pattern];
    const int n = pd.noteCount[channel] > cfg::kMaxNotes ? cfg::kMaxNotes : pd.noteCount[channel];
    *count = (uint8_t)n;
    memset(notes, 0, sizeof(PianoNote) * cfg::kMaxNotes);
    for (int i = 0; i < n; ++i)
        notes[i] = pd.notes[channel][i];
}

void PianoRollView::refresh(UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int pat = p.currentPattern, ch = ctx.selectedChannel;
    if (pat != selPattern_ || ch != selChannel_) {
        // History and selection belong to one pattern + channel.
        selPattern_ = pat;
        selChannel_ = ch;
        sel_ = 0;
        undoCount_ = redoCount_ = 0;
        if (mode_ != Mode::Draw)
            mode_ = Mode::Draw;
    }
    noteedit::load(set_, p.patterns[pat], ch);
    const uint64_t valid = set_.count >= 64 ? ~(uint64_t)0 : (bit(set_.count) - 1);
    set_.sel = sel_ & valid;
}

void PianoRollView::beginEdit(UiContext& ctx)
{
    const Project& p = ctx.session.project();
    if (undoCount_ == kUndoDepth) {
        memmove(&undo_[0], &undo_[1], sizeof(Snapshot) * (kUndoDepth - 1));
        --undoCount_;
    }
    Snapshot& s = undo_[undoCount_++];
    s.pattern = (uint8_t)p.currentPattern;
    s.channel = (uint8_t)ctx.selectedChannel;
    capture(p, s.pattern, s.channel, &s.count, s.notes);
    redoCount_ = 0;
}

void PianoRollView::commit(UiContext& ctx)
{
    ctx.session.setNotes(selPattern_, selChannel_, set_.notes, set_.count);
    sel_ = set_.sel;
}

void PianoRollView::undo(UiContext& ctx, bool redo)
{
    const Project& p = ctx.session.project();
    Snapshot* from = redo ? redo_ : undo_;
    int& fromCount = redo ? redoCount_ : undoCount_;
    Snapshot* to = redo ? undo_ : redo_;
    int& toCount = redo ? undoCount_ : redoCount_;
    if (fromCount == 0) {
        ctx.toast(redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    Snapshot now;
    now.pattern = (uint8_t)selPattern_;
    now.channel = (uint8_t)selChannel_;
    capture(p, selPattern_, selChannel_, &now.count, now.notes);
    const Snapshot s = from[--fromCount];
    if (toCount == kUndoDepth) {
        memmove(&to[0], &to[1], sizeof(Snapshot) * (kUndoDepth - 1));
        --toCount;
    }
    to[toCount++] = now;
    ctx.session.setNotes(s.pattern, s.channel, s.notes, s.count);
    sel_ = 0;
    ctx.toast("%s (%d left)", redo ? "Redo" : "Undo", fromCount);
}

void PianoRollView::moveCursorH(int dir, int lenSteps)
{
    const int p = pos();
    int np;
    if (dir > 0)
        np = (p / snapTicks_ + 1) * snapTicks_;
    else
        np = (p % snapTicks_) ? (p / snapTicks_) * snapTicks_ : p - snapTicks_;
    np = clampi(np, 0, lenSteps * kTicks - 1);
    setPos(np);
}

// ---------------------------------------------------------------------------
// menu
// ---------------------------------------------------------------------------

void PianoRollView::openMenu(UiContext& ctx)
{
    const ChannelData& c = ctx.session.project().channels[ctx.selectedChannel];
    const bool sel = noteedit::selectedCount(set_) > 0;
    const bool onNote = noteedit::indexCovering(set_, pos(), pitch_) >= 0;
    char buf[64], t[32];
    ctx.menu.open(mode_ == Mode::Draw ? "PIANO ROLL" : "PIANO ROLL: SELECTION");
    ctx.menu.add(MenuMode, mode_ == Mode::Draw ? "Mode: switch to SELECT notes" : "Mode: back to DRAW");
    snprintf(buf, sizeof(buf), "Undo (%d)", undoCount_);
    ctx.menu.add(MenuUndo, buf, undoCount_ > 0);
    snprintf(buf, sizeof(buf), "Redo (%d)", redoCount_);
    ctx.menu.add(MenuRedo, buf, redoCount_ > 0);
    ctx.menu.add(MenuChop, sel ? "Chop selection into parts..." : "Chop note under cursor...", sel || onNote);
    if (mode_ != Mode::Draw) {
        ctx.menu.add(MenuCopy, sel ? "Copy selection" : "Copy all notes", set_.count > 0);
        ctx.menu.add(MenuCut, "Cut selection", sel);
        ctx.menu.add(MenuPaste, "Paste at cursor", clip_.count > 0);
        ctx.menu.add(MenuDuplicate, "Duplicate right after", set_.count > 0);
        ctx.menu.add(MenuDelete, "Delete selection", sel);
        ctx.menu.add(MenuSelectAll, "Select all");
        ctx.menu.add(MenuSelectColumn, "Select notes in this step", set_.count > 0);
        ctx.menu.add(MenuSelectRow, "Select notes on this pitch", set_.count > 0);
        ctx.menu.add(MenuSelectFrom, "Select from here on", set_.count > 0);
    } else {
        ctx.menu.add(MenuPaste, "Paste at cursor", clip_.count > 0);
    }
    if (sel)
        snprintf(buf, sizeof(buf), "Length of selection...");
    else
        snprintf(buf, sizeof(buf), "Length: %s...", tickText(brushTicks_, t, sizeof(t)));
    ctx.menu.add(MenuLength, buf);
    snprintf(buf, sizeof(buf), sel ? "Velocity of selection..." : "Velocity: %d...", brushVel_);
    ctx.menu.add(MenuVelocity, buf);
    snprintf(buf, sizeof(buf), "Snap grid: %s...", tickText(snapTicks_, t, sizeof(t)));
    ctx.menu.add(MenuSnap, buf);
    ctx.menu.add(MenuQuantize, sel ? "Quantize selection to snap" : "Quantize all to snap", set_.count > 0);
    ctx.menu.add(MenuTranspose, sel ? "Transpose selection..." : "Transpose all notes...", set_.count > 0);
    if (mode_ == Mode::Draw) {
        ctx.menu.add(MenuGate, c.gate ? "Instrument: SUSTAINED (length cuts)" : "Instrument: ONE-SHOT (plays through)");
        ctx.menu.add(MenuClear, "Clear notes of this channel...", set_.count > 0);
    }
}

void PianoRollView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    const int ch = ctx.selectedChannel;
    const int lenSteps = s.project().pattern().length;
    const int lenTicks = lenSteps * kTicks;
    char t[32];
    if (id >= MenuSnapBase && id < MenuSnapBase + 1000) {
        snapTicks_ = id - MenuSnapBase;
        setPos((pos() / snapTicks_) * snapTicks_);
        ctx.toast("Snap: %s (cursor, quantize)", tickText(snapTicks_, t, sizeof(t)));
        return;
    }
    if (id >= MenuLengthBase && id < MenuLengthBase + 1000) {
        const int n = id - MenuLengthBase;
        brushTicks_ = n;
        if (set_.count) {
            beginEdit(ctx);
            if (noteedit::selectedCount(set_)) {
                noteedit::setLengthTicks(set_, n, false);
            } else {
                const int i = noteedit::indexCovering(set_, pos(), pitch_);
                if (i >= 0)
                    noteedit::setDuration(set_.notes[i], n);
            }
            commit(ctx);
        }
        ctx.toast("Note length: %s", tickText(n, t, sizeof(t)));
        return;
    }
    if (id >= MenuVelocityBase && id < MenuVelocityBase + 200) {
        const int v = id - MenuVelocityBase;
        brushVel_ = v;
        if (set_.count) {
            beginEdit(ctx);
            if (noteedit::selectedCount(set_)) {
                noteedit::setVelocity(set_, v, false);
            } else {
                const int i = noteedit::indexCovering(set_, pos(), pitch_);
                if (i >= 0)
                    set_.notes[i].velocity = (uint8_t)v;
            }
            commit(ctx);
        }
        ctx.toast("Velocity: %d", v);
        return;
    }
    if (id >= MenuTransposeBase && id < MenuTransposeBase + 100) {
        const int d = id - MenuTransposeBase - 24;
        beginEdit(ctx);
        if (!noteedit::transpose(set_, d, targetAll())) {
            --undoCount_; // nothing changed
            ctx.toast("Some notes would leave the 0-127 range");
        } else {
            commit(ctx);
        }
        return;
    }
    if (id >= MenuChopBase && id < MenuChopBase + 100) {
        const int parts = id - MenuChopBase;
        beginEdit(ctx);
        bool ok;
        if (noteedit::selectedCount(set_)) {
            ok = noteedit::chop(set_, parts, false);
        } else {
            const int i = noteedit::indexCovering(set_, pos(), pitch_);
            set_.sel = i >= 0 ? bit(i) : 0;
            ok = i >= 0 && noteedit::chop(set_, parts, false);
            if (!ok)
                set_.sel = 0;
        }
        if (ok) {
            commit(ctx);
            if (mode_ == Mode::Draw)
                mode_ = Mode::Select; // the pieces are selected: move, resize or velocity-shape them
            ctx.toast("Chopped into %d parts", parts);
        } else {
            --undoCount_;
            ctx.toast("Cannot chop: notes too short, or more than %d notes", cfg::kMaxNotes);
        }
        return;
    }
    switch (id) {
    case MenuMode:
        mode_ = mode_ == Mode::Draw ? Mode::Select : Mode::Draw;
        if (mode_ == Mode::Draw) {
            sel_ = 0;
            set_.sel = 0;
        }
        ctx.toast(mode_ == Mode::Select ? "SELECT: Cross picks notes, Square grabs them" : "DRAW mode");
        break;
    case MenuUndo:
        undo(ctx, false);
        break;
    case MenuRedo:
        undo(ctx, true);
        break;
    case MenuCopy: {
        const int n = noteedit::copy(set_, clip_);
        ctx.toast("Copied %d note%s", n, n == 1 ? "" : "s");
        break;
    }
    case MenuCut: {
        noteedit::copy(set_, clip_);
        beginEdit(ctx);
        const int n = noteedit::removeSelected(set_);
        commit(ctx);
        ctx.toast("Cut %d note%s", n, n == 1 ? "" : "s");
        break;
    }
    case MenuPaste:
        beginEdit(ctx);
        if (noteedit::paste(set_, clip_, pos(), pitch_, lenTicks)) {
            commit(ctx);
            if (mode_ == Mode::Draw)
                mode_ = Mode::Select; // the pasted notes are selected: ready to move or adjust
            ctx.toast("Pasted %d note%s", noteedit::selectedCount(set_), noteedit::selectedCount(set_) == 1 ? "" : "s");
        } else {
            --undoCount_;
            ctx.toast(clip_.count ? "No room: a channel holds %d notes" : "Nothing to paste", cfg::kMaxNotes);
        }
        break;
    case MenuDuplicate:
        beginEdit(ctx);
        if (noteedit::duplicate(set_, lenTicks)) {
            commit(ctx);
            ctx.toast("Duplicated");
        } else {
            --undoCount_;
            ctx.toast("No room for a copy there");
        }
        break;
    case MenuDelete:
        beginEdit(ctx);
        noteedit::removeSelected(set_);
        commit(ctx);
        break;
    case MenuSelectAll:
        noteedit::selectAll(set_);
        sel_ = set_.sel;
        break;
    case MenuSelectNone:
        noteedit::selectNone(set_);
        sel_ = 0;
        break;
    case MenuSelectColumn:
        noteedit::selectColumn(set_, step_);
        sel_ = set_.sel;
        break;
    case MenuSelectRow:
        noteedit::selectRow(set_, pitch_);
        sel_ = set_.sel;
        break;
    case MenuSelectFrom:
        noteedit::selectFrom(set_, pos());
        sel_ = set_.sel;
        break;
    case MenuLength:
        ctx.menu.open("NOTE LENGTH");
        for (int n : kLengthTicks) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s%s%s", tickText(n, t, sizeof(t)), n == 96 ? "  (1 beat)" : (n == 384 ? "  (1 bar)" : ""),
                     n == brushTicks_ ? "  *" : "");
            ctx.menu.add(MenuLengthBase + n, buf);
        }
        break;
    case MenuVelocity: {
        ctx.menu.open("VELOCITY");
        static const int kMenuVelocities[] = {20, 40, 60, 80, 100, 115, 127};
        for (int v : kMenuVelocities) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d", v);
            ctx.menu.add(MenuVelocityBase + v, buf);
        }
        break;
    }
    case MenuSnap:
        ctx.menu.open("SNAP GRID");
        for (int n : kSnapTicks) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s%s", tickText(n, t, sizeof(t)), n == snapTicks_ ? "  *" : "");
            ctx.menu.add(MenuSnapBase + n, buf);
        }
        break;
    case MenuChop:
        ctx.menu.open("CHOP INTO EQUAL PARTS");
        for (int parts : kChopParts) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%d parts", parts);
            ctx.menu.add(MenuChopBase + parts, buf);
        }
        break;
    case MenuQuantize:
        beginEdit(ctx);
        noteedit::quantize(set_, snapTicks_, lenTicks, targetAll());
        commit(ctx);
        ctx.toast("Quantized to %s", tickText(snapTicks_, t, sizeof(t)));
        break;
    case MenuTranspose:
        ctx.menu.open("TRANSPOSE");
        ctx.menu.add(MenuTransposeBase + 24 + 12, "+1 octave");
        ctx.menu.add(MenuTransposeBase + 24 + 1, "+1 semitone");
        ctx.menu.add(MenuTransposeBase + 24 - 1, "-1 semitone");
        ctx.menu.add(MenuTransposeBase + 24 - 12, "-1 octave");
        break;
    case MenuGate:
        s.setChannelGate(ch, !s.project().channels[ch].gate);
        ctx.toast(s.project().channels[ch].gate ? "Sustained: note length cuts the sample" : "One-shot: samples play to the end");
        break;
    case MenuClear:
        ctx.menu.open("CLEAR THESE NOTES?");
        ctx.menu.add(MenuClearConfirm, "Yes, clear the notes");
        break;
    case MenuClearConfirm:
        beginEdit(ctx);
        set_.count = 0;
        set_.sel = 0;
        commit(ctx);
        ctx.toast("Notes cleared (undo brings them back)");
        break;
    }
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void PianoRollView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }
    Session& s = ctx.session;
    refresh(ctx);
    const int ch = ctx.selectedChannel;
    const int pat = s.project().currentPattern;
    const int len = s.project().pattern().length;
    const int lenTicks = len * kTicks;
    // L1/R1 resize by the snap grid when it is finer than a step, else by whole steps (as always)
    const int lenStep = snapTicks_ < kTicks ? snapTicks_ : kTicks;

    // L2 / R2: alone they switch pattern (on release); with another button they are chords.
    if (in.hit(btn::L2)) {
        l2Down_ = true;
        l2Used_ = false;
    }
    if (in.hit(btn::R2)) {
        r2Down_ = true;
        r2Used_ = false;
    }
    if (l2Down_ && in.hit(btn::Cross)) {
        undo(ctx, false);
        l2Used_ = true;
        return;
    }
    if (l2Down_ && in.hit(btn::Circle)) {
        undo(ctx, true);
        l2Used_ = true;
        return;
    }
    if ((in.released & btn::L2) && l2Down_) {
        l2Down_ = false;
        if (!l2Used_ && mode_ != Mode::Grab) {
            const int np = (pat + cfg::kMaxPatterns - 1) % cfg::kMaxPatterns;
            s.selectPattern(np);
            clampView(ctx);
            ctx.toast("Pattern %d: %s", np + 1, s.project().patterns[np].name);
            return;
        }
    }
    if ((in.released & btn::R2) && r2Down_) {
        r2Down_ = false;
        if (!r2Used_ && mode_ != Mode::Grab) {
            const int np = (pat + 1) % cfg::kMaxPatterns;
            s.selectPattern(np);
            clampView(ctx);
            ctx.toast("Pattern %d: %s", np + 1, s.project().patterns[np].name);
            return;
        }
    }

    // ---- grab mode: the D-pad carries the selection ----
    if (mode_ == Mode::Grab) {
        int dT = 0, dP = 0;
        if (in.rep(btn::Left))
            dT = -snapTicks_;
        if (in.rep(btn::Right))
            dT = snapTicks_;
        if (in.rep(btn::Up))
            dP = r2Down_ ? 12 : 1;
        if (in.rep(btn::Down))
            dP = r2Down_ ? -12 : -1;
        if (r2Down_ && (in.rep(btn::Up) || in.rep(btn::Down)))
            r2Used_ = true;
        if (dT || dP) {
            if (noteedit::move(set_, dT, dP, lenTicks)) {
                commit(ctx);
                setPos(pos() + dT);
                pitch_ += dP;
                if (dP)
                    s.previewChannel(ch, pitch_ - pitch::kRootNote);
            }
        }
        if (in.hit(btn::Cross)) {
            mode_ = Mode::Select;
            ctx.toast("Dropped");
        }
        if (in.hit(btn::Circle)) {
            // restore what the grab started from
            const Snapshot& g0 = undo_[undoCount_ - 1];
            s.setNotes(g0.pattern, g0.channel, g0.notes, g0.count);
            --undoCount_;
            mode_ = Mode::Select;
            ctx.toast("Move cancelled");
        }
        clampView(ctx);
        return;
    }

    // ---- cursor ----
    const bool octave = r2Down_;
    if (in.rep(btn::Left)) {
        if (octave) {
            step_ = (step_ / kVisibleSteps - 1) * kVisibleSteps;
            tick_ = 0;
            r2Used_ = true;
        } else {
            moveCursorH(-1, len);
        }
    }
    if (in.rep(btn::Right)) {
        if (octave) {
            step_ = (step_ / kVisibleSteps + 1) * kVisibleSteps;
            tick_ = 0;
            r2Used_ = true;
        } else {
            moveCursorH(+1, len);
        }
    }
    if (in.rep(btn::Up)) {
        pitch_ += octave ? 12 : 1;
        r2Used_ |= octave;
    }
    if (in.rep(btn::Down)) {
        pitch_ -= octave ? 12 : 1;
        r2Used_ |= octave;
    }
    clampView(ctx);
    const int idx = noteedit::indexCovering(set_, pos(), pitch_);
    const int exact = noteedit::indexAt(set_, pos(), pitch_);

    // ---- length (L1/R1) ----
    if (in.rep(btn::L1) || in.rep(btn::R1)) {
        const int d = (in.rep(btn::R1) ? 1 : -1) * lenStep;
        if (mode_ == Mode::Select && noteedit::selectedCount(set_)) {
            beginEdit(ctx);
            noteedit::addLength(set_, d, false);
            commit(ctx);
        } else if (exact >= 0 || idx >= 0) {
            const int i = exact >= 0 ? exact : idx;
            beginEdit(ctx);
            noteedit::setDuration(set_.notes[i], clampi(set_.notes[i].durTicks() + d, kMinTicks, cfg::kMaxSteps * kTicks));
            commit(ctx);
        } else {
            brushTicks_ = clampi(brushTicks_ + d, kMinTicks, 16 * kTicks);
        }
    }

    // ---- right stick: velocity ----
    if (in.ry && ctx.nowMs - lastStickMs_ >= 80) {
        lastStickMs_ = ctx.nowMs;
        const int d = -in.ry / 24;
        if (d) {
            if (mode_ == Mode::Select && noteedit::selectedCount(set_)) {
                beginEdit(ctx);
                noteedit::addVelocity(set_, d, false);
                commit(ctx);
            } else if (idx >= 0) {
                beginEdit(ctx);
                set_.notes[idx].velocity = (uint8_t)clampi(set_.notes[idx].velocity + d, 1, 127);
                commit(ctx);
            } else {
                brushVel_ = clampi(brushVel_ + d, 1, 127);
            }
        }
    }

    // ---- mode specific buttons ----
    if (mode_ == Mode::Select) {
        if (in.hit(btn::Cross)) {
            if (idx >= 0) {
                noteedit::toggleSelect(set_, idx);
                sel_ = set_.sel;
            } else {
                ctx.toast("No note here: move onto a note and press " G_CROSS);
            }
        }
        if (in.hit(btn::Square)) {
            if (noteedit::selectedCount(set_)) {
                beginEdit(ctx);
                mode_ = Mode::Grab;
                ctx.toast("Grab: move with the D-pad, " G_CROSS " drops, " G_CIRCLE " cancels");
            } else {
                s.previewChannel(ch, pitch_ - pitch::kRootNote);
            }
        }
        if (in.hit(btn::Circle)) {
            if (noteedit::selectedCount(set_)) {
                noteedit::selectNone(set_);
                sel_ = 0;
            } else {
                mode_ = Mode::Draw;
                ctx.toast("DRAW mode");
            }
        }
        if (in.hit(btn::Triangle))
            openMenu(ctx);
        return;
    }

    // DRAW mode (unchanged bindings)
    if (in.hit(btn::Cross) && !l2Down_) {
        if (idx >= 0) {
            set_.sel = bit(idx);
            beginEdit(ctx);
            noteedit::removeSelected(set_);
            commit(ctx);
        } else if (tick_ == 0 && pitch_ == pitch::kRootNote && s.project().pattern().velocity[ch][step_]) {
            s.setStep(pat, ch, step_, 0); // the rack's root-pitch hit at this step
        } else if (set_.count < cfg::kMaxNotes) {
            beginEdit(ctx);
            PianoNote n = {};
            n.pitch = (uint8_t)pitch_;
            n.velocity = (uint8_t)brushVel_;
            noteedit::setStart(n, pos());
            noteedit::setDuration(n, brushTicks_);
            set_.notes[set_.count++] = n;
            commit(ctx);
            s.previewChannel(ch, pitch_ - pitch::kRootNote);
        } else {
            ctx.toast("Channel holds %d notes already", cfg::kMaxNotes);
        }
    }
    if (in.hit(btn::Square))
        s.previewChannel(ch, pitch_ - pitch::kRootNote);
    if (in.hit(btn::Circle) && !l2Down_) {
        ctx.selectedChannel = (ch + 1) % s.project().channelCount;
        ctx.toast("Channel %d: %s", ctx.selectedChannel + 1, s.project().channels[ctx.selectedChannel].name);
    }
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void PianoRollView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int ch = ctx.selectedChannel;
    const ChannelData& c = p.channels[ch];
    const PatternData& pat = p.pattern();
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    refresh(ctx);

    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, nullptr);
    g.fillRect(x, kViewTop, w, 22, theme::kPanelHeader);
    g.textf(x + 8, kViewTop + 3, theme::kText, "ROLL  CH%d %.7s", ch + 1, c.name);
    g.textf(x + 240, kViewTop + 3, theme::kTextDim, "P%d %.8s", p.currentPattern + 1, pat.name);
    const char* modeName = mode_ == Mode::Draw ? "DRAW" : (mode_ == Mode::Select ? "SELECT" : "GRAB");
    g.text(x + 370, kViewTop + 3, modeName, mode_ == Mode::Draw ? theme::kTextDim : theme::kEdit);
    g.text(x + 470, kViewTop + 3, c.inst.kind == (uint8_t)InstrKind::Synth ? "SYNTH" : (c.gate ? "SUSTAIN" : "ONE-SHOT"),
           c.gate ? theme::kAlive : theme::kTextDim);
    const int len = pat.length;
    const int winStart = scrollStep_ * kTicks, winEnd = (scrollStep_ + kVisibleSteps) * kTicks;
    auto px = [&](int ticks) { return kGridX + (ticks - winStart) * kCellW / kTicks; };
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
    // Snap guides: a brighter edge at every snap multiple that is not already a step line.
    if (snapTicks_ != kTicks)
        for (int t = winStart; t < winEnd && t < len * kTicks; t += snapTicks_)
            if (t % kTicks != 0 || snapTicks_ > kTicks)
                g.fillRect(px(t) - 1, kGridY, 1, kVisibleRows * kRowH, theme::kBorder);
    // Root-pitch hits placed in the rack's step grid show on the ROOT row.
    for (int i = 0; i < kVisibleSteps; ++i) {
        const int step = scrollStep_ + i;
        const int r = topPitch_ - pitch::kRootNote;
        if (step < len && pat.velocity[ch][step] && r >= 0 && r < kVisibleRows)
            g.fillRect(kGridX + i * kCellW + 1, kGridY + r * kRowH + 3, kCellW - 3, kRowH - 7, theme::kCellOnDim);
    }
    // Notes: velocity decides the colour; notes sounding right now glow; selected notes are outlined.
    const int heard = ctx.heardStep();
    const bool playingHere = heard >= 0 && ctx.engine.status().pattern == p.currentPattern;
    for (int n = 0; n < pat.noteCount[ch] && n < cfg::kMaxNotes; ++n) {
        const PianoNote& note = pat.notes[ch][n];
        const int r = topPitch_ - note.pitch;
        if (r < 0 || r >= kVisibleRows)
            continue;
        const int t0 = note.startTicks(), t1 = t0 + note.durTicks();
        if (t1 <= winStart || t0 >= winEnd)
            continue;
        const int v0 = t0 < winStart ? winStart : t0;
        const int v1 = t1 > winEnd ? winEnd : t1;
        const bool sounding = playingHere && heard * kTicks < t1 && heard * kTicks + kTicks > t0;
        uint32_t col = note.velocity >= 90 ? theme::kCellOn : theme::kCellOnDim;
        if (sounding)
            col = theme::kPlayhead;
        const int nx = px(v0), ny = kGridY + r * kRowH + 1;
        int nw = px(v1) - nx - 1;
        if (nw < 3)
            nw = 3; // chopped notes stay visible
        g.fillRect(nx, ny, nw, kRowH - 3, col);
        if (nw > 6)
            g.fillRect(nx, ny, 3, kRowH - 3, theme::kPlayhead);
        // velocity bar along the bottom edge
        if (nw > 6)
            g.fillRect(nx + 3, ny + kRowH - 6, (nw - 3) * note.velocity / 127, 2, theme::kTextDark);
        if (noteedit::isSelected(set_, n))
            g.frameRect(nx - 1, ny - 1, nw + 2, kRowH - 1, theme::kSelect, 2);
    }
    // Playhead.
    if (heard >= scrollStep_ && heard < scrollStep_ + kVisibleSteps && ctx.engine.status().pattern == p.currentPattern)
        g.fillRect(kGridX + (heard - scrollStep_) * kCellW + kCellW / 2 - 1, kGridY, 2, kVisibleRows * kRowH, theme::kPlayhead);
    // Cursor: as wide as the snap grid.
    const int cr = topPitch_ - pitch_;
    if (cr >= 0 && cr < kVisibleRows) {
        int cw = snapTicks_ * kCellW / kTicks - 1;
        if (cw < 4)
            cw = 4;
        ui::selectOutline(g, px(pos()), kGridY + cr * kRowH, cw, kRowH - 1, mode_ == Mode::Grab ? theme::kEdit : theme::kSelect);
    }
    // Scroll indicator for the pitch window on the right.
    ui::scrollbar(g, kGridX + kVisibleSteps * kCellW + 4, kGridY, 6, kVisibleRows * kRowH, 127 - topPitch_, kVisibleRows, 128);
    // Details.
    const int iy = kGridY + kVisibleRows * kRowH + 6;
    char nm[12], line[128], t1[32], t2[32];
    noteName(pitch_, nm, sizeof(nm));
    const int idx = noteedit::indexCovering(set_, pos(), pitch_);
    char at[24];
    if (tick_)
        snprintf(at, sizeof(at), "%d+%d/%d", step_ + 1, tick_, kTicks);
    else
        snprintf(at, sizeof(at), "%d", step_ + 1);
    if (idx >= 0) {
        const PianoNote& n = set_.notes[idx];
        snprintf(line, sizeof(line), "Step %s  %s  length %s  velocity %d  (%+d st)", at, nm, tickText(n.durTicks(), t1, sizeof(t1)),
                 n.velocity, n.pitch - pitch::kRootNote);
    } else {
        snprintf(line, sizeof(line), "Step %s  %s  empty. Cross places: %s, velocity %d", at, nm, tickText(brushTicks_, t1, sizeof(t1)),
                 brushVel_);
    }
    g.text(x + 8, iy, line, theme::kText, 1, 2);
    snprintf(line, sizeof(line), "%d/%d notes  snap %s  %d sel  undo %d  steps %d-%d/%d%s", pat.noteCount[ch], cfg::kMaxNotes,
             tickText(snapTicks_, t2, sizeof(t2)), noteedit::selectedCount(set_), undoCount_, scrollStep_ + 1,
             scrollStep_ + kVisibleSteps < len ? scrollStep_ + kVisibleSteps : len, len, clip_.count ? "  clip" : "");
    g.text(x + 8, iy + 16, line, theme::kTextDim, 1, 2);
    if (c.voiceMode == (uint8_t)VoiceMode::Spu2 && c.inst.kind != (uint8_t)InstrKind::Synth)
        g.text(x + 8, iy + 32, "SPU2 voice: pitch yes, note lengths ignored.", theme::kWarning, 1, 2);
}
