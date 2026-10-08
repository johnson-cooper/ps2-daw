#include "ui/project_view.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/drum_synth.hpp"
#include "core/strutil.hpp"
#include "project/project_io.hpp"
#include "project/slot_store.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kRowH = 17;
constexpr int kLeftX = theme::kSafeLeft + 8;
constexpr int kListY = kViewTop + 28;
const int kLengths[] = {8, 12, 16, 24, 32, 48, 64};

// Shared I/O buffer for project files (64 KiB, static: no large stack use).
uint8_t g_fileBuf[projectio::kMaxFileBytes];
uint8_t g_verifyBuf[projectio::kMaxFileBytes];

const char kNameChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";

class StorageFiles : public ProjectFiles {
public:
    explicit StorageFiles(Storage& s) : st_(s) {}
    int readAll(const char* path, uint8_t* buf, size_t cap) override { return st_.readFile(path, buf, cap); }
    bool writeAll(const char* path, const uint8_t* d, size_t n) override { return st_.writeFile(path, d, n); }
    bool renameTo(const char* a, const char* b) override { return st_.renameFile(a, b); }
    bool removeFile(const char* p) override { return st_.removeFile(p); }
    bool exists(const char* p) override { return st_.fileExists(p); }

private:
    Storage& st_;
};
} // namespace

const char* ProjectView::hint() const
{
    return G_UP G_DOWN " SELECT  " G_LEFT G_RIGHT " CHANGE  L1/R1 FINE  " G_CROSS " RUN";
}

void ProjectView::adjust(int row, int dir, bool fine, UiContext& ctx)
{
    Session& s = ctx.session;
    const Project& p = s.project();
    switch (row) {
    case RowTempo:
        s.setBpmCenti((int)p.bpmCenti + dir * (fine ? 10 : 100));
        break;
    case RowSwing:
        s.setSwing((int)p.swing + dir * (fine ? 1 : 5));
        break;
    case RowMetronome:
        s.setMetronome(!s.metronome());
        break;
    case RowPattern:
        s.selectPattern((p.currentPattern + dir + cfg::kMaxPatterns) % cfg::kMaxPatterns);
        break;
    case RowLength: {
        int idx = 0;
        for (int i = 0; i < 7; ++i)
            if (kLengths[i] <= p.pattern().length)
                idx = i;
        idx = idx + dir < 0 ? 0 : (idx + dir > 6 ? 6 : idx + dir);
        s.setPatternLength(p.currentPattern, kLengths[idx]);
        break;
    }
    case RowMaster:
        s.setMasterVolume(p.masterVolume + dir * (fine ? 1 : 5));
        break;
    case RowLatency:
        ctx.audio.setLatencyFrames(ctx.audio.latencyFrames() + dir * (fine ? 128 : 512));
        break;
    case RowSlot:
        slot_ = slot_ + dir < 1 ? 1 : (slot_ + dir > slotstore::kSlots ? slotstore::kSlots : slot_ + dir);
        confirmLoad_ = false;
        break;
    default:
        break;
    }
}

bool ProjectView::modified(UiContext& ctx)
{
    const size_t n = projectio::save(ctx.session.project(), g_fileBuf, sizeof(g_fileBuf));
    if (n == 0)
        return true;
    const uint32_t crc = projectio::crc32(g_fileBuf, n);
    if (!haveSavedCrc_) { // first look: the song as booted counts as saved
        haveSavedCrc_ = true;
        savedCrc_ = crc;
    }
    return crc != savedCrc_;
}

void ProjectView::onEnter(UiContext&)
{
    slotInfoFor_ = 0;       // re-read the slot and autosave information
    autoInfoValid_ = false;
}

void ProjectView::refreshSlotInfo(UiContext& ctx)
{
    slotInfoFor_ = slot_;
    memset(&slotInfo_, 0, sizeof(slotInfo_));
    memset(&autoInfo_, 0, sizeof(autoInfo_));
    autoInfoValid_ = true;
    char dir[64];
    if (!ctx.storage.ready() || !ctx.storage.appPath(dir, sizeof(dir), ""))
        return;
    StorageFiles fs(ctx.storage);
    slotInfo_ = slotstore::peek(fs, dir, slot_, g_fileBuf, sizeof(g_fileBuf));
    autoInfo_ = slotstore::peek(fs, dir, slotstore::kAutosaveSlot, g_fileBuf, sizeof(g_fileBuf));
}

