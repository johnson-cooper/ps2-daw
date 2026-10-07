#include "ui/browser.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/sample_ref.hpp"
#include "core/strutil.hpp"
#include "project/sample_library.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kRowH = 22;
constexpr int kListY = kViewTop + 50;
constexpr int kVisibleRows = 9;

void sizeText(char* out, size_t cap, uint32_t bytes)
{
    if (bytes >= 1024u * 1024u)
        snprintf(out, cap, "%lu.%lu MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes & 0xfffff) * 10 >> 20));
    else
        snprintf(out, cap, "%lu KB", (unsigned long)((bytes + 1023) / 1024));
}
} // namespace

const char* BrowserView::hint() const
{
    return G_CROSS " LOAD/ASSIGN " G_CIRCLE " UP " G_SQUARE " PREVIEW " G_TRIANGLE " SPU2  L2 SOURCE  R2 MENU  L1/R1 CH";
}

void BrowserView::onEnter(UiContext& ctx)
{
    if (!scanned_ && ctx.storage.ready())
        scan(ctx);
}

void BrowserView::rebuildLoadedList(const UiContext& ctx)
{
    loadedCount_ = 0;
    for (int i = 0; i < ctx.bank.count() && loadedCount_ < cfg::kMaxSamples; ++i)
        if (ctx.bank.get(i))
            loadedSlots_[loadedCount_++] = i;
}

int BrowserView::itemCount(const UiContext&) const
{
    return source_ == SrcLoaded ? loadedCount_ : entryCount_;
}

void BrowserView::clampSel(const UiContext& ctx)
{
    const int n = itemCount(ctx);
    int& sel = sel_[source_];
    int& first = first_[source_];
    if (sel >= n)
        sel = n > 0 ? n - 1 : 0;
    if (sel < 0)
        sel = 0;
    if (sel < first)
        first = sel;
    if (sel >= first + kVisibleRows)
        first = sel - kVisibleRows + 1;
    if (first < 0)
        first = 0;
}

void BrowserView::scan(UiContext& ctx)
{
    scanned_ = true;
    entryCount_ = 0;
    truncated_ = false;
    sel_[SrcUsb] = first_[SrcUsb] = 0;
    const int n = ctx.storage.listSamples(dirRel_, entries_, kMaxEntries, &truncated_);
    scanFailed_ = n < 0;
    if (n < 0)
        return;
    entryCount_ = n;
    // Directories first, then by name (insertion sort: <= 96 short entries).
    for (int i = 1; i < entryCount_; ++i) {
        Storage::DirEntry tmp = entries_[i];
        int j = i - 1;
        while (j >= 0 && sampleref::compareEntries(entries_[j].name, entries_[j].isDir, tmp.name, tmp.isDir) > 0) {
            entries_[j + 1] = entries_[j];
            --j;
        }
        entries_[j + 1] = tmp;
    }
}

bool BrowserView::selectedRef(UiContext& ctx, char* ref, size_t cap, bool toastErrors)
{
    if (source_ == SrcLoaded) {
        const Sample* s = ctx.bank.get(selectedSlot(ctx));
        if (!s)
            return false;
        str::copy(ref, cap, s->ref);
        return true;
    }
    const int i = sel_[SrcUsb];
    if (i < 0 || i >= entryCount_ || entries_[i].isDir)
        return false;
    char rel[96];
    if (!sampleref::joinRel(rel, sizeof(rel), dirRel_, entries_[i].name) || !sampleref::make(ref, cap, rel)) {
        if (toastErrors)
            ctx.toast("Name/path too long or has unsupported characters");
        return false;
    }
    return true;
}

int BrowserView::selectedSlot(const UiContext& ctx) const
{
    if (source_ == SrcLoaded) {
        const int i = sel_[SrcLoaded];
        return (i >= 0 && i < loadedCount_) ? loadedSlots_[i] : -1;
    }
    const int i = sel_[SrcUsb];
    if (i < 0 || i >= entryCount_ || entries_[i].isDir)
        return -1;
    char rel[96], ref[sampleref::kMaxRef];
    if (!sampleref::joinRel(rel, sizeof(rel), dirRel_, entries_[i].name) || !sampleref::make(ref, sizeof(ref), rel))
        return -1;
    return ctx.bank.findByRef(ref);
}

