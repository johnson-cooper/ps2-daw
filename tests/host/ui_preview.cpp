// Host-side UI preview: compiles the real views against a software
// rasterizer implementation of Gfx and stub platform services, then writes
// one image per screen. Layout check only; the PS2 build never uses this.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "audio/drum_synth.hpp"
#include "platform/ps2_audio.hpp"
#define private public // preview-only: poke storage state
#include "platform/ps2_filesystem.hpp"
#undef private
#include "platform/ps2_graphics.hpp"
#include "platform/ps2_input.hpp"
#include "platform/ps2_system.hpp"
#include "project/sample_library.hpp"
#include "project/session.hpp"
#include "ui/browser.hpp"
#include "ui/channel_rack.hpp"
#include "ui/instrument_view.hpp"
#include "ui/waveform.hpp"
#include "audio/wav_export.hpp"
#include "ui/debug_overlay.hpp"
#include "ui/font.hpp"
#include "ui/mixer_view.hpp"
#include "ui/piano_roll.hpp"
#include "ui/playlist_view.hpp"
#include "ui/project_view.hpp"
#include "ui/theme.hpp"

// ---- software Gfx -----------------------------------------------------------
static uint32_t g_fb[Gfx::kWidth * Gfx::kHeight];

bool Gfx::init(StatusLog&) { return true; }
void Gfx::beginFrame(uint32_t rgb)
{
    for (auto& p : g_fb)
        p = rgb;
}
void Gfx::endFrame() { ++frames_; }
void Gfx::fillRect(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha)
{
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || yy < 0 || xx >= kWidth || yy >= kHeight)
                continue;
            uint32_t& d = g_fb[yy * kWidth + xx];
            if (alpha >= 0x80) {
                d = rgb;
                continue;
            }
            uint32_t out = 0;
            for (int s = 0; s < 24; s += 8) {
                const int cs = (rgb >> s) & 0xff, cd = (d >> s) & 0xff;
                out |= (uint32_t)(cd + (cs - cd) * alpha / 128) << s;
            }
            d = out;
        }
}
void Gfx::frameRect(int x, int y, int w, int h, uint32_t rgb, int t)
{
    fillRect(x, y, w, t, rgb);
    fillRect(x, y + h - t, w, t, rgb);
    fillRect(x, y + t, t, h - 2 * t, rgb);
    fillRect(x + w - t, y + t, t, h - 2 * t, rgb);
}
int Gfx::textWidth(const char* s, int sx) { return (int)strlen(s) * font::kCellW * sx; }
int Gfx::text(int x, int y, const char* s, uint32_t rgb, int sx, int sy)
{
    int px = x;
    for (const unsigned char* c = (const unsigned char*)s; *c; ++c) {
        const int g = (int)*c - font::kFirstCode;
        if (g > 0 && g < font::kGlyphCount)
            for (int r = 0; r < font::kGlyphRows; ++r)
                for (int col = 0; col < font::kGlyphCols; ++col)
                    if (font::kGlyphs[g][r] & (0x10 >> col))
                        fillRect(px + col * sx, y + r * sy, sx, sy, rgb);
        px += font::kCellW * sx;
    }
    return px - x;
}
int Gfx::textf(int x, int y, uint32_t rgb, const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text(x, y, buf, rgb);
}

