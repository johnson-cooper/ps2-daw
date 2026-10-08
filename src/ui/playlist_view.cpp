#include "ui/playlist_view.hpp"

#include <stdio.h>
#include <string.h>

#include "core/strutil.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"
#include "ui/waveform.hpp"

namespace {
constexpr int kCellW = 32;
constexpr int kRowH = 28; // eight tracks
constexpr int kGridX = theme::kSafeLeft + 44;
constexpr int kGridY = kViewTop + 48;
constexpr int kBarsPerStep = cfg::kStepsPerBeat * cfg::kBeatsPerBar; // steps per bar
// One distinguishable colour per pattern (original palette).
const uint32_t kPatternColors[cfg::kMaxPatterns] = {0xf0902a, 0x4aa8f0, 0x8ed44a, 0xd84a9a,
                                                    0x9a7cf0, 0xe8c63a, 0x4ad8c0, 0xd86a4a};
constexpr uint32_t kAudioColor = 0x2a7f78;
constexpr uint32_t kAudioWave = 0xb4f5ec;
const char* const kTrackNames[] = {"DRUMS", "BASS", "LEAD", "PAD", "VOX", "FX"};

// Bars an audio clip needs to hold the whole sample at the current tempo.
int autoBars(const Sample& s, uint32_t bpmCenti)
{
    const uint64_t num = (uint64_t)s.frames * bpmCenti;
    const uint64_t den = (uint64_t)(s.sampleRate ? s.sampleRate : 48000) * 6000ull * cfg::kBeatsPerBar;
    int bars = (int)((num + den - 1) / den);
    return bars < 1 ? 1 : (bars > 32 ? 32 : bars);
}
} // namespace

const char* PlaylistView::hint() const
{
    if (editingAudio_)
        return G_UP G_DOWN " ROW  " G_LEFT G_RIGHT " ADJUST  L1/R1 x10  " G_SQUARE " HEAR  " G_CIRCLE " DONE";
    if (audioBrush_)
        return G_CROSS " PLACE/REMOVE " G_SQUARE " MOVE " G_CIRCLE " SONG MODE " G_TRIANGLE " MENU  L1/R1 LEN  L2/R2 SAMPLE";
    return G_CROSS " PLACE/REMOVE " G_SQUARE " MOVE " G_CIRCLE " SONG MODE " G_TRIANGLE " MENU  L1/R1 LEN  L2/R2 PATTERN";
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
    if (track_ < trackScroll_)
        trackScroll_ = track_;
    if (track_ >= trackScroll_ + kVisibleTracks)
        trackScroll_ = track_ - kVisibleTracks + 1;
}

int PlaylistView::firstAudioSlot(const UiContext& ctx, int from, int dir) const
{
    const SampleBank& bank = ctx.bank;
    const int n = cfg::kMaxSamples;
    for (int k = 1; k <= n; ++k) {
        const int slot = ((from + dir * k) % n + n) % n;
        const Sample* s = bank.get(slot);
        if (s && s->data && !s->hwOnly)
            return slot;
    }
    return -1;
}

void PlaylistView::openMenu(UiContext& ctx)
{
    ctx.menu.open("PLAYLIST");
    const Project& p = ctx.session.project();
    char buf[44];
    ctx.menu.add(MenuMode, ctx.session.songMode() ? "Mode: SONG (plays playlist)" : "Mode: PATTERN (loops pattern)");
    snprintf(buf, sizeof(buf), "Play song from bar %d", bar_ + 1);
    ctx.menu.add(MenuPlayFromBar, buf, p.songBars() > 0);
    const Sample* smp = ctx.bank.get(audioSlot_);
    snprintf(buf, sizeof(buf), "Brush: %s", audioBrush_ ? "AUDIO SAMPLE" : "PATTERN");
    ctx.menu.add(MenuBrush, buf, audioBrush_ || firstAudioSlot(ctx, -1, +1) >= 0);
    snprintf(buf, sizeof(buf), "Sample: %.12s", smp ? smp->name : "(choose)");
    ctx.menu.add(MenuChooseSample, buf, firstAudioSlot(ctx, -1, +1) >= 0);
    const bool onPattern = p.clipAt(track_, bar_) >= 0, onAudio = p.audioClipAt(track_, bar_) >= 0;
    if (onPattern) {
        const PlaylistClip& pc = p.clips[p.clipAt(track_, bar_)];
        int content = 0;
        for (int ch = 0; ch < p.channelCount; ++ch)
            content += ((pc.chanMask >> ch) & 1) && ctx.session.channelHasContent(pc.pattern, ch);
        ctx.menu.add(MenuUngroup, "Ungroup: one track per instrument", content > 1);
        ctx.menu.add(MenuTakeOut, "Take one instrument out...", content > 1);
        ctx.menu.add(MenuRegroup, "Regroup: clip plays all instruments", pc.chanMask != kAllChannels);
    }
    ctx.menu.add(MenuEditAudio, "Edit audio clip: volume, loop, trim...", onAudio);
    snprintf(buf, sizeof(buf), "Track %d: %s", track_ + 1, (p.trackMute >> track_) & 1 ? "MUTED (unmute)" : "mute");
    ctx.menu.add(MenuMute, buf);
    snprintf(buf, sizeof(buf), "Track %d: %s", track_ + 1, (p.trackSolo >> track_) & 1 ? "SOLO (unsolo)" : "solo");
    ctx.menu.add(MenuSolo, buf);
    snprintf(buf, sizeof(buf), "Track %d name: %.8s (next)", track_ + 1, p.playlistTrackName[track_]);
    ctx.menu.add(MenuTrackName, buf);
    ctx.menu.add(MenuPick, "Use this clip as the brush", onPattern || onAudio);
    ctx.menu.add(MenuDuplicate, "Duplicate clip right after itself", onPattern || onAudio);
    ctx.menu.add(MenuMove, "Move clip (D-pad, Cross drops)", onPattern || onAudio);
    ctx.menu.add(MenuClearTrack, "Clear this track");
    ctx.menu.add(MenuClearAll, "Clear whole playlist...", p.clipCount > 0 || p.audioClipCount > 0);
}