void BrowserView::act(UiContext& ctx, int actionId)
{
    SampleLibrary& lib = ctx.library;
    const int slot = selectedSlot(ctx);
    const auto action = (SampleLibrary::Action)actionId;

    if (slot >= 0) { // already in memory: no I/O needed
        const Sample* s = ctx.bank.get(slot);
        if (!s)
            return;
        switch (action) {
        case SampleLibrary::Action::Assign:
            ctx.session.setSample(ctx.selectedChannel, slot);
            ctx.toast("Channel %d <- %s", ctx.selectedChannel + 1, s->name);
            break;
        case SampleLibrary::Action::Preview:
            if (s->hwOnly && !ctx.audio.resident(slot))
                ctx.toast("SPU2 sample not resident");
            else
                ctx.session.previewSample(slot, s->hwOnly ? VoiceMode::Spu2 : VoiceMode::Software);
            break;
        case SampleLibrary::Action::PreviewSpu2:
            if (lib.uploadSpu2(slot))
                ctx.session.previewSample(slot, VoiceMode::Spu2);
            else
                ctx.toast("%s", lib.message());
            break;
        case SampleLibrary::Action::UploadSpu2:
            lib.uploadSpu2(slot);
            ctx.toast("%s", lib.message());
            break;
        default:
            break;
        }
        return;
    }

    char ref[sampleref::kMaxRef];
    if (!selectedRef(ctx, ref, sizeof(ref), true))
        return;
    if (lib.request(ref, action, ctx.selectedChannel))
        ctx.toast("Loading %s...", sampleref::baseName(ref));
    else
        ctx.toast("%s", lib.message());
}

void BrowserView::openMenu(UiContext& ctx)
{
    const int slot = selectedSlot(ctx);
    const Sample* s = ctx.bank.get(slot);
    ctx.menu.open("SAMPLE ACTIONS");
    const bool isFile = source_ == SrcLoaded ? s != nullptr : (sel_[SrcUsb] < entryCount_ && !entries_[sel_[SrcUsb]].isDir);
    const bool mono = s ? (s->channels == 1 && !s->hwOnly) : true;
    const bool res = slot >= 0 && ctx.audio.resident(slot);
    ctx.menu.add(MenuLoad, "Load into memory", isFile && slot < 0);
    ctx.menu.add(MenuAssign, "Assign to selected channel", isFile);
    ctx.menu.add(MenuPreviewSpu2, "Preview on SPU2 voice", isFile && mono);
    ctx.menu.add(MenuUploadSpu2, "Upload to SPU2 RAM", isFile && mono && !res);
    ctx.menu.add(MenuRemoveSpu2, "Remove from SPU2 RAM", res && s && !s->hwOnly);
    ctx.menu.add(MenuUnload, "Unload sample (free RAM)", s && !s->builtin);
    ctx.menu.add(MenuRescan, "Rescan folder", source_ == SrcUsb);
    ctx.menu.add(MenuCreateDir, "Create PS2DAW/SAMPLES folder", source_ == SrcUsb && scanFailed_ && ctx.storage.ready());
}

