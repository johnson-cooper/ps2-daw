#include "ui/playlist_view.hpp"

#include <stdio.h>

#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kCellW = 32;
constexpr int kRowH = 34;
constexpr int kGridX = theme::kSafeLeft + 44;
constexpr int kGridY = kViewTop + 48;
constexpr int kBarsPerStep = cfg::kStepsPerBeat * cfg::kBeatsPerBar; // steps per bar

// One distinguishable colour per pattern (original palette).
const uint32_t kPatternColors[cfg::kMaxPatterns] = {0xf0902a, 0x4aa8f0, 0x8ed44a, 0xd84a9a,
                                                    0x9a7cf0, 0xe8c63a, 0x4ad8c0, 0xd86a4a};
} // namespace

const char* PlaylistView::hint() const
{
    return G_CROSS " PLACE/REMOVE " G_SQUARE " PICK " G_CIRCLE " SONG MODE " G_TRIANGLE " MENU  L1/R1 LEN  L2/R2 PATTERN";
}

void PlaylistView::onEnter(UiContext& ctx)
{
    brushBars_ = ctx.session.patternBars(ctx.session.project().currentPattern);
}

void PlaylistView::scrollToCursor()
{
    if (bar_ < scroll_)
        scroll_ = bar_;
    if (bar_ >= scroll_ + kVisibleBars)
        scroll_ = bar_ - kVisibleBars + 1;
    if (scroll_ < 0)
        scroll_ = 0;
}

void PlaylistView::openMenu(UiContext& ctx)
{
    ctx.menu.open("PLAYLIST");
    ctx.menu.add(MenuMode, ctx.session.songMode() ? "Mode: SONG (plays playlist)" : "Mode: PATTERN (loops pattern)");
    ctx.menu.add(MenuClearTrack, "Clear this track");
    ctx.menu.add(MenuClearAll, "Clear whole playlist...", ctx.session.project().clipCount > 0);
}

void PlaylistView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    switch (id) {
    case MenuMode:
        s.setSongMode(!s.songMode());
        ctx.toast(s.songMode() ? (s.project().clipCount ? "Song mode: playing the playlist" : "Song mode (playlist is empty)")
                               : "Pattern mode: looping the current pattern");
        break;
    case MenuClearTrack:
        s.clearTrack(track_);
        ctx.toast("Track %d cleared", track_ + 1);
        break;
    case MenuClearAll:
        ctx.menu.open("CLEAR EVERY CLIP?");
        ctx.menu.add(MenuClearAllConfirm, "Yes, clear the playlist");
        break;
    case MenuClearAllConfirm:
        s.clearPlaylist();
        ctx.toast("Playlist cleared");
        break;
    }
}

void PlaylistView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }
    Session& s = ctx.session;
    const Project& p = s.project();
    if (!brushInit_) {
        brushInit_ = true;
        brushBars_ = s.patternBars(p.currentPattern);
    }

    if (in.rep(btn::Left) && bar_ > 0)
        --bar_;
    if (in.rep(btn::Right) && bar_ < cfg::kMaxSongBars - 1)
        ++bar_;
    if (in.rep(btn::Up) && track_ > 0)
        --track_;
    if (in.rep(btn::Down) && track_ < cfg::kPlaylistTracks - 1)
        ++track_;
    scrollToCursor();

    const int idx = p.clipAt(track_, bar_);

    // L2/R2: choose the pattern to paint with (the same selection the rack edits).
    if (in.hit(btn::L2) || in.hit(btn::R2)) {
        const int np = (p.currentPattern + (in.hit(btn::R2) ? 1 : cfg::kMaxPatterns - 1)) % cfg::kMaxPatterns;
        s.selectPattern(np);
        brushBars_ = s.patternBars(np);
        ctx.toast("Pattern %d: %s", np + 1, s.project().patterns[np].name);
    }
    // L1/R1: length of the clip under the cursor, or of the next clip to place.
    if (in.rep(btn::L1) || in.rep(btn::R1)) {
        const int d = in.rep(btn::R1) ? 1 : -1;
        if (idx >= 0) {
            s.setClipLength(idx, p.clips[idx].lengthBars + d);
        } else {
            brushBars_ += d;
            brushBars_ = brushBars_ < 1 ? 1 : (brushBars_ > 16 ? 16 : brushBars_);
        }
    }
    if (in.hit(btn::Cross)) {
        if (idx >= 0) {
            s.removeClip(idx);
        } else if (!s.placeClip(track_, bar_, p.currentPattern, brushBars_)) {
            ctx.toast("Playlist is full (%d clips)", cfg::kMaxClips);
        }
    }
    if (in.hit(btn::Square) && idx >= 0) {
        s.selectPattern(p.clips[idx].pattern);
        brushBars_ = p.clips[idx].lengthBars;
        ctx.toast("Picked pattern %d", p.clips[idx].pattern + 1);
    }
    if (in.hit(btn::Circle)) {
        s.setSongMode(!s.songMode());
        ctx.toast(s.songMode() ? (p.clipCount ? "SONG mode: START plays the playlist" : "SONG mode, but the playlist is empty")
                               : "PATTERN mode: START loops the current pattern");
    }
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