void ProjectView::exportWav(UiContext& ctx)
{
    Storage& st = ctx.storage;
    char dir[64];
    if (!st.ready() || !st.appPath(dir, sizeof(dir), "")) {
        ctx.log.error(Subsystem::Project, "export: no USB drive ready");
        ctx.toast("Export needs a USB drive");
        return;
    }
    char exportDir[96], path[160];
    snprintf(exportDir, sizeof(exportDir), "%s/EXPORT", dir);
    if (!st.ensureDir(dir) || !st.ensureDir(exportDir)) {
        ctx.log.error(Subsystem::Project, "export: cannot create PS2DAW/EXPORT");
        ctx.toast("Cannot create the EXPORT folder on the drive");
        return;
    }
    // File name from the project name: A-Z 0-9 _ - only, at most 20 characters.
    char base[24];
    int n = 0;
    for (const char* c = ctx.session.project().name; *c && n < 20; ++c) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
            base[n++] = ch;
        else if (ch == ' ' && n > 0 && base[n - 1] != '_')
            base[n++] = '_';
    }
    while (n > 0 && base[n - 1] == '_')
        --n;
    if (n == 0)
        snprintf(base, sizeof(base), "SONG");
    else
        base[n] = '\0';
    snprintf(path, sizeof(path), "%s/%s.WAV", exportDir, base);
    char err[96];
    const Exporter::Source src = ctx.session.project().songBars() > 0 ? Exporter::Source::Song : Exporter::Source::Pattern;
    if (!ctx.exporter.begin(path, src, err, sizeof(err))) {
        ctx.log.error(Subsystem::Project, "export: %s", err);
        ctx.toast("Export failed: %s", err);
        return;
    }
    ctx.toast("Rendering %s...", src == Exporter::Source::Song ? "the song" : "the pattern");
}

void ProjectView::save(UiContext& ctx)
{
    Storage& st = ctx.storage;
    char dir[64];
    if (!st.ready() || !st.appPath(dir, sizeof(dir), "")) {
        ctx.log.error(Subsystem::Project, "save: no USB drive ready");
        return;
    }
    st.ensureDir(dir);
    StorageFiles fs(st);
    char err[64];
    if (!slotstore::save(fs, dir, slot_, ctx.session.project(), g_fileBuf, g_verifyBuf, sizeof(g_fileBuf), err, sizeof(err))) {
        ctx.log.error(Subsystem::Project, "save SLOT%d: %s", slot_, err);
        return;
    }
    modified(ctx); // make sure the baseline exists, then adopt the saved state
    const size_t n = projectio::save(ctx.session.project(), g_fileBuf, sizeof(g_fileBuf));
    savedCrc_ = projectio::crc32(g_fileBuf, n);
    ctx.log.set(Subsystem::Project, Health::Ok, "saved SLOT%d (%u bytes)", slot_, (unsigned)n);
    ctx.toast("Saved SLOT%d", slot_);
    refreshSlotInfo(ctx);
}

