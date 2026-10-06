#include "ui/project_view.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/drum_synth.hpp"
#include "project/project_io.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace {
constexpr int kRowH = 20;
constexpr int kLeftX = theme::kSafeLeft + 8;
constexpr int kListY = kViewTop + 28;
const int kLengths[] = {8, 12, 16, 24, 32, 48, 64};

// Shared I/O buffer for project files (64 KiB, static: no large stack use).
uint8_t g_fileBuf[projectio::kMaxFileBytes];
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
        slot_ = slot_ + dir < 1 ? 1 : (slot_ + dir > 8 ? 8 : slot_ + dir);
        break;
    default:
        break;
    }
}

void ProjectView::save(UiContext& ctx)
{
    Storage& st = ctx.storage;
    char dir[64], path[96], rel[32];
    if (!st.ready() || !st.appPath(dir, sizeof(dir), "")) {
        ctx.log.error(Subsystem::Project, "save: no USB drive ready");
        return;
    }
    st.ensureDir(dir);
    snprintf(rel, sizeof(rel), "SLOT%d.ps2daw", slot_);
    st.appPath(path, sizeof(path), rel);
    const size_t n = projectio::save(ctx.session.project(), g_fileBuf, sizeof(g_fileBuf));
    if (n == 0) {
        ctx.log.error(Subsystem::Project, "save: project too large");
        return;
    }
    if (!st.writeFile(path, g_fileBuf, n)) {
        ctx.log.error(Subsystem::Project, "write failed: %s", path);
        return;
    }
    ctx.log.set(Subsystem::Project, Health::Ok, "saved %s (%u bytes)", path, (unsigned)n);
    ctx.toast("Saved %s", path);
}

void ProjectView::load(UiContext& ctx)
{
    Storage& st = ctx.storage;
    char path[96], rel[32];
    snprintf(rel, sizeof(rel), "SLOT%d.ps2daw", slot_);
    if (!st.ready() || !st.appPath(path, sizeof(path), rel)) {
        ctx.log.error(Subsystem::Project, "load: no USB drive ready");
        return;
    }
    const int n = st.readFile(path, g_fileBuf, sizeof(g_fileBuf));
    if (n == -2) {
        ctx.log.error(Subsystem::Project, "%s is too large", path);
        return;
    }
    if (n < 0) {
        ctx.log.error(Subsystem::Project, "cannot read %s", path);
        return;
    }
    static Project loaded; // ~5 KiB; static keeps it off the stack
    char err[64];
    if (!projectio::load(g_fileBuf, (size_t)n, loaded, err, sizeof(err))) {
        ctx.log.error(Subsystem::Project, "%s: %s", rel, err);
        return;
    }
    ctx.session.loadProject(loaded);
    ctx.log.set(Subsystem::Project, Health::Ok, "loaded %s", path);
    ctx.toast("Loaded %s", path);
}

void ProjectView::activate(int row, UiContext& ctx)
{
    Session& s = ctx.session;
    const int tone = (int)drumsynth::Kind::TestTone;
    switch (row) {
    case RowSave:
        save(ctx);
        break;
    case RowLoad:
        load(ctx);
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
    if (in.rep(btn::Up)) {
        row_ = (row_ + RowCount - 1) % RowCount;
        confirmNew_ = false;
    }
    if (in.rep(btn::Down)) {
        row_ = (row_ + 1) % RowCount;
        confirmNew_ = false;
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
    snprintf(v[RowTempo], 28, "%lu.%02lu BPM", (unsigned long)(p.bpmCenti / 100), (unsigned long)(p.bpmCenti % 100));
    snprintf(v[RowPattern], 28, "%d / %d", p.currentPattern + 1, cfg::kMaxPatterns);
    snprintf(v[RowLength], 28, "%d steps", p.pattern().length);
    snprintf(v[RowMaster], 28, "%d%%", p.masterVolume);
    const int lat = ctx.audio.latencyFrames();
    snprintf(v[RowLatency], 28, "%d fr %d ms", lat, lat * 1000 / cfg::kSampleRate);
    snprintf(v[RowSlot], 28, "SLOT%d", slot_);
    snprintf(v[RowSave], 28, "%s", ctx.storage.ready() ? G_CROSS : "no USB");
    snprintf(v[RowLoad], 28, "%s", ctx.storage.ready() ? G_CROSS : "no USB");
    snprintf(v[RowNew], 28, "%s", confirmNew_ ? "SURE? " G_CROSS : G_CROSS);
    snprintf(v[RowToneSw], 28, G_CROSS);
    snprintf(v[RowToneSpu], 28, "%s", ctx.audio.stats().spuSounds ? G_CROSS : "n/a");
    snprintf(v[RowOverlay], 28, "%s", ctx.debugOverlay ? "ON" : "off");
    snprintf(v[RowClearError], 28, "%s", ctx.log.hasError() ? G_CROSS : "-");

    static const char* const kNames[RowCount] = {
        "Tempo", "Pattern", "Length", "Master vol", "Latency", "File slot", "Save to USB", "Load from USB",
        "New (demo)", "Test tone SW", "Test tone SPU2", "Debug overlay", "Clear error",
    };
    for (int r = 0; r < RowCount; ++r) {
        const int y = kListY + r * kRowH;
        ui::listRow(g, x + 4, y, 292, kRowH - 2, r == row_);
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