void PlaylistView::handleMenu(int id, UiContext& ctx)
{
    Session& s = ctx.session;
    const Project& p = s.project();
    if (id >= MenuSampleBase && id < MenuSampleBase + cfg::kMaxSamples) {
        audioSlot_ = id - MenuSampleBase;
        audioBrush_ = true;
        const Sample* smp = ctx.bank.get(audioSlot_);
        if (smp)
            ctx.toast("Audio brush: %s (%d bar%s)", smp->name, autoBars(*smp, p.bpmCenti), autoBars(*smp, p.bpmCenti) == 1 ? "" : "s");
        return;
    }
    if (id >= MenuTakeOutBase && id < MenuTakeOutBase + cfg::kMaxChannels) {
        const int ci = p.clipAt(track_, bar_);
        if (ci >= 0 && s.splitClipChannel(ci, id - MenuTakeOutBase))
            ctx.toast("%s moved to its own clip on another track", p.channels[id - MenuTakeOutBase].name);
        else
            ctx.toast("No free track for that clip");
        return;
    }
    switch (id) {
    case MenuPick: {
        const int ai = p.audioClipAt(track_, bar_), ci = p.clipAt(track_, bar_);
        if (ai >= 0) {
            audioSlot_ = s.audioClipSlot(ai);
            audioBrush_ = true;
            brushBars_ = p.audioClips[ai].lengthBars;
            ctx.toast("Brush: sample %s", ctx.bank.get(audioSlot_) ? ctx.bank.get(audioSlot_)->name : "?");
        } else if (ci >= 0) {
            s.selectPattern(p.clips[ci].pattern);
            audioBrush_ = false;
            brushBars_ = p.clips[ci].lengthBars;
            ctx.toast("Brush: pattern %d", p.clips[ci].pattern + 1);
        }
        break;
    }
    case MenuUngroup: {
        const int ci = p.clipAt(track_, bar_);
        if (ci < 0)
            break;
        const int made = s.ungroupClip(ci);
        ctx.toast(made ? "Ungrouped: %d instrument%s now have their own clips" : "No free track to ungroup into", made, made == 1 ? "" : "s");
        break;
    }
    case MenuTakeOut: {
        const int ci = p.clipAt(track_, bar_);
        if (ci < 0)
            break;
        ctx.menu.open("TAKE OUT WHICH INSTRUMENT?");
        for (int ch = 0; ch < p.channelCount; ++ch)
            if (((p.clips[ci].chanMask >> ch) & 1) && s.channelHasContent(p.clips[ci].pattern, ch)) {
                char buf[40];
                snprintf(buf, sizeof(buf), "%d %.10s", ch + 1, p.channels[ch].name);
                ctx.menu.add(MenuTakeOutBase + ch, buf);
            }
        break;
    }
    case MenuRegroup: {
        const int ci = p.clipAt(track_, bar_);
        if (ci >= 0) {
            s.setClipMask(ci, kAllChannels);
            ctx.toast("Clip plays every instrument again (remove the split clips you no longer need)");
        }
        break;
    }
    case MenuMode:
        s.setSongMode(!s.songMode());
        ctx.toast(s.songMode() ? (p.songBars() ? "Song mode: playing the playlist" : "Song mode (playlist is empty)")
                               : "Pattern mode: looping the current pattern");
        break;
    case MenuPlayFromBar:
        s.setSongMode(true);
        s.playFromBar(bar_);
        ctx.toast("Song from bar %d", bar_ + 1);
        break;
    case MenuBrush:
        audioBrush_ = !audioBrush_;
        if (audioBrush_ && !ctx.bank.get(audioSlot_))
            audioSlot_ = firstAudioSlot(ctx, -1, +1);
        ctx.toast(audioBrush_ ? "Brush: audio sample" : "Brush: pattern");
        break;
    case MenuChooseSample: {
        ctx.menu.open("CHOOSE AUDIO SAMPLE");
        for (int i = 0; i < cfg::kMaxSamples; ++i) {
            const Sample* smp = ctx.bank.get(i);
            if (!smp || !smp->data || smp->hwOnly)
                continue;
            char buf[44];
            snprintf(buf, sizeof(buf), "%-10.10s %5lu ms%s", smp->name,
                     (unsigned long)((uint64_t)smp->frames * 1000u / (smp->sampleRate ? smp->sampleRate : 48000)),
                     i == audioSlot_ ? "  *" : "");
            ctx.menu.add(MenuSampleBase + i, buf);
        }
        break;
    }
    case MenuEditAudio: {
        const int idx = p.audioClipAt(track_, bar_);
        if (idx >= 0) {
            editingAudio_ = true;
            editClip_ = idx;
            editRow_ = 0;
        }
        break;
    }
    case MenuMute:
        s.setTrackMute(track_, !((p.trackMute >> track_) & 1));
        break;
    case MenuSolo:
        s.setTrackSolo(track_, !((p.trackSolo >> track_) & 1));
        break;
    case MenuTrackName: {
        int next = 0;
        for (int i = 0; i < 6; ++i)
            if (str::equalsNoCase(p.playlistTrackName[track_], kTrackNames[i]))
                next = i + 1;
        char name[24];
        if (next >= 6)
            snprintf(name, sizeof(name), "TRACK %d", track_ + 1);
        else
            snprintf(name, sizeof(name), "%s", kTrackNames[next]);
        s.setPlaylistTrackName(track_, name);
        ctx.toast("Track %d: %s", track_ + 1, name);
        break;
    }
    case MenuDuplicate: {
        const int ai = p.audioClipAt(track_, bar_);
        if (ai >= 0) {
            const int n = s.duplicateAudioClip(ai);
            if (n >= 0) {
                bar_ = s.project().audioClips[n].startBar;
                scrollToCursor();
                ctx.toast("Duplicated to bar %d", bar_ + 1);
            } else {
                ctx.toast("No free room for a copy");
            }
            break;
        }
        const int idx = p.clipAt(track_, bar_);
        if (idx < 0)
            break;
        const PlaylistClip k = p.clips[idx];
        const int start = k.startBar + k.lengthBars;
        const bool room = start + k.lengthBars <= cfg::kMaxSongBars && p.trackFree(k.track, start, k.lengthBars); // never overwrite another clip
        if (room && s.placeClip(k.track, start, k.pattern, k.lengthBars, k.chanMask)) {
            bar_ = start;
            scrollToCursor();
            ctx.toast("Duplicated to bar %d", start + 1);
        } else {
            ctx.toast("No free room right after the clip");
        }
        break;
    }
    case MenuMove: {
        const int ai = p.audioClipAt(track_, bar_);
        if (ai >= 0) {
            movingAudio_ = true;
            movingIndex_ = ai;
            moving_ = true;
            held_.lengthBars = p.audioClips[ai].lengthBars;
            held_.pattern = 7;
            bar_ = p.audioClips[ai].startBar;
            track_ = p.audioClips[ai].track;
            ctx.toast("Move the clip, " G_CROSS " drops it, " G_CIRCLE " cancels");
            break;
        }
        const int idx = p.clipAt(track_, bar_);
        if (idx < 0)
            break;
        held_ = p.clips[idx];
        s.removeClip(idx);
        moving_ = true;
        movingAudio_ = false;
        bar_ = held_.startBar;
        track_ = held_.track;
        ctx.toast("Move the clip, " G_CROSS " drops it, " G_CIRCLE " cancels");
        break;
    }
    case MenuClearTrack: {
        s.clearTrack(track_);
        for (int i = p.audioClipCount - 1; i >= 0; --i)
            if (p.audioClips[i].track == track_)
                s.removeAudioClip(i);
        ctx.toast("Track %d cleared", track_ + 1);
        break;
    }
    case MenuClearAll:
        ctx.menu.open("CLEAR EVERY CLIP?");
        ctx.menu.add(MenuClearAllConfirm, "Yes, clear the playlist");
        break;
    case MenuClearAllConfirm:
        s.clearPlaylist();
        while (s.project().audioClipCount > 0)
            s.removeAudioClip(0);
        ctx.toast("Playlist cleared");
        break;
    }
}

