#include "ui/debug_overlay.hpp"

#include <stdio.h>

#include "platform/ps2_system.hpp"
#include "ui/theme.hpp"
#include "version.hpp"

namespace ui {

void drawDebugOverlay(Gfx& g, const UiContext& ctx, uint32_t fpsTimes10, uint32_t frameUs)
{
    const int x = 300, y = kViewTop, w = theme::kSafeRight - x, lh = 16;
    g.fillRect(x, y, w, kViewBottom - y, 0x0b0c0e, 0x7a); // ~95% opaque: must stay readable over the grid
    g.frameRect(x, y, w, kViewBottom - y, theme::kAccent, 2);

    const EngineStatus& es = ctx.engine.status();
    const Ps2Audio::Stats& as = ctx.audio.stats();
    const int lat = ctx.audio.latencyFrames();
    const uint32_t blockUs = Ps2Audio::kBlockFrames * 1000000u / cfg::kSampleRate;
    static const char* const kTransport[] = {"STOP", "PLAY", "PAUSE"};

    char lines[20][56];
    int n = 0;
    snprintf(lines[n++], 56, PS2DAW_NAME " " PS2DAW_VERSION);
    snprintf(lines[n++], 56, "built " PS2DAW_BUILD_DATE);
    snprintf(lines[n++], 56, "FPS %lu.%lu  UI frame %lu us", (unsigned long)(fpsTimes10 / 10), (unsigned long)(fpsTimes10 % 10),
             (unsigned long)frameUs);
    snprintf(lines[n++], 56, "%s  %lu.%02lu BPM  pat %d", kTransport[es.transport % 3], (unsigned long)(es.bpmCenti / 100),
             (unsigned long)(es.bpmCenti % 100), es.pattern + 1);
    snprintf(lines[n++], 56, "audio irx %s init %s stream %s", as.driverLoaded ? "OK" : "--", as.initialized ? "OK" : "--",
             as.streaming ? "OK" : "--");
    snprintf(lines[n++], 56, "queue %lu fr (%lu ms) target %d", (unsigned long)as.queuedFrames,
             (unsigned long)(as.queuedFrames * 1000u / cfg::kSampleRate), lat);
    snprintf(lines[n++], 56, "underruns %lu  rpc err %lu  blocks %lu", (unsigned long)as.underruns, (unsigned long)as.rpcErrors,
             (unsigned long)as.blocks);
    snprintf(lines[n++], 56, "render avg %lu max %lu us (%lu%%)", (unsigned long)as.renderUsAvg, (unsigned long)as.renderUsMax,
             (unsigned long)(as.renderUsAvg * 100u / blockUs));
    snprintf(lines[n++], 56, "SW voices %lu/%d  steals %lu", (unsigned long)es.voicesActive, cfg::kMaxVoices, (unsigned long)es.voiceSteals);
    snprintf(lines[n++], 56, "SPU2 notes %lu  late max %lu us  drop %lu", (unsigned long)as.hwPlayed, (unsigned long)as.hwLateUs,
             (unsigned long)es.hwDropped);
    snprintf(lines[n++], 56, "samples %d (%lu KiB)  SPU2 %d (%lu KiB)", ctx.bank.count(), (unsigned long)(ctx.bank.bytesUsed() / 1024),
             as.spuSounds, (unsigned long)(as.spuBytes / 1024));
    snprintf(lines[n++], 56, "clipped samples %lu  cmds %lu", (unsigned long)es.clipSamples, (unsigned long)es.commands);
    snprintf(lines[n++], 56, "EE heap in use %lu KiB", (unsigned long)(ps2sys::heapUsed() / 1024));
    snprintf(lines[n++], 56, "storage %s  dropped cmds %lu", ctx.storage.rootName(), (unsigned long)ctx.session.droppedCommands());
    snprintf(lines[n++], 56, "audio err %ld: %.30s", (long)as.lastError, as.lastError ? Ps2Audio::errorText(as.lastError) : "none");

    int ty = y + 6;
    for (int i = 0; i < n; ++i, ty += lh)
        g.text(x + 8, ty, lines[i], i < 2 ? theme::kAccent : theme::kText, 1, 2);

    // IOP module table, compact: name + OK/ERR.
    int mx = x + 8;
    for (int i = 0; i < ps2sys::moduleCount(); ++i) {
        const ps2sys::ModuleRecord& m = ps2sys::module(i);
        char buf[24];
        snprintf(buf, sizeof(buf), "%s%s ", m.name, m.ok ? "" : "!");
        const int wpx = Gfx::textWidth(buf, 1);
        if (mx + wpx > x + w - 4) {
            mx = x + 8;
            ty += lh;
        }
        g.text(mx, ty, buf, m.ok ? theme::kTextDim : theme::kError, 1, 2);
        mx += wpx;
    }
}

} // namespace ui