// ---- platform stubs -----------------------------------------------------------
const char* Ps2Audio::errorText(int) { return "stub"; }
void Ps2Audio::setLatencyFrames(int f) { latency_ = f; }
const char* Storage::rootName() const { return readyRoot_ >= 0 ? "mass0:" : "none"; }
bool Storage::appPath(char*, size_t, const char*) const { return false; }
int Storage::readFile(const char*, uint8_t*, size_t) const { return -1; }
bool Storage::writeFile(const char*, const uint8_t*, size_t) const { return false; }
bool Storage::ensureDir(const char*) const { return false; }
bool Ps2Audio::uploadPcm(int, const Sample&, char*, size_t) { return false; }
bool Ps2Audio::uploadApcm(int, const uint8_t*, uint32_t, char*, size_t) { return false; }
void Ps2Audio::unload(int) {}
bool Ps2Audio::resident(int slot) const { return slot >= 0 && slot < 8; }
bool Storage::createSampleDir() const { return false; }
bool Storage::fileExists(const char*) const { return false; }
bool Storage::removeFile(const char*) const { return false; }
bool Storage::renameFile(const char*, const char*) const { return false; }
int Storage::openSample(const char*, uint32_t*) { return -1; }
int Storage::read(int, uint8_t*, uint32_t) { return -1; }
void Storage::close(int) {}
int Storage::listSamples(const char* rel, DirEntry* out, int max, bool* truncated) const
{
    if (truncated)
        *truncated = false;
    if (rel && *rel)
        return 0;
    static const struct { const char* n; uint32_t sz; bool dir; } e[] = {
        {"808", 0, true},        {"DRUMS", 0, true},          {"LOOPS", 0, true},        {"AMEN BREAK.WAV", 1240000, false},
        {"CLAP.WAV", 31000, false}, {"KICK_LONG.WAV", 212000, false}, {"PAD.ADP", 96000, false}, {"README.TXT", 400, false}};
    int n = 0;
    for (const auto& x : e) {
        if (n >= max)
            break;
        snprintf(out[n].name, sizeof(out[n].name), "%s", x.n);
        out[n].size = x.sz;
        out[n].isDir = x.dir;
        ++n;
    }
    return n;
}
namespace ps2sys {
static ModuleRecord g_mods[] = {{"sio2man", 1, 0, true}, {"padman", 2, 0, true}, {"libsd", 3, 0, true}, {"audio", 4, 0, true},
                                {"iomanX", 5, 0, true}, {"fileXio", 6, 0, true}, {"usbd", 7, 0, true}, {"bdm", 8, 0, true},
                                {"bdmfs_fatfs", 9, 0, true}, {"usbmass_bd", 10, 0, true}};
int moduleCount() { return 10; }
const ModuleRecord& module(int i) { return g_mods[i]; }
uint32_t heapUsed() { return 3 * 1024 * 1024; }
uint64_t timeUs() { return 0; }
void sleepUs(int) {}
} // namespace ps2sys

// ---- PNG writer (stored zlib blocks, no dependencies) ----------------------
static uint32_t crcTable[256];
static uint32_t crc(const uint8_t* d, size_t n, uint32_t c = 0xffffffffu)
{
    for (size_t i = 0; i < n; ++i)
        c = crcTable[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8)
        v.push_back((uint8_t)(x >> s));
}
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> v;
    be32(v, (uint32_t)data.size());
    v.insert(v.end(), type, type + 4);
    v.insert(v.end(), data.begin(), data.end());
    be32(v, ~crc(v.data() + 4, v.size() - 4));
    fwrite(v.data(), 1, v.size(), f);
}
static void writePng(const char* path)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        crcTable[i] = c;
    }
    // Duplicate rows vertically x1, horizontally x1: 640x448 as on screen.
    std::vector<uint8_t> raw;
    for (int y = 0; y < Gfx::kHeight; ++y) {
        raw.push_back(0);
        for (int x = 0; x < Gfx::kWidth; ++x) {
            const uint32_t p = g_fb[y * Gfx::kWidth + x];
            raw.push_back((uint8_t)(p >> 16));
            raw.push_back((uint8_t)(p >> 8));
            raw.push_back((uint8_t)p);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        const size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    be32(z, (b << 16) | a);
    FILE* f = fopen(path, "wb");
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, Gfx::kWidth);
    be32(ihdr, Gfx::kHeight);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
}

// Minimal copy of the App chrome so previews show the full screen.
static void chrome(Gfx& g, UiContext& ctx, int tab, View& v)
{
    g.fillRect(0, 0, Gfx::kWidth, 44, theme::kPanelDark);
    g.text(theme::kSafeLeft, 18, G_BLOCK " PS2 DAW", theme::kAccent);
    g.text(150, 18, G_PLAY " PLAY", theme::kAlive);
    g.text(250, 18, "120.00 BPM", theme::kText);
    g.text(390, 18, "P1 2.1", theme::kText);
    g.text(540, 18, G_NOTE " OK", theme::kOk);
    const char* labels[] = {"RACK", "ROLL", "INST", "SONG", "MIXER", "BROWSER", "PROJECT"};
    ui::tabBar(g, theme::kSafeLeft, 48, labels, 7, tab);
    v.draw(g, ctx);
    const int y = kViewBottom + 4;
    g.fillRect(0, y, Gfx::kWidth, Gfx::kHeight - y, theme::kPanelDark);
    g.text(theme::kSafeLeft, y + 2, v.hint(), theme::kTextDim, 1, 2);
    g.text(theme::kSafeLeft, y + 18, "START play/pause  L3 stop  R3 debug", theme::kTextDim, 1, 2);
}

