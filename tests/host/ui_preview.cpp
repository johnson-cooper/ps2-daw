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
#include "ui/debug_overlay.hpp"
#include "ui/font.hpp"
#include "ui/mixer_view.hpp"
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
    const char* labels[] = {"RACK", "SONG", "MIXER", "BROWSER", "PROJECT"};
    ui::tabBar(g, theme::kSafeLeft, 48, labels, 5, tab);
    g.text(theme::kSafeRight - Gfx::textWidth("SELECT: VIEW", 1), 50, "SELECT: VIEW", theme::kTextDim, 1, 2);
    v.draw(g, ctx);
    const int y = kViewBottom + 4;
    g.fillRect(0, y, Gfx::kWidth, Gfx::kHeight - y, theme::kPanelDark);
    g.text(theme::kSafeLeft, y + 2, v.hint(), theme::kTextDim, 1, 2);
    g.text(theme::kSafeLeft, y + 18, "START play/pause  L3 stop  R3 debug", theme::kTextDim, 1, 2);
}

static bool noWait() { return false; }

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
    UiContext ctx{session, engine, audio, storage, log, bank, menu, library};
    ctx.selectedChannel = 2;
    Gfx g;
    ChannelRackView rack;
    PlaylistView playlist;
    MixerView mixer;
    BrowserView browser;
    ProjectView project;
    View* views[] = {&rack, &playlist, &mixer, &browser, &project};
    const char* names[] = {"rack", "playlist", "mixer", "browser", "project"};
    char path[256];
    session.placeClip(0, 0, 0, 2);
    session.placeClip(0, 2, 1, 1);
    session.placeClip(1, 1, 2, 4);
    session.placeClip(2, 4, 3, 2);
    session.placeClip(3, 0, 0, 20); // runs past the visible window
    session.setSongMode(true);
    for (int i = 0; i < 5; ++i) {
        g.beginFrame(theme::kBackground);
        chrome(g, ctx, i, *views[i]);
        snprintf(path, sizeof(path), "%s/%s.png", outDir, names[i]);
        writePng(path);
    }
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
        chrome(g, ctx, 3, browser);
        snprintf(path, sizeof(path), "%s/browser_usb.png", outDir);
        writePng(path);
        InputState r2;
        r2.pressed = r2.repeat = btn::R2;
        browser.update(r2, ctx);
        g.beginFrame(theme::kBackground);
        chrome(g, ctx, 3, browser);
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