void BrowserView::handleMenu(int id, UiContext& ctx)
{
    const int slot = selectedSlot(ctx);
    switch (id) {
    case MenuLoad:
        act(ctx, (int)SampleLibrary::Action::Load);
        break;
    case MenuAssign:
        act(ctx, (int)SampleLibrary::Action::Assign);
        break;
    case MenuPreviewSpu2:
        act(ctx, (int)SampleLibrary::Action::PreviewSpu2);
        break;
    case MenuUploadSpu2:
        act(ctx, (int)SampleLibrary::Action::UploadSpu2);
        break;
    case MenuRemoveSpu2:
        if (slot >= 0) {
            ctx.library.unloadSpu2(slot);
            ctx.toast("%s", ctx.library.message());
        }
        break;
    case MenuUnload:
        if (slot >= 0) {
            ctx.library.unload(slot);
            ctx.toast("%s", ctx.library.message());
        }
        break;
    case MenuRescan:
        scan(ctx);
        ctx.toast("%d entries", entryCount_);
        break;
    case MenuCreateDir:
        if (ctx.storage.createSampleDir()) {
            ctx.toast("Created PS2DAW/SAMPLES");
            scan(ctx);
        } else {
            ctx.log.error(Subsystem::Storage, "cannot create PS2DAW/SAMPLES (read-only drive?)");
        }
        break;
    }
}

void BrowserView::update(const InputState& in, UiContext& ctx)
{
    rebuildLoadedList(ctx);
    if (ctx.menu.isOpen()) {
        const int r = ctx.menu.update(in);
        if (r != ui::ContextMenu::kNone)
            handleMenu(r, ctx);
        return;
    }

    // A second drive appearing, or the first one coming up after boot, makes
    // the listing stale: rescan once per change, never while idle.
    uint32_t mask = 0;
    for (int i = 0; i < Storage::kMaxRoots; ++i)
        if (ctx.storage.rootReady(i))
            mask |= 1u << i;
    if (mask != lastRootMask_) {
        lastRootMask_ = mask;
        if (source_ == SrcUsb || !scanned_)
            scan(ctx);
    }

    if (in.hit(btn::L2)) {
        source_ = source_ == SrcLoaded ? SrcUsb : SrcLoaded;
        if (source_ == SrcUsb && !scanned_)
            scan(ctx);
    }

    int& sel = sel_[source_];
    const int n = itemCount(ctx);
    if (in.rep(btn::Up) && sel > 0)
        --sel;
    if (in.rep(btn::Down) && sel < n - 1)
        ++sel;
    if (in.rep(btn::Left))
        sel = sel > kVisibleRows ? sel - kVisibleRows : 0;
    if (in.rep(btn::Right))
        sel = sel + kVisibleRows < n ? sel + kVisibleRows : (n > 0 ? n - 1 : 0);
    clampSel(ctx);

    const int channels = ctx.session.project().channelCount;
    if (in.rep(btn::L1))
        ctx.selectedChannel = (ctx.selectedChannel + channels - 1) % channels;
    if (in.rep(btn::R1))
        ctx.selectedChannel = (ctx.selectedChannel + 1) % channels;

    if (in.hit(btn::R2)) {
        openMenu(ctx);
        return;
    }

    if (source_ == SrcUsb) {
        if (in.hit(btn::Circle) && sampleref::parentRel(dirRel_))
            scan(ctx);
        if (sel < entryCount_ && entries_[sel].isDir) {
            if (in.hit(btn::Cross)) {
                char next[sizeof(dirRel_)];
                if (sampleref::joinRel(next, sizeof(next), dirRel_, entries_[sel].name) && sampleref::validRelPath(next)) {
                    str::copy(dirRel_, sizeof(dirRel_), next);
                    scan(ctx);
                } else {
                    ctx.toast("Folder name/path not supported");
                }
            }
            return;
        }
        if (sel < entryCount_ && sampleref::classifyFile(entries_[sel].name, false) == sampleref::FileType::Unsupported) {
            if (in.hit(btn::Cross) || in.hit(btn::Square) || in.hit(btn::Triangle))
                ctx.toast("Unsupported file type (WAV or ADP only)");
            return;
        }
    }

    if (in.hit(btn::Cross))
        act(ctx, (int)SampleLibrary::Action::Assign);
    if (in.hit(btn::Square))
        act(ctx, (int)SampleLibrary::Action::Preview);
    if (in.hit(btn::Triangle))
        act(ctx, (int)SampleLibrary::Action::PreviewSpu2);
}