// ---------------------------------------------------------------------------
// audio clip editor (volume, loop, trim, length, mixer track)
// ---------------------------------------------------------------------------

void PlaylistView::updateAudioEditor(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;
    const Project& p = s.project();
    if (editClip_ < 0 || editClip_ >= p.audioClipCount) {
        editingAudio_ = false;
        return;
    }
    const AudioClipData& k = p.audioClips[editClip_];
    const Sample* smp = ctx.bank.get(p.audioSlot[k.source]);
    const uint32_t rate = smp && smp->sampleRate ? smp->sampleRate : 48000;
    if (in.rep(btn::Up) && editRow_ > 0)
        --editRow_;
    if (in.rep(btn::Down) && editRow_ + 1 < EditRows)
        ++editRow_;
    int dir = 0, mult = 1;
    if (in.rep(btn::Left))
        dir = -1;
    if (in.rep(btn::Right))
        dir = 1;
    if (in.rep(btn::L1)) {
        dir = -1;
        mult = 10;
    }
    if (in.rep(btn::R1)) {
        dir = 1;
        mult = 10;
    }
    if (dir) {
        switch (editRow_) {
        case EditVolume:
            s.setAudioClipVolume(editClip_, k.volume + dir * 2 * mult);
            break;
        case EditLoop:
            s.setAudioClipLoop(editClip_, !k.loop);
            break;
        case EditTrimStart: {
            const int64_t d = (int64_t)dir * mult * 10 * rate / 1000; // 10 ms steps
            int64_t v = (int64_t)k.trimStart + d;
            s.setAudioClipTrim(editClip_, v < 0 ? 0 : (uint32_t)v, k.trimEnd);
            break;
        }
        case EditTrimEnd: {
            const uint32_t total = smp ? smp->frames : 0;
            int64_t cur = k.trimEnd ? k.trimEnd : total;
            cur += (int64_t)dir * mult * 10 * rate / 1000;
            if (cur < 1)
                cur = 1;
            s.setAudioClipTrim(editClip_, k.trimStart, (uint32_t)cur);
            break;
        }
        case EditLength:
            s.setAudioClipLength(editClip_, k.lengthBars + dir);
            break;
        case EditRoute: {
            int r = k.route + dir;
            r = r < 0 ? cfg::kMixTracks : (r > cfg::kMixTracks ? 0 : r);
            s.setAudioClipRoute(editClip_, r);
            break;
        }
        }
    }
    if (in.hit(btn::Square) && smp) {
        const int slot = p.audioSlot[k.source];
        s.previewSample(slot, VoiceMode::Software);
    }
    if (in.hit(btn::Circle) || in.hit(btn::Triangle))
        editingAudio_ = false;
}