void ProjectView::load(UiContext& ctx, int slot)
{
    Storage& st = ctx.storage;
    char dir[64];
    if (!st.ready() || !st.appPath(dir, sizeof(dir), "")) {
        ctx.log.error(Subsystem::Project, "load: no USB drive ready");
        return;
    }
    if (modified(ctx) && !confirmLoad_) {
        confirmLoad_ = true;
        ctx.toast("Unsaved changes will be lost. Press " G_CROSS " again to load %s", slot == slotstore::kAutosaveSlot ? "the autosave" : "this slot");
        return;
    }
    confirmLoad_ = false;
    StorageFiles fs(st);
    static Project loaded; // ~6 KiB; static keeps it off the stack
    char err[64];
    bool backup = false;
    if (!slotstore::load(fs, dir, slot, loaded, g_fileBuf, sizeof(g_fileBuf), &backup, err, sizeof(err))) {
        ctx.log.error(Subsystem::Project, "%s: %s", slot == slotstore::kAutosaveSlot ? "autosave" : "slot", err);
        return;
    }
    ctx.session.loadProject(loaded);
    const size_t n = projectio::save(ctx.session.project(), g_fileBuf, sizeof(g_fileBuf));
    savedCrc_ = projectio::crc32(g_fileBuf, n);
    haveSavedCrc_ = true;
    if (backup) {
        if (slot == slotstore::kAutosaveSlot) {
            ctx.log.set(Subsystem::Project, Health::Warning, "autosave interrupted: loaded its .BAK");
            ctx.toast("The autosave was interrupted: recovered the previous autosave");
        } else {
            ctx.log.set(Subsystem::Project, Health::Warning, "SLOT%d damaged: loaded .BAK", slot);
            ctx.toast("SLOT%d was damaged: recovered the previous save", slot);
        }
    } else if (slot == slotstore::kAutosaveSlot) {
        ctx.log.set(Subsystem::Project, Health::Ok, "recovered the autosave");
        ctx.toast("Recovered the autosave. Save it into a slot to keep it.");
    } else {
        ctx.log.set(Subsystem::Project, Health::Ok, "loaded SLOT%d", slot);
        ctx.toast("Loaded SLOT%d (samples load in the background)", slot);
    }
}

void ProjectView::editName(const InputState& in, UiContext& ctx)
{
    Session& s = ctx.session;
    char name[sizeof(s.project().name)];
    str::copy(name, sizeof(name), s.project().name);
    int len = (int)strlen(name);
    if (nameCursor_ > len)
        nameCursor_ = len;
    const int kChars = (int)sizeof(kNameChars) - 1;
    if (in.rep(btn::Left) && nameCursor_ > 0)
        --nameCursor_;
    if (in.rep(btn::Right) && nameCursor_ < (int)sizeof(name) - 2)
        ++nameCursor_;
    if (in.rep(btn::Up) || in.rep(btn::Down)) {
        while (len <= nameCursor_ && len < (int)sizeof(name) - 1)
            name[len++] = ' ';
        name[len] = '\0';
        int idx = 0;
        for (int i = 0; i < kChars; ++i)
            if (kNameChars[i] == name[nameCursor_])
                idx = i;
        idx = (idx + (in.rep(btn::Up) ? 1 : kChars - 1)) % kChars;
        name[nameCursor_] = kNameChars[idx];
        // Trailing spaces carry no information.
        for (int i = (int)strlen(name) - 1; i > 0 && name[i] == ' ' && i > nameCursor_; --i)
            name[i] = '\0';
        str::copy(s.project().name, sizeof(s.project().name), name);
    }
    if (in.hit(btn::Cross) || in.hit(btn::Circle))
        nameEdit_ = false;
}