void BrowserView::describe(const UiContext& ctx, char lines[3][80]) const
{
    lines[0][0] = lines[1][0] = lines[2][0] = '\0';
    const int slot = selectedSlot(ctx);
    const Sample* s = ctx.bank.get(slot);
    if (s) {
        char sz[16];
        const bool res = ctx.audio.resident(slot);
        if (s->hwOnly) {
            sizeText(sz, sizeof(sz), s->fileBytes);
            snprintf(lines[0], 80, "%s  ADP file %s  SPU2 only", s->name, sz);
            snprintf(lines[1], 80, "RAM: 0 KB in EE   SPU2: %s", res ? "resident" : "not resident");
        } else {
            const unsigned long ms = (unsigned long)((uint64_t)s->frames * 1000u / s->sampleRate);
            snprintf(lines[0], 80, "%s  %lu Hz %s  %lu.%02lu s", s->name, (unsigned long)s->sampleRate,
                     s->channels == 2 ? "stereo" : "mono", ms / 1000, (ms % 1000) / 10);
            snprintf(lines[1], 80, "RAM %lu KB  Software: yes  SPU2: %s", (unsigned long)((s->pcmBytes + 1023) / 1024),
                     s->channels != 1 ? "stereo (n/a)" : (res ? "resident" : "not uploaded"));
        }
        return;
    }
    if (source_ == SrcUsb && sel_[SrcUsb] < entryCount_) {
        const Storage::DirEntry& e = entries_[sel_[SrcUsb]];
        const sampleref::FileType t = sampleref::classifyFile(e.name, e.isDir);
        char sz[16];
        sizeText(sz, sizeof(sz), e.size);
        if (t == sampleref::FileType::Dir)
            snprintf(lines[0], 80, "Folder %s", e.name);
        else if (t == sampleref::FileType::Unsupported)
            snprintf(lines[0], 80, "%s  %s  unsupported type", e.name, sz);
        else
            snprintf(lines[0], 80, "%s  %s on disk, not loaded", e.name, sz);
        if (t == sampleref::FileType::Wav)
            snprintf(lines[1], 80, "Max %lu MB file; ~%lu KB RAM when loaded", (unsigned long)(3), (unsigned long)((e.size + 1023) / 1024));
        else if (t == sampleref::FileType::Adp)
            snprintf(lines[1], 80, "Loads straight into SPU2 RAM (no software path)");
    }
}