static bool noWait() { return false; }

// Export plumbing is not exercised by the layout previews.
struct NullExportFile : ExportFile {
    bool open(const char*) override { return false; }
    bool write(const void*, uint32_t) override { return false; }
    bool rewriteHeader(const uint8_t*, uint32_t) override { return false; }
    bool close() override { return true; }
    bool readHeader(const char*, uint8_t*, uint32_t, uint32_t*) override { return false; }
    bool rename(const char*, const char*) override { return false; }
    void remove(const char*) override {}
};

static void step(View& v, UiContext& ctx, uint32_t buttons, int times = 1)
{
    InputState in;
    in.pressed = in.repeat = in.held = buttons;
    for (int i = 0; i < times; ++i)
        v.update(in, ctx);
}

static void snapshot(Gfx& g, UiContext& ctx, int tab, View& v, const char* dir, const char* name, bool menu = false)
{
    g.beginFrame(theme::kBackground);
    chrome(g, ctx, tab, v);
    if (menu)
        ctx.menu.draw(g);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    writePng(path);
}

int main(int argc, char** argv)
{
    const char* outDir = argc > 1 ? argv[1] : ".";
    static SampleBank bank;
    static AudioEngine engine;
    drumsynth::generateKit(bank);
    engine.setSampleBank(&bank);
    static Session session(engine, bank, noWait);
    session.resync();
    session.setMute(4, true);
    session.setVoiceMode(6, VoiceMode::Spu2);
    session.play();
    int16_t buf[cfg::kMaxBlockFrames * 2];
    for (int i = 0; i < 60; ++i) // ~0.64 s: playhead around beat 2
        engine.render(buf, 512);

    static Ps2Audio audio;
    static Storage storage;
    static StatusLog log;
    static ui::ContextMenu menu;
    Ps2Audio::Stats& st = const_cast<Ps2Audio::Stats&>(audio.stats());
    st.driverLoaded = st.initialized = st.streaming = 1;
    st.heardFrame = engine.status().renderedFrames;
    st.queuedFrames = 2048;
    st.spuSounds = 9;
    st.renderUsAvg = 310;
    st.renderUsMax = 820;
    log.set(Subsystem::Iop, Health::Ok, "IOP reset, LMB patch applied");
    log.set(Subsystem::Graphics, Health::Ok, "NTSC 640x448 interlaced, font in VRAM");
    log.set(Subsystem::Controller, Health::Ok, "DualShock 2, analog locked");
    log.set(Subsystem::AudioIrx, Health::Ok, "libsd.irx + audio.irx loaded");
    log.set(Subsystem::AudioInit, Health::Ok, "lib 1.0.0 drv 1.0.0");
    log.set(Subsystem::AudioStream, Health::Ok, "48000 Hz s16 stereo, 512-frame blocks");
    log.set(Subsystem::Spu2, Health::Ok, "9 sounds, 83 KiB in SPU2 RAM");
    log.set(Subsystem::Samples, Health::Ok, "9 built-in sounds, 281 KiB");
    log.set(Subsystem::Storage, Health::Warning, "no USB drive found (still watching)");
    log.set(Subsystem::Project, Health::Ok, "DEMO BEAT");

    storage.driversOk_ = true;
    storage.rootMask_ = 1;
    storage.readyRoot_ = 0;
    static SampleLibrary library(session, bank, storage, &audio, log);
    {
        int16_t* d = (int16_t*)calloc(2 * 44100, sizeof(int16_t));
        bank.add("CLAP.WAV", d, 44100, 44100, 2, false, "samples:CLAP.WAV", 176444);
    }
    static NullExportFile nullFile;
    static Exporter exporter(engine, session, nullFile, nullptr);
    static WaveformCache waves;
    UiContext ctx{session, engine, audio, storage, log, bank, menu, library, exporter, waves};
    ctx.selectedChannel = 2;
    Gfx g;
    ChannelRackView rack;
    PianoRollView roll;
    InstrumentView instrument;
    PlaylistView playlist;
    MixerView mixer;
    BrowserView browser;
    ProjectView project;
    View* views[] = {&rack, &roll, &instrument, &playlist, &mixer, &browser, &project};
    const char* names[] = {"rack", "roll", "instrument", "playlist", "mixer", "browser", "project"};
    char path[256];
    session.placeClip(0, 0, 0, 2);
    session.placeClip(0, 2, 1, 1);
    session.placeClip(1, 1, 2, 4);
    session.placeClip(2, 4, 3, 2);
    session.placeClip(3, 0, 0, 20); // runs past the visible window
    session.setSongMode(true);
    // A short melody on channel 3 (chord on step 8) plus a long note, for the piano roll preview.
    session.setChannelGate(2, true);
    session.addNote(0, 2, 0, 60, 2, 100);
    session.addNote(0, 2, 2, 63, 2, 90);
    session.addNote(0, 2, 4, 67, 4, 100);
    session.addNote(0, 2, 8, 60, 2, 100);
    session.addNote(0, 2, 8, 64, 2, 100);
    session.addNote(0, 2, 8, 67, 2, 70);
    session.addNote(0, 2, 12, 72, 3, 100);
    session.addNote(0, 2, 13, 55, 1, 60);
    session.ungroupClip(session.project().clipAt(0, 0)); // instruments of pattern 1 get their own clips
    // Mixer content for the new previews: routing, inserts with effects, a synth channel, an audio clip.
    session.setRoute(0, 1);
    session.setRoute(1, 1);
    session.setRoute(2, 2);
    session.setRoute(7, 3);
    session.setMixerTrackName(1, "DRUMS");
    session.setMixerTrackName(3, "BASS");
    session.setFxType(1, 0, FxType::Eq);
    session.setFxType(1, 1, FxType::Compressor);
    session.setFxParam(1, 1, 0, -22);
    session.setFxType(3, 0, FxType::Filter);
    session.setFxType(0, 0, FxType::Reverb);
    session.setFxBypass(0, 0, true);
    session.applySynthPreset(3, 1);
    session.setEnvParam(0, kEnvEnabled, 1);
    session.setEnvParam(0, kEnvAttack, 30);
    session.setEnvParam(0, kEnvDecay, 300);
    session.setEnvParam(0, kEnvSustain, 55);
    session.setEnvParam(0, kEnvRelease, 400);
    session.setPlaylistTrackName(1, "BASS");
    {
        // A 1.5 s synthetic loop with a few hits, as an audio clip with its waveform.
        int16_t* d = (int16_t*)calloc(72000, sizeof(int16_t));
        for (int i = 0; i < 72000; ++i) {
            const int hit = i % 18000;
            d[i] = (int16_t)(hit < 6000 ? (int)(16000.0 * (1.0 - hit / 6000.0)) * ((i / 40) % 2 ? 1 : -1) : 0);
        }
        const int s = bank.add("LOOP.WAV", d, 72000, 48000, 1, false, "samples:LOOP.WAV", 144044);
        session.placeAudioClip(4, 2, s);
        session.placeAudioClip(5, 0, s, 3);
        session.setAudioClipLoop(1, true);
        for (int i = 0; i < 10; ++i)
            waves.tick(bank);
    }
    // keep the audio running so meters show levels
    for (int i = 0; i < 40; ++i)
        engine.render(buf, 512);
    for (int i = 0; i < 7; ++i) {
        g.beginFrame(theme::kBackground);
        chrome(g, ctx, i, *views[i]);
        snprintf(path, sizeof(path), "%s/%s.png", outDir, names[i]);
        writePng(path);
    }
    // ---- instrument: sampler envelope, then the synth ----
    ctx.selectedChannel = 0;
    instrument.onEnter(ctx);
    snapshot(g, ctx, 2, instrument, outDir, "instrument_sampler");
    step(instrument, ctx, btn::Down, 3);
    step(instrument, ctx, btn::Right, 2);
    ctx.selectedChannel = 3;
    instrument.onEnter(ctx);
    snapshot(g, ctx, 2, instrument, outDir, "instrument_synth");
    step(instrument, ctx, btn::Down, 8);
    snapshot(g, ctx, 2, instrument, outDir, "instrument_synth_scrolled");
    step(instrument, ctx, btn::Triangle);
    snapshot(g, ctx, 2, instrument, outDir, "instrument_menu", true);
    menu.close();
    // ---- mixer: channels, inserts, effects editor ----
    ctx.selectedChannel = 2;
    step(mixer, ctx, btn::L2);
    snapshot(g, ctx, 4, mixer, outDir, "mixer_inserts");
    step(mixer, ctx, btn::Circle);
    snapshot(g, ctx, 4, mixer, outDir, "mixer_fx");
    step(mixer, ctx, btn::Right);
    step(mixer, ctx, btn::Down);
    snapshot(g, ctx, 4, mixer, outDir, "mixer_fx_params");
    step(mixer, ctx, btn::Circle);
    step(mixer, ctx, btn::Circle);
    step(mixer, ctx, btn::Triangle);
    snapshot(g, ctx, 4, mixer, outDir, "mixer_menu", true);
    menu.close();
    // ---- playlist with audio clips and the clip editor ----
    step(playlist, ctx, btn::Down, 4);
    step(playlist, ctx, btn::Right, 2);
    step(playlist, ctx, btn::Triangle);
    snapshot(g, ctx, 3, playlist, outDir, "playlist_menu", true);
    menu.close();
    // ---- piano roll in select mode with two notes selected ----
    ctx.selectedChannel = 2;
    roll.onEnter(ctx);
    step(roll, ctx, btn::Triangle);
    step(roll, ctx, btn::Cross);      // menu: Mode -> SELECT
    step(roll, ctx, btn::Cross);      // pick the note under the cursor (step 1, C4)
    step(roll, ctx, btn::Right, 2);
    step(roll, ctx, btn::Up, 3);
    step(roll, ctx, btn::Cross);      // and the Eb4 at step 3
    snapshot(g, ctx, 1, roll, outDir, "roll_select");
    step(roll, ctx, btn::Square);     // grab
    step(roll, ctx, btn::Right, 2);
    step(roll, ctx, btn::Up, 2);
    snapshot(g, ctx, 1, roll, outDir, "roll_grab");
    step(roll, ctx, btn::Cross);      // drop
    step(roll, ctx, btn::Triangle);
    snapshot(g, ctx, 1, roll, outDir, "roll_menu", true);
    menu.close();
    printf("extra previews done\n");
    // Browser on the USB source, and its action menu.
    {
        InputState l2;
        l2.pressed = l2.repeat = btn::L2;
        browser.onEnter(ctx);
        browser.update(l2, ctx);
        InputState dn;
        dn.pressed = dn.repeat = btn::Down;
        for (int i = 0; i < 4; ++i)
            browser.update(dn, ctx);
        g.beginFrame(theme::kBackground);
        chrome(g, ctx, 4, browser);
        snprintf(path, sizeof(path), "%s/browser_usb.png", outDir);
        writePng(path);
        InputState r2;
        r2.pressed = r2.repeat = btn::R2;
        browser.update(r2, ctx);
        g.beginFrame(theme::kBackground);
        chrome(g, ctx, 4, browser);
        menu.draw(g);
        snprintf(path, sizeof(path), "%s/browser_menu.png", outDir);
        writePng(path);
        menu.close();
    }
    // Rack with the context menu open, and with the debug overlay.
    InputState in;
    in.pressed = in.repeat = btn::Triangle;
    rack.update(in, ctx);
    g.beginFrame(theme::kBackground);
    chrome(g, ctx, 0, rack);
    menu.draw(g);
    snprintf(path, sizeof(path), "%s/rack_menu.png", outDir);
    writePng(path);
    menu.close();
    g.beginFrame(theme::kBackground);
    chrome(g, ctx, 0, rack);
    ui::drawDebugOverlay(g, ctx, 599, 4100);
    snprintf(path, sizeof(path), "%s/rack_debug.png", outDir);
    writePng(path);
    printf("wrote previews to %s\n", outDir);
    return 0;
}