void PlaylistView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, "PLAYLIST");
    const bool song = ctx.session.songMode();
    g.textf(x + 120, kViewTop + 3, song ? theme::kAlive : theme::kTextDim, "%s  %d BARS", song ? "SONG MODE" : "PATTERN MODE",
            p.songBars());

    // Bar ruler.
    for (int i = 0; i < kVisibleBars; ++i) {
        const int bar = scroll_ + i;
        const int cx = kGridX + i * kCellW;
        if (bar % 4 == 0)
            g.textf(cx + 2, kGridY - 18, theme::kTextDim, "%d", bar + 1);
    }
    // Rows and cells.
    for (int t = 0; t < cfg::kPlaylistTracks; ++t) {
        const int ry = kGridY + t * kRowH;
        g.textf(x + 6, ry + 9, t == track_ ? theme::kText : theme::kTextDim, "T%d", t + 1);
        for (int i = 0; i < kVisibleBars; ++i) {
            const int bar = scroll_ + i;
            g.fillRect(kGridX + i * kCellW + 1, ry + 1, kCellW - 2, kRowH - 2, (bar / 4) % 2 ? theme::kCellOffAlt : theme::kCellOff);
        }
    }
    // Clips: one rectangle per clip, clipped to the visible window.
    for (int i = 0; i < p.clipCount; ++i) {
        const PlaylistClip& k = p.clips[i];
        const int s0 = k.startBar, s1 = k.startBar + k.lengthBars;
        if (s1 <= scroll_ || s0 >= scroll_ + kVisibleBars)
            continue;
        const int v0 = s0 < scroll_ ? scroll_ : s0, v1 = s1 > scroll_ + kVisibleBars ? scroll_ + kVisibleBars : s1;
        const int cx = kGridX + (v0 - scroll_) * kCellW, cw = (v1 - v0) * kCellW;
        const int ry = kGridY + k.track * kRowH;
        const uint32_t col = kPatternColors[k.pattern % cfg::kMaxPatterns];
        g.fillRect(cx + 1, ry + 1, cw - 2, kRowH - 2, col);
        if (s0 >= scroll_) // label only where the clip really starts
            g.textf(cx + 4, ry + 9, theme::kTextDark, "P%d", k.pattern + 1);
        else
            g.text(cx + 4, ry + 9, G_RIGHT, theme::kTextDark);
        // Notch where the clip ends so adjacent clips read as separate.
        if (s1 <= scroll_ + kVisibleBars)
            g.fillRect(cx + cw - 3, ry + 1, 2, kRowH - 2, theme::kPanelDark);
    }
    // End of the song.
    const int end = p.songBars();
    if (end > scroll_ && end <= scroll_ + kVisibleBars)
        g.fillRect(kGridX + (end - scroll_) * kCellW - 1, kGridY - 4, 2, cfg::kPlaylistTracks * kRowH + 4, theme::kMute);

    // Playhead (song mode only: in pattern mode the rack shows the playhead).
    const int songStep = ctx.heardSongStep();
    if (song && songStep >= 0) {
        const int px = kGridX + songStep * kCellW / kBarsPerStep - scroll_ * kCellW;
        if (px >= kGridX && px < kGridX + kVisibleBars * kCellW)
            g.fillRect(px, kGridY - 4, 2, cfg::kPlaylistTracks * kRowH + 4, theme::kPlayhead);
    }
    // Cursor.
    ui::selectOutline(g, kGridX + (bar_ - scroll_) * kCellW, kGridY + track_ * kRowH, kCellW, kRowH, theme::kSelect);

    // Details (small font, two lines).
    const int iy = kGridY + cfg::kPlaylistTracks * kRowH + 8;
    const int idx = p.clipAt(track_, bar_);
    char line[96];
    if (idx >= 0) {
        const PlaylistClip& k = p.clips[idx];
        snprintf(line, sizeof(line), "T%d bar %d: P%d %.12s, %d bar%s (bars %d-%d)  L1/R1 length", track_ + 1, bar_ + 1,
                 k.pattern + 1, p.patterns[k.pattern].name, k.lengthBars, k.lengthBars == 1 ? "" : "s", k.startBar + 1,
                 k.startBar + k.lengthBars);
    } else {
        snprintf(line, sizeof(line), "T%d bar %d: empty. Cross places P%d %.12s, %d bar%s", track_ + 1, bar_ + 1, p.currentPattern + 1,
                 p.pattern().name, brushBars_, brushBars_ == 1 ? "" : "s");
    }
    g.text(x + 8, iy, line, theme::kText, 1, 2);
    snprintf(line, sizeof(line), "%d clip%s. %s", p.clipCount, p.clipCount == 1 ? "" : "s",
             song ? (p.clipCount ? "START plays from bar 1 and loops at the end" : "Add clips: START has nothing to play")
                  : "PATTERN mode ignores the playlist. Circle: SONG");
    g.text(x + 8, iy + 16, line, theme::kTextDim, 1, 2);
}