void BrowserView::draw(Gfx& g, UiContext& ctx)
{
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, w, kViewBottom - kViewTop, "SAMPLE BROWSER");

    // Source tabs and target channel.
    const ChannelData& c = ctx.session.project().channels[ctx.selectedChannel];
    g.text(x + 8, kViewTop + 28, "LOADED", source_ == SrcLoaded ? theme::kAccent : theme::kTextDim, 1, 2);
    g.text(x + 62, kViewTop + 28, "USB", source_ == SrcUsb ? theme::kAccent : theme::kTextDim, 1, 2);
    g.textf(x + 96, kViewTop + 28, theme::kTextDim, "-> CH%d %s", ctx.selectedChannel + 1, c.name);
    if (source_ == SrcUsb)
        g.textf(x + 280, kViewTop + 28, theme::kText, "%s/PS2DAW/SAMPLES%s%s", ctx.storage.ready() ? ctx.storage.rootName() : "----",
                dirRel_[0] ? "/" : "", dirRel_);

    const int n = itemCount(ctx);
    const int first = first_[source_], sel = sel_[source_];
    if (source_ == SrcUsb && (scanFailed_ || n == 0)) {
        const char* msg = !ctx.storage.available()  ? "USB drivers are not running (SELECT held at boot?)"
                          : !ctx.storage.ready()    ? "No USB drive detected yet..."
                          : scanFailed_             ? "No PS2DAW/SAMPLES folder on the drive.  R2 > Create folder"
                                                    : "Folder is empty";
        g.text(x + 16, kListY + 8, msg, theme::kWarning, 1, 2);
    }

    for (int r = 0; r < kVisibleRows; ++r) {
        const int i = first + r;
        if (i >= n)
            break;
        const int y = kListY + r * kRowH;
        ui::listRow(g, x + 6, y, w - 30, kRowH - 2, i == sel);
        const uint32_t col = i == sel ? theme::kText : theme::kTextDim;
        if (source_ == SrcLoaded) {
            const int slot = loadedSlots_[i];
            const Sample* s = ctx.bank.get(slot);
            if (!s)
                continue;
            char dur[16];
            if (s->hwOnly)
                snprintf(dur, sizeof(dur), "SPU2");
            else
                snprintf(dur, sizeof(dur), "%lums", (unsigned long)((uint64_t)s->frames * 1000u / s->sampleRate));
            g.textf(x + 16, y + 3, col, "%-3s %-12.12s %7s %s%s", s->builtin ? "KIT" : (s->hwOnly ? "ADP" : "WAV"), s->name, dur,
                    s->hwOnly ? "" : (s->channels == 2 ? "ST" : "MO"), ctx.audio.resident(slot) ? " SPU" : "");
            if (c.sampleSlot == slot)
                g.text(x + w - 60, y + 3, G_NOTE, theme::kAccent);
        } else {
            const Storage::DirEntry& e = entries_[i];
            const sampleref::FileType t = sampleref::classifyFile(e.name, e.isDir);
            const bool dim = t == sampleref::FileType::Unsupported;
            char sz[16] = "";
            if (t != sampleref::FileType::Dir)
                sizeText(sz, sizeof(sz), e.size);
            g.textf(x + 16, y + 3, dim ? theme::kBorder : col, "%-3s %-24.24s %8s", sampleref::typeName(t), e.name, sz);
            char rel[96], ref[sampleref::kMaxRef];
            if (t != sampleref::FileType::Dir && !dim && sampleref::joinRel(rel, sizeof(rel), dirRel_, e.name) &&
                sampleref::make(ref, sizeof(ref), rel)) {
                const int slot = ctx.bank.findByRef(ref);
                if (slot >= 0) {
                    g.text(x + w - 112, y + 3, "RAM", theme::kAlive);
                    if (c.sampleSlot == slot)
                        g.text(x + w - 60, y + 3, G_NOTE, theme::kAccent);
                }
            }
        }
    }
    ui::scrollbar(g, x + w - 18, kListY, 8, kVisibleRows * kRowH, first, kVisibleRows, n);

    // Details of the selection.
    char lines[3][80];
    describe(ctx, lines);
    const int iy = kListY + kVisibleRows * kRowH + 4;
    g.text(x + 8, iy, lines[0], theme::kText, 1, 2);
    g.text(x + 8, iy + 14, lines[1], theme::kTextDim, 1, 2);

    // Loader activity (sticky result otherwise) and memory budget.
    const SampleLibrary& lib = ctx.library;
    const int fy = kViewBottom - 34;
    if (lib.busy()) {
        const int pct = lib.progressPercent();
        g.textf(x + 8, fy, theme::kWarning, "LOADING %s %d%%%s", sampleref::baseName(lib.currentName()), pct,
                lib.pending() > 1 ? " (+queue)" : "");
        g.fillRect(x + 360, fy + 2, 200, 8, theme::kCellOffAlt);
        g.fillRect(x + 360, fy + 2, 2 * pct, 8, theme::kAccent);
    } else {
        g.text(x + 8, fy, lib.message(), theme::kTextDim, 1, 2);
    }
    g.textf(x + 8, fy + 14, theme::kTextDim, "PCM %lu/%lu KB  SPU2 %lu KB  slots %d/%d  miss %d",
            (unsigned long)(ctx.bank.externalBytesUsed() / 1024), (unsigned long)(SampleBank::kMaxExternalBytes / 1024),
            (unsigned long)(ctx.audio.stats().spuBytes / 1024), ctx.bank.liveCount(), cfg::kMaxSamples, lib.missingCount());
    if (truncated_ && source_ == SrcUsb)
        g.textf(x + w - 150, kListY - 14, theme::kWarning, "first %d shown", kMaxEntries);
}