void ProjectView::activate(int row, UiContext& ctx)
{
    Session& s = ctx.session;
    const int tone = (int)drumsynth::Kind::TestTone;
    switch (row) {
    case RowMetronome:
        s.setMetronome(!s.metronome());
        ctx.toast(s.metronome() ? "Metronome on (not rendered into exports)" : "Metronome off");
        break;
    case RowName:
        nameEdit_ = true;
        nameCursor_ = 0;
        ctx.toast(G_UP G_DOWN " change letter  " G_LEFT G_RIGHT " move  " G_CROSS " done");
        break;
    case RowMissing:
        if (ctx.library.missingCount() == 0) {
            ctx.toast("No missing samples");
        } else {
            ctx.menu.open("MISSING SAMPLES");
            for (int i = 0; i < ctx.library.missingCount(); ++i)
                ctx.menu.add(900 + i, ctx.library.missingRef(i) + (strncmp(ctx.library.missingRef(i), "samples:", 8) == 0 ? 8 : 0), false);
        }
        break;
    case RowSave:
        save(ctx);
        break;
    case RowLoad:
        load(ctx, slot_);
        break;
    case RowExport:
        exportWav(ctx);
        break;
    case RowRecover:
        if (autoInfoValid_ && autoInfo_.exists && !autoInfo_.corrupt)
            load(ctx, slotstore::kAutosaveSlot);
        else
            ctx.toast("No autosave on the drive");
        break;
    case RowNew:
        if (!confirmNew_) {
            confirmNew_ = true;
            ctx.toast("Press " G_CROSS " again to replace the song with the demo");
        } else {
            confirmNew_ = false;
            static Project fresh;
            fresh.resetDemo();
            s.loadProject(fresh);
            haveSavedCrc_ = false; // the new demo is the baseline
            ctx.toast("New demo project");
        }
        break;
    case RowToneSw:
        s.previewSample(tone, VoiceMode::Software);
        ctx.toast("1 kHz tone via PCM stream (software mixer)");
        break;
    case RowToneSpu:
        if (ctx.audio.stats().spuSounds == 0) {
            ctx.toast("SPU2 voices unavailable: see status table");
        } else {
            s.previewSample(tone, VoiceMode::Spu2);
            ctx.toast("1 kHz tone via SPU2 hardware voice 23");
        }
        break;
    case RowOverlay:
        ctx.debugOverlay = !ctx.debugOverlay;
        break;
    case RowClearError:
        ctx.log.clearLastError();
        break;
    default:
        break;
    }
}

void ProjectView::update(const InputState& in, UiContext& ctx)
{
    if (ctx.menu.isOpen()) {
        ctx.menu.update(in); // informational list (missing samples)
        return;
    }
    if (nameEdit_) {
        editName(in, ctx);
        return;
    }
    if (slotInfoFor_ != slot_ && ctx.storage.ready())
        refreshSlotInfo(ctx);
    if (in.rep(btn::Up)) {
        row_ = (row_ + RowCount - 1) % RowCount;
        confirmNew_ = confirmLoad_ = false;
    }
    if (in.rep(btn::Down)) {
        row_ = (row_ + 1) % RowCount;
        confirmNew_ = confirmLoad_ = false;
    }
    if (in.rep(btn::Left))
        adjust(row_, -1, false, ctx);
    if (in.rep(btn::Right))
        adjust(row_, +1, false, ctx);
    if (in.rep(btn::L1))
        adjust(row_, -1, true, ctx);
    if (in.rep(btn::R1))
        adjust(row_, +1, true, ctx);
    if (in.hit(btn::Cross))
        activate(row_, ctx);
}