void PlaylistView::drawAudioEditor(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    if (editClip_ < 0 || editClip_ >= p.audioClipCount)
        return;
    const AudioClipData& k = p.audioClips[editClip_];
    const Sample* smp = ctx.bank.get(p.audioSlot[k.source]);
    const int x = theme::kSafeLeft + 70, y = kViewTop + 40, w = 450, h = 240;
    g.fillRect(x - 2, y - 2, w + 4, h + 4, theme::kAccent);
    char title[48];
    snprintf(title, sizeof(title), "AUDIO CLIP: %.14s", smp ? smp->name : "(missing)");
    ui::panel(g, x, y, w, h, title);
    const uint32_t rate = smp && smp->sampleRate ? smp->sampleRate : 48000;
    char val[24];
    for (int r = 0; r < EditRows; ++r) {
        const char* name = "";
        int frac = 0;
        switch (r) {
        case EditVolume:
            name = "VOLUME";
            snprintf(val, sizeof(val), "%d%%", k.volume);
            frac = k.volume * 10;
            break;
        case EditLoop:
            name = "LOOP";
            snprintf(val, sizeof(val), "%s", k.loop ? "ON" : "OFF");
            frac = k.loop ? 1000 : 0;
            break;
        case EditTrimStart:
            name = "TRIM START";
            snprintf(val, sizeof(val), "%lu ms", (unsigned long)((uint64_t)k.trimStart * 1000 / rate));
            frac = smp && smp->frames ? (int)((uint64_t)k.trimStart * 1000 / smp->frames) : 0;
            break;
        case EditTrimEnd:
            name = "TRIM END";
            if (k.trimEnd)
                snprintf(val, sizeof(val), "%lu ms", (unsigned long)((uint64_t)k.trimEnd * 1000 / rate));
            else
                snprintf(val, sizeof(val), "END");
            frac = smp && smp->frames ? (int)((uint64_t)(k.trimEnd ? k.trimEnd : smp->frames) * 1000 / smp->frames) : 1000;
            break;
        case EditLength:
            name = "LENGTH";
            snprintf(val, sizeof(val), "%d BAR%s", k.lengthBars, k.lengthBars == 1 ? "" : "S");
            frac = k.lengthBars * 1000 / 32;
            break;
        case EditRoute:
            name = "MIXER TRACK";
            snprintf(val, sizeof(val), "%s", k.route == 0 ? "MASTER" : p.tracks[k.route].name);
            frac = k.route * 1000 / cfg::kMixTracks;
            break;
        }
        ui::paramRow(g, x + 8, y + 32 + r * 22, w - 16, name, val, frac, r == editRow_, false);
    }
    // Trimmed region preview
    const int wy = y + 32 + EditRows * 22 + 8;
    g.fillRect(x + 8, wy, w - 16, 54, theme::kPanelDark);
    const uint8_t* peaks = ctx.waves.peaks(p.audioSlot[k.source]);
    if (peaks && smp && smp->frames) {
        const uint32_t end = k.trimEnd && k.trimEnd < smp->frames ? k.trimEnd : smp->frames;
        for (int px = 0; px < w - 16; ++px) {
            const int col = (int)((uint64_t)px * WaveformCache::kColumns / (w - 16));
            const uint32_t frame = (uint32_t)((uint64_t)smp->frames * col / WaveformCache::kColumns);
            const bool inside = frame >= k.trimStart && frame < end;
            const int amp = peaks[col] * 26 / 255;
            g.fillRect(x + 8 + px, wy + 27 - amp, 1, amp * 2 + 1, inside ? kAudioWave : theme::kBorder);
        }
    }
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void PlaylistView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }
    if (editingAudio_) {
        updateAudioEditor(in, ctx);
        return;
    }
    Session& s = ctx.session;
    const Project& p = s.project();
    if (moving_) {
        // Carrying a clip: only movement, drop and cancel are active.
        const int jump = in.down(btn::R2) ? 4 : 1; // hold R2 to carry four bars at a time
        if (in.rep(btn::Left))
            bar_ = bar_ - jump < 0 ? 0 : bar_ - jump;
        if (in.rep(btn::Right))
            bar_ = bar_ + jump > cfg::kMaxSongBars - 1 ? cfg::kMaxSongBars - 1 : bar_ + jump;
        if (!movingAudio_ && (in.rep(btn::L1) || in.rep(btn::R1))) {
            const int nl = held_.lengthBars + (in.rep(btn::R1) ? 1 : -1);
            held_.lengthBars = (uint16_t)(nl < 1 ? 1 : (nl > 32 ? 32 : nl));
        }
        if (in.rep(btn::Up) && track_ > 0)
            --track_;
        if (in.rep(btn::Down) && track_ < cfg::kPlaylistTracks - 1)
            ++track_;
        scrollToCursor();
        if (in.hit(btn::Cross)) {
            if (movingAudio_) {
                if (!s.moveAudioClip(movingIndex_, track_, bar_))
                    ctx.toast("That spot is taken");
                else
                    moving_ = movingAudio_ = false;
            } else {
                s.placeClip(track_, bar_, held_.pattern, held_.lengthBars, held_.chanMask);
                moving_ = false;
            }
        }
        if (in.hit(btn::Circle)) {
            if (!movingAudio_)
                s.placeClip(held_.track, held_.startBar, held_.pattern, held_.lengthBars, held_.chanMask);
            moving_ = movingAudio_ = false;
            ctx.toast("Move cancelled");
        }
        return;
    }
    if (!brushInit_) {
        brushInit_ = true;
        brushBars_ = s.patternBars(p.currentPattern);
        audioSlot_ = firstAudioSlot(ctx, -1, +1);
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
    const int aidx = p.audioClipAt(track_, bar_);
    // L2/R2: choose the pattern to paint with (the same selection the rack edits),
    // or, with the audio brush, the sample.
    if (in.hit(btn::L2) || in.hit(btn::R2)) {
        if (audioBrush_) {
            const int ns = firstAudioSlot(ctx, audioSlot_ < 0 ? -1 : audioSlot_, in.hit(btn::R2) ? +1 : -1);
            if (ns >= 0) {
                audioSlot_ = ns;
                const Sample* smp = ctx.bank.get(ns);
                brushBars_ = autoBars(*smp, p.bpmCenti);
                ctx.toast("Sample: %s (%d bar%s)", smp->name, brushBars_, brushBars_ == 1 ? "" : "s");
                s.previewSample(ns, VoiceMode::Software);
            } else {
                ctx.toast("No audio samples loaded: use the BROWSER");
            }
        } else {
            const int np = (p.currentPattern + (in.hit(btn::R2) ? 1 : cfg::kMaxPatterns - 1)) % cfg::kMaxPatterns;
            s.selectPattern(np);
            brushBars_ = s.patternBars(np);
            ctx.toast("Pattern %d: %s", np + 1, s.project().patterns[np].name);
        }
    }
    // L1/R1: length of the clip under the cursor, or of the next clip to place.
    if (in.rep(btn::L1) || in.rep(btn::R1)) {
        const int d = in.rep(btn::R1) ? 1 : -1;
        if (aidx >= 0) {
            s.setAudioClipLength(aidx, p.audioClips[aidx].lengthBars + d);
        } else if (idx >= 0) {
            s.setClipLength(idx, p.clips[idx].lengthBars + d);
        } else {
            brushBars_ += d;
            brushBars_ = brushBars_ < 1 ? 1 : (brushBars_ > 32 ? 32 : brushBars_);
        }
    }
    if (in.hit(btn::Cross)) {
        if (aidx >= 0) {
            s.removeAudioClip(aidx);
        } else if (idx >= 0) {
            s.removeClip(idx);
        } else if (audioBrush_) {
            const int n = ctx.bank.get(audioSlot_) ? s.placeAudioClip(track_, bar_, audioSlot_, brushBars_) : -1;
            if (n < 0)
                ctx.toast(ctx.bank.get(audioSlot_) ? "No room for the clip here (or table full)" : "Choose a sample first (menu)");
        } else if (!s.placeClip(track_, bar_, p.currentPattern, brushBars_)) {
            ctx.toast("Playlist is full (%d clips)", cfg::kMaxClips);
        }
    }
    // Square picks a clip up: carry it with the D-pad, Cross drops it (the menu has "use as brush").
    if (in.hit(btn::Square) && (aidx >= 0 || idx >= 0))
        handleMenu(MenuMove, ctx);
    if (in.hit(btn::Circle)) {
        s.setSongMode(!s.songMode());
        ctx.toast(s.songMode() ? (p.songBars() ? "SONG mode: START plays the playlist" : "SONG mode, but the playlist is empty")
                               : "PATTERN mode: START loops the current pattern");
    }
    if (in.hit(btn::Triangle))
        openMenu(ctx);
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void PlaylistView::drawAudioClip(Gfx& g, UiContext& ctx, int i)
{
    const Project& p = ctx.session.project();
    const AudioClipData& k = p.audioClips[i];
    const int s0 = k.startBar, s1 = k.startBar + k.lengthBars;
    if (s1 <= scroll_ || s0 >= scroll_ + kVisibleBars)
        return;
    const int v0 = s0 < scroll_ ? scroll_ : s0, v1 = s1 > scroll_ + kVisibleBars ? scroll_ + kVisibleBars : s1;
    const int cx = kGridX + (v0 - scroll_) * kCellW, cw = (v1 - v0) * kCellW;
    const int arow = rowOf(k.track);
    if (arow < 0)
        return;
    const int ry = kGridY + arow * kRowH;
    g.fillRect(cx + 1, ry + 1, cw - 2, kRowH - 2, kAudioColor);

    // Waveform of the trimmed region, drawn at the clip's real time scale.
    const int slot = p.audioSlot[k.source];
    const Sample* smp = ctx.bank.get(slot);
    const uint8_t* peaks = ctx.waves.peaks(slot);
    if (smp && peaks && smp->frames && p.bpmCenti) {
        const uint32_t end = k.trimEnd && k.trimEnd < smp->frames ? k.trimEnd : smp->frames;
        const uint32_t trim = k.trimStart < end ? k.trimStart : 0;
        const uint64_t region = end - trim;
        // source frames advanced per pixel: (bar frames / pixels per bar) * rate / 48000
        const uint64_t barFrames = 1152000000ull / p.bpmCenti;
        const uint64_t srcPerBar = barFrames * (smp->sampleRate ? smp->sampleRate : 48000) / cfg::kSampleRate;
        for (int px = 0; px < cw - 2; ++px) {
            const uint64_t barPos = (uint64_t)(v0 - s0) * kCellW + (uint64_t)px;
            uint64_t off = barPos * srcPerBar / kCellW;
            if (off >= region) {
                if (!k.loop)
                    break;
                off %= region;
            }
            const uint32_t frame = trim + (uint32_t)off;
            const int col = (int)((uint64_t)frame * WaveformCache::kColumns / smp->frames);
            const int amp = peaks[col < WaveformCache::kColumns ? col : WaveformCache::kColumns - 1] * 5 / 255; // lower half: the name sits above
            if (amp > 0)
                g.fillRect(cx + 1 + px, ry + 19 - amp, 1, amp * 2, kAudioWave);
        }
    }
    if (s0 >= scroll_) {
        {
            char nm[10];
            snprintf(nm, sizeof(nm), "%.8s", smp ? smp->name : "MISSING");
            g.text(cx + 4, ry + 2, nm, theme::kTextDark, 1, 1);
        }
        if (k.loop)
            g.text(cx + cw - 10, ry + 17, "L", theme::kTextDark, 1, 1);
    } else {
        g.text(cx + 4, ry + 6, G_RIGHT, theme::kTextDark, 1, 2);
    }
    if (s1 <= scroll_ + kVisibleBars)
        g.fillRect(cx + cw - 3, ry + 1, 2, kRowH - 2, theme::kPanelDark);
    if (!smp)
        g.frameRect(cx + 1, ry + 1, cw - 2, kRowH - 2, theme::kError, 2);
}

void PlaylistView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, "PLAYLIST");
    const bool song = ctx.session.songMode();
    g.textf(x + 120, kViewTop + 3, song ? theme::kAlive : theme::kTextDim, "%s  %d BARS", song ? "SONG MODE" : "PATTERN MODE",
            p.songBars());
    // Brush indicator in the header.
    if (audioBrush_) {
        const Sample* smp = ctx.bank.get(audioSlot_);
        g.textf(x + 390, kViewTop + 3, theme::kAlive, "AUDIO %.8s", smp ? smp->name : "-");
    } else {
        g.textf(x + 390, kViewTop + 3, kPatternColors[p.currentPattern % cfg::kMaxPatterns], "PATTERN %d", p.currentPattern + 1);
    }
    // Bar ruler.
    for (int i = 0; i < kVisibleBars; ++i) {
        const int bar = scroll_ + i;
        const int cx = kGridX + i * kCellW;
        if (bar % 4 == 0)
            g.textf(cx + 2, kGridY - 18, theme::kTextDim, "%d", bar + 1);
    }
    // Rows and cells.
    for (int tr = 0; tr < kVisibleTracks; ++tr) {
        const int t = trackScroll_ + tr;
        const int ry = kGridY + tr * kRowH;
        const bool muted = (p.trackMute >> t) & 1, soloed = (p.trackSolo >> t) & 1;
        g.textf(x + 6, ry + 1, muted ? theme::kMute : (t == track_ ? theme::kText : theme::kTextDim), "T%d", t + 1);
        if (strncmp(p.playlistTrackName[t], "TRACK", 5) != 0)
            {
                char nm[8];
                snprintf(nm, sizeof(nm), "%.6s", p.playlistTrackName[t]);
                g.text(x + 4, ry + 18, nm, t == track_ ? theme::kAccent : theme::kTextDim, 1, 1);
            }
        else if (muted || soloed)
            g.text(x + 30, ry + 6, muted ? "M" : "S", muted ? theme::kMute : theme::kSolo, 1, 2);
        if ((muted || soloed) && strncmp(p.playlistTrackName[t], "TRACK", 5) != 0)
            g.text(x + 30, ry + 6, muted ? "M" : "S", muted ? theme::kMute : theme::kSolo, 1, 2);
        for (int i = 0; i < kVisibleBars; ++i) {
            const int bar = scroll_ + i;
            g.fillRect(kGridX + i * kCellW + 1, ry + 1, kCellW - 2, kRowH - 2, (bar / 4) % 2 ? theme::kCellOffAlt : theme::kCellOff);
        }
    }
    // Pattern clips: one rectangle per clip, clipped to the visible window.
    for (int i = 0; i < p.clipCount; ++i) {
        const PlaylistClip& k = p.clips[i];
        const int s0 = k.startBar, s1 = k.startBar + k.lengthBars;
        if (s1 <= scroll_ || s0 >= scroll_ + kVisibleBars)
            continue;
        const int v0 = s0 < scroll_ ? scroll_ : s0, v1 = s1 > scroll_ + kVisibleBars ? scroll_ + kVisibleBars : s1;
        const int cx = kGridX + (v0 - scroll_) * kCellW, cw = (v1 - v0) * kCellW;
        const int prow = rowOf(k.track);
        if (prow < 0)
            continue;
        const int ry = kGridY + prow * kRowH;
        const uint32_t col = kPatternColors[k.pattern % cfg::kMaxPatterns];
        g.fillRect(cx + 1, ry + 1, cw - 2, kRowH - 2, col);
        if (s0 >= scroll_) { // label only where the clip really starts
            char lab[24];
            int plays = 0, only = -1;
            for (int ch = 0; ch < p.channelCount; ++ch)
                if (((k.chanMask >> ch) & 1) && ctx.session.channelHasContent(k.pattern, ch)) {
                    ++plays;
                    only = ch;
                }
            const bool masked = k.chanMask != kAllChannels && plays > 0;
            if (!masked)
                snprintf(lab, sizeof(lab), "P%d", k.pattern + 1);
            else if (plays == 1)
                snprintf(lab, sizeof(lab), "P%d", k.pattern + 1);
            else
                snprintf(lab, sizeof(lab), "P%d +%d", k.pattern + 1, plays);
            const int fit = (cw - 8) / 12; // never draw past the clip
            if (fit >= 2 && (int)strlen(lab) > fit)
                lab[fit] = '\0';
            if (masked && plays == 1) { // an ungrouped instrument: pattern number, then its name in small type
                g.text(cx + 4, ry + 2, lab, theme::kTextDark, 1, 2);
                char nm[12];
                snprintf(nm, sizeof(nm), "%.*s", (cw - 8) / 6 > 10 ? 10 : (cw - 8) / 6, p.channels[only].name);
                g.text(cx + 4, ry + 18, nm, theme::kTextDark, 1, 1);
            } else {
                g.text(cx + 4, ry + 6, lab, theme::kTextDark, 1, 2);
            }
        } else {
            g.text(cx + 4, ry + 6, G_RIGHT, theme::kTextDark);
        }
        // Notch where the clip ends so adjacent clips read as separate.
        if (s1 <= scroll_ + kVisibleBars)
            g.fillRect(cx + cw - 3, ry + 1, 2, kRowH - 2, theme::kPanelDark);
    }
    for (int i = 0; i < p.audioClipCount; ++i)
        drawAudioClip(g, ctx, i);
    // End of the song.
    const int end = p.songBars();
    if (end > scroll_ && end <= scroll_ + kVisibleBars)
        g.fillRect(kGridX + (end - scroll_) * kCellW - 1, kGridY - 4, 2, kVisibleTracks * kRowH + 4, theme::kMute);
    // Playhead (song mode only: in pattern mode the rack shows the playhead).
    const int songStep = ctx.heardSongStep();
    if (song && songStep >= 0) {
        const int px = kGridX + songStep * kCellW / kBarsPerStep - scroll_ * kCellW;
        if (px >= kGridX && px < kGridX + kVisibleBars * kCellW)
            g.fillRect(px, kGridY - 4, 2, kVisibleTracks * kRowH + 4, theme::kPlayhead);
    }
    // A carried clip: outline where it would land.
    if (moving_) {
        const int gw = (movingAudio_ ? p.audioClips[movingIndex_].lengthBars : held_.lengthBars) * kCellW;
        const int vis = (bar_ - scroll_) * kCellW;
        g.frameRect(kGridX + vis, kGridY + (track_ - trackScroll_) * kRowH, gw > kVisibleBars * kCellW - vis ? kVisibleBars * kCellW - vis : gw,
                    kRowH, movingAudio_ ? kAudioWave : kPatternColors[held_.pattern % cfg::kMaxPatterns], 2);
    }
    // Cursor.
    ui::selectOutline(g, kGridX + (bar_ - scroll_) * kCellW, kGridY + (track_ - trackScroll_) * kRowH, kCellW, kRowH, theme::kSelect);
    ui::scrollbar(g, kGridX + kVisibleBars * kCellW + 6, kGridY, 6, kVisibleTracks * kRowH, trackScroll_, kVisibleTracks, cfg::kPlaylistTracks);
    // Details (small font, two lines).
    const int iy = kGridY + kVisibleTracks * kRowH + 8;
    const int idx = p.clipAt(track_, bar_);
    const int aidx = p.audioClipAt(track_, bar_);
    char line[110];
    if (moving_) {
        snprintf(line, sizeof(line), "Moving %s (%d bar%s) to T%d bar %d", movingAudio_ ? "audio clip" : "pattern clip",
                 movingAudio_ ? p.audioClips[movingIndex_].lengthBars : held_.lengthBars,
                 (movingAudio_ ? p.audioClips[movingIndex_].lengthBars : held_.lengthBars) == 1 ? "" : "s", track_ + 1, bar_ + 1);
    } else if (aidx >= 0) {
        const AudioClipData& k = p.audioClips[aidx];
        const Sample* smp = ctx.bank.get(p.audioSlot[k.source]);
        snprintf(line, sizeof(line), "T%d bar %d: AUDIO %.10s %d bar%s vol %d%%%s -> %s  (menu: edit)", track_ + 1, bar_ + 1,
                 smp ? smp->name : "MISSING", k.lengthBars, k.lengthBars == 1 ? "" : "s", k.volume, k.loop ? " loop" : "",
                 k.route == 0 ? "MASTER" : p.tracks[k.route].name);
    } else if (idx >= 0) {
        const PlaylistClip& k = p.clips[idx];
        snprintf(line, sizeof(line), "T%d bar %d: P%d %.12s, %d bar%s (bars %d-%d)  L1/R1 length", track_ + 1, bar_ + 1,
                 k.pattern + 1, p.patterns[k.pattern].name, k.lengthBars, k.lengthBars == 1 ? "" : "s", k.startBar + 1,
                 k.startBar + k.lengthBars);
    } else if (audioBrush_) {
        const Sample* smp = ctx.bank.get(audioSlot_);
        snprintf(line, sizeof(line), "T%d bar %d: empty. Cross places audio %.10s, %d bar%s", track_ + 1, bar_ + 1,
                 smp ? smp->name : "(none)", brushBars_, brushBars_ == 1 ? "" : "s");
    } else {
        snprintf(line, sizeof(line), "T%d bar %d: empty. Cross places P%d %.12s, %d bar%s", track_ + 1, bar_ + 1, p.currentPattern + 1,
                 p.pattern().name, brushBars_, brushBars_ == 1 ? "" : "s");
    }
    g.text(x + 8, iy, line, theme::kText, 1, 2);
    const int total = p.clipCount + p.audioClipCount;
    snprintf(line, sizeof(line), "%d clip%s (%d audio). %s", total, total == 1 ? "" : "s", p.audioClipCount,
             song ? (total ? "START plays from bar 1 and loops at the end" : "Add clips: START has nothing to play")
                  : "PATTERN mode ignores the playlist. Circle: SONG");
    g.text(x + 8, iy + 16, line, theme::kTextDim, 1, 2);
    if (editingAudio_)
        drawAudioEditor(g, ctx);
}