void ProjectView::draw(Gfx& g, UiContext& ctx)
{
    const Project& p = ctx.session.project();
    const int x = theme::kSafeLeft, w = theme::kSafeRight - theme::kSafeLeft;
    ui::panel(g, x, kViewTop, 300, kViewBottom - kViewTop, "PROJECT");

    char v[RowCount][28];
    snprintf(v[RowName], 28, "%.23s%s", p.name, nameEdit_ ? "_" : "");
    snprintf(v[RowTempo], 28, "%lu.%02lu BPM", (unsigned long)(p.bpmCenti / 100), (unsigned long)(p.bpmCenti % 100));
    snprintf(v[RowSwing], 28, "%d%%", p.swing);
    snprintf(v[RowMetronome], 28, "%s", ctx.session.metronome() ? "ON" : "off");
    snprintf(v[RowPattern], 28, "%d %.10s", p.currentPattern + 1, p.pattern().name);
    snprintf(v[RowLength], 28, "%d steps", p.pattern().length);
    snprintf(v[RowMaster], 28, "%d%%", p.masterVolume);
    const int lat = ctx.audio.latencyFrames();
    snprintf(v[RowLatency], 28, "%d fr %d ms", lat, lat * 1000 / cfg::kSampleRate);
    if (!ctx.storage.ready())
        snprintf(v[RowSlot], 28, "%d", slot_);
    else if (!slotInfo_.exists)
        snprintf(v[RowSlot], 28, "%d (empty)", slot_);
    else if (slotInfo_.corrupt)
        snprintf(v[RowSlot], 28, "%d DAMAGED", slot_);
    else
        snprintf(v[RowSlot], 28, "%d %.11s%s", slot_, slotInfo_.name, slotInfo_.fromBackup ? " BAK" : "");
    if (ctx.library.missingCount())
        snprintf(v[RowMissing], 28, "%d (%s)", ctx.library.missingCount(), G_CROSS);
    else
        snprintf(v[RowMissing], 28, "none");
    snprintf(v[RowSave], 28, "%s", ctx.storage.ready() ? G_CROSS : "no USB");
    snprintf(v[RowLoad], 28, "%s", !ctx.storage.ready() ? "no USB" : (confirmLoad_ ? "SURE? " G_CROSS : G_CROSS));
    snprintf(v[RowNew], 28, "%s", confirmNew_ ? "SURE? " G_CROSS : G_CROSS);
    snprintf(v[RowExport], 28, "%s", !ctx.storage.ready() ? "no USB" : (p.songBars() > 0 ? "song " G_CROSS : "pattern " G_CROSS));
    if (!ctx.storage.ready())
        snprintf(v[RowRecover], 28, "no USB");
    else if (autoInfoValid_ && autoInfo_.exists && !autoInfo_.corrupt)
        snprintf(v[RowRecover], 28, "%.11s " G_CROSS, autoInfo_.name);
    else
        snprintf(v[RowRecover], 28, "none");
    snprintf(v[RowToneSw], 28, G_CROSS);
    snprintf(v[RowToneSpu], 28, "%s", ctx.audio.stats().spuSounds ? G_CROSS : "n/a");
    snprintf(v[RowOverlay], 28, "%s", ctx.debugOverlay ? "ON" : "off");
    snprintf(v[RowClearError], 28, "%s", ctx.log.hasError() ? G_CROSS : "-");

    static const char* const kNames[RowCount] = {
        "Name", "Tempo", "Swing", "Metronome", "Pattern", "Length", "Master vol", "Latency", "File slot", "Missing samples", "Save to USB", "Load from USB",
        "New (demo)", "Export WAV", "Recover autosave", "Test tone SW", "Test tone SPU2", "Debug overlay", "Clear error",
    };
    constexpr int kVisible = 17;
    if (row_ < scroll_)
        scroll_ = row_;
    if (row_ >= scroll_ + kVisible)
        scroll_ = row_ - kVisible + 1;
    for (int r = scroll_; r < RowCount && r < scroll_ + kVisible; ++r) {
        const int y = kListY + (r - scroll_) * kRowH;
        ui::listRow(g, x + 4, y, 292, kRowH - 1, r == row_);
        g.text(kLeftX + 6, y + 1, kNames[r], r == row_ ? theme::kText : theme::kTextDim, 1, 2);
        g.text(kLeftX + 138, y + 1, v[r], r == row_ ? theme::kAccent : theme::kText, 1, 2);
    }

    // Health table (same data as the boot screen) so failures stay visible.
    const int hx = x + 308, hw = w - 308;
    ui::panel(g, hx, kViewTop, hw, kViewBottom - kViewTop, "SYSTEM STATUS");
    for (int i = 0; i < (int)Subsystem::Count; ++i) {
        const StatusLog::Entry& e = ctx.log.entry((Subsystem)i);
        const int y = kViewTop + 26 + i * 30; // 10 rows x 30 px fits the view exactly
        uint32_t c = theme::kTextDim;
        if (e.health == Health::Ok)
            c = theme::kOk;
        else if (e.health == Health::Failed)
            c = theme::kError;
        else if (e.health == Health::Warning)
            c = theme::kWarning;
        g.text(hx + 6, y, StatusLog::healthName(e.health), c, 1, 2);
        g.text(hx + 6 + 30, y, StatusLog::name((Subsystem)i), theme::kText, 1, 2);
        char detail[48];
        snprintf(detail, sizeof(detail), "%.46s", e.detail);
        g.text(hx + 6, y + 14, detail, theme::kTextDim, 1, 2);
    }
}
