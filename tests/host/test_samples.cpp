// Host tests for external samples: references, bank lifecycle, import limits,
// the audio-thread release handshake, and the asynchronous library.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "audio/audio_engine.hpp"
#include "audio/drum_synth.hpp"
#include "audio/sample_import.hpp"
#include "audio/sample_ref.hpp"
#include "project/project_io.hpp"
#include "project/sample_library.hpp"
#include "project/session.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static bool noYield() { return false; }

static void p16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void p32(std::vector<uint8_t>& v, uint32_t x) { p16(v, (uint16_t)x); p16(v, (uint16_t)(x >> 16)); }

static std::vector<uint8_t> wavFile(uint16_t ch, uint32_t rate, uint16_t bits, uint32_t frames, int16_t level = 9000)
{
    std::vector<uint8_t> v;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    p32(v, 0);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    p32(v, 16);
    p16(v, 1);
    p16(v, ch);
    p32(v, rate);
    p32(v, rate * ch * bits / 8);
    p16(v, (uint16_t)(ch * bits / 8));
    p16(v, bits);
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    p32(v, frames * ch * bits / 8);
    for (uint32_t i = 0; i < frames * ch; ++i) {
        if (bits == 16)
            p16(v, (uint16_t)level);
        else
            v.push_back(200);
    }
    const uint32_t riff = (uint32_t)v.size() - 8;
    v[4] = (uint8_t)riff; v[5] = (uint8_t)(riff >> 8); v[6] = (uint8_t)(riff >> 16); v[7] = (uint8_t)(riff >> 24);
    return v;
}

// ---- references -----------------------------------------------------------------
static void testRefs()
{
    using namespace sampleref;
    CHECK(classify("") == Kind::None);
    CHECK(classify(nullptr) == Kind::None);
    CHECK(classify("builtin:KICK") == Kind::Builtin);
    CHECK(classify("builtin:") == Kind::Invalid);
    CHECK(classify("samples:KICK.WAV") == Kind::Samples);
    CHECK(classify("samples:DRUMS/808/KICK.WAV") == Kind::Samples);
    CHECK(classify("mass0:/PS2DAW/SAMPLES/KICK.WAV") == Kind::Invalid); // device paths are never stored
    CHECK(classify("KICK.WAV") == Kind::Invalid);

    const char* bad[] = {"", "/abs.wav", "a//b.wav", "a/", "../x.wav", "a/../x.wav", "./x.wav", "a/./x.wav", "x\\y.wav",
                         "mass0:x.wav", "a:b.wav", "bad*.wav", "q?.wav", "p|.wav", "x<y.wav", "tab\t.wav", "high\x80.wav",
                         "trail.", "trail /x.wav", "sp ace ", ".."};
    for (const char* b : bad)
        CHECK(!validRelPath(b));
    CHECK(validRelPath("Kick Drum (1).wav"));
    CHECK(validRelPath("A/B/C/d.wav"));

    char out[kMaxRef];
    CHECK(make(out, sizeof(out), "A/B.WAV") && strcmp(out, "samples:A/B.WAV") == 0);
    CHECK(!make(out, sizeof(out), "../B.WAV") && out[0] == '\0');
    // Longest relative path that still fits the 64-byte reference field.
    std::string longOk(kMaxRef - strlen(kSamplesPrefix) - 1, 'a');
    CHECK(make(out, sizeof(out), longOk.c_str()));
    CHECK(!make(out, sizeof(out), (longOk + "a").c_str()));
    CHECK(!make(out, 10, "KICK.WAV")); // output buffer too small

    char dev[200];
    CHECK(devicePath(dev, sizeof(dev), "mass1:", "A/K.WAV") && strcmp(dev, "mass1:/PS2DAW/SAMPLES/A/K.WAV") == 0);
    CHECK(devicePath(dev, sizeof(dev), "mass0:", "") && strcmp(dev, "mass0:/PS2DAW/SAMPLES") == 0);
    CHECK(!devicePath(dev, sizeof(dev), "mass0:", "../../etc"));
    CHECK(!devicePath(dev, 12, "mass0:", "K.WAV"));
    // The same reference resolves under either root: portable between ports.
    CHECK(strcmp(relPart("samples:A/K.WAV"), "A/K.WAV") == 0 && relPart("builtin:X") == nullptr);

    CHECK(classifyFile("k.WaV", false) == FileType::Wav);
    CHECK(classifyFile("k.adp", false) == FileType::Adp);
    CHECK(classifyFile("k.mp3", false) == FileType::Unsupported);
    CHECK(classifyFile("noext", false) == FileType::Unsupported);
    CHECK(classifyFile("x.wav", true) == FileType::Dir);

    char rel[64] = "A/B";
    CHECK(parentRel(rel) && strcmp(rel, "A") == 0);
    CHECK(parentRel(rel) && rel[0] == '\0');
    CHECK(!parentRel(rel));
    char j[12];
    CHECK(joinRel(j, sizeof(j), "", "K.WAV") && strcmp(j, "K.WAV") == 0);
    CHECK(joinRel(j, sizeof(j), "AB", "K.WAV") && strcmp(j, "AB/K.WAV") == 0);
    CHECK(!joinRel(j, sizeof(j), "ABCDEFGH", "K.WAV"));

    CHECK(compareEntries("b", true, "a", false) < 0);   // directories first
    CHECK(compareEntries("a.wav", false, "B.wav", false) < 0);
    CHECK(compareEntries("kick", false, "KICK", false) == 0);
}

// ---- bank lifecycle ---------------------------------------------------------------
static int16_t* pcm(uint32_t samples)
{
    int16_t* p = (int16_t*)malloc(samples * sizeof(int16_t));
    for (uint32_t i = 0; i < samples; ++i)
        p[i] = 100;
    return p;
}

static void testBankLifecycle()
{
    SampleBank bank;
    const int kick = bank.add("KICK", pcm(1000), 1000, 48000, 1, true);
    CHECK(kick == 0 && strcmp(bank.get(0)->ref, "builtin:KICK") == 0);
    CHECK(!bank.requestRelease(kick)); // built-ins are never released

    const int a = bank.add("A.WAV", pcm(2000), 1000, 44100, 2, false, "samples:A.WAV", 4044);
    CHECK(a == 1 && bank.findByRef("SAMPLES:a.wav") == 1);
    CHECK(bank.externalBytesUsed() == 4000);
    const uint16_t gen = bank.get(a)->generation;

    CHECK(bank.requestRelease(a));
    CHECK(bank.get(a) == nullptr);         // unreadable at once: no new voices
    CHECK(bank.peek(a) != nullptr);        // memory still there for the audio thread
    CHECK(bank.externalBytesUsed() == 4000);
    CHECK(bank.reap() == 0);               // not acknowledged: must NOT free
    CHECK(bank.peek(a)->data != nullptr);
    bank.acknowledgeRelease(a);
    CHECK(bank.reap() == 1);
    CHECK(bank.externalBytesUsed() == 0 && bank.findByRef("samples:A.WAV") < 0);

    const int b = bank.add("B.WAV", pcm(10), 10, 8000, 1, false, "samples:B.WAV");
    CHECK(b == a); // slot reused
    CHECK(bank.get(b)->generation != gen);

    // Budget: one huge sample cannot exceed kMaxExternalBytes.
    const uint32_t big = SampleBank::kMaxExternalBytes / 2 + 2; // frames of stereo = 2 bytes*2
    int16_t* d1 = pcm(big);
    CHECK(bank.add("BIG1", d1, big / 2, 48000, 1, false, "samples:BIG1.WAV") >= 0);
    int16_t* d2 = pcm(big);
    CHECK(bank.add("BIG2", d2, big / 2, 48000, 1, false, "samples:BIG2.WAV") < 0);
    CHECK(bank.lastFailure() == SampleBank::Failure::BudgetFull);
    free(d2); // add() takes ownership only on success

    // Slots: fill every slot with tiny samples.
    int added = 0; (void)added;
    for (int i = 0; i < cfg::kMaxSamples + 4; ++i) {
        char n[16];
        snprintf(n, sizeof(n), "S%d", i);
        char r[32];
        snprintf(r, sizeof(r), "samples:S%d.WAV", i);
        int16_t* p = pcm(4);
        if (bank.add(n, p, 4, 8000, 1, false, r) >= 0)
            ++added;
        else
            free(p);
    }
    CHECK(bank.lastFailure() == SampleBank::Failure::SlotsFull);
    CHECK(!bank.hasFreeSlot());
    CHECK(bank.liveCount() == cfg::kMaxSamples);

    // Invalid input.
    CHECK(bank.add("x", nullptr, 4, 8000, 1, false) < 0);
    int16_t* zero = pcm(1);
    CHECK(bank.add("x", zero, 0, 8000, 1, false) < 0);
    free(zero);
    int16_t* bad = pcm(1);
    CHECK(bank.add("x", bad, 1, 8000, 3, false) < 0);
    free(bad);

    // discardNew frees an unpublished slot immediately.
    SampleBank b2;
    const int s = b2.add("N", pcm(8), 8, 8000, 1, false, "samples:N.WAV");
    b2.discardNew(s);
    CHECK(b2.liveCount() == 0 && b2.externalBytesUsed() == 0);
    // Hardware-only slots carry no PCM.
    const int h = b2.addHwOnly("X.ADP", "samples:X.ADP", 4096);
    CHECK(h >= 0 && b2.get(h)->hwOnly && b2.get(h)->data == nullptr && b2.externalBytesUsed() == 0);
}

// ---- import limits ----------------------------------------------------------------
static void testImportLimits()
{
    SampleBank bank;
    char err[64];
    int slot = -1;
    using sampleimport::Result;

    auto ok = wavFile(2, 44100, 16, 1000);
    CHECK(sampleimport::importWav(bank, "OK.WAV", "samples:OK.WAV", ok.data(), ok.size(), &slot, err, sizeof(err)) == Result::Ok);
    CHECK(slot == 0 && bank.get(0)->channels == 2 && bank.get(0)->frames == 1000 && bank.get(0)->pcmBytes == 4000);

    auto eight = wavFile(1, 22050, 8, 500);
    CHECK(sampleimport::importWav(bank, "E.WAV", "samples:E.WAV", eight.data(), eight.size(), &slot, err, sizeof(err)) == Result::Ok);
    CHECK(bank.get(slot)->pcmBytes == 1000 && bank.get(slot)->data[0] == (int16_t)((200 - 128) << 8));

    // File larger than the cap: rejected before parsing.
    std::vector<uint8_t> huge(sampleimport::kMaxFileBytes + 1, 0);
    CHECK(sampleimport::importWav(bank, "H", "samples:H.WAV", huge.data(), huge.size(), &slot, err, sizeof(err)) == Result::TooLarge);
    CHECK(slot == -1);

    // 8-bit file that fits on disk but doubles past the PCM cap on conversion.
    auto doubling = wavFile(1, 8000, 8, 2 * 1024 * 1024 + 200);
    CHECK(doubling.size() <= sampleimport::kMaxFileBytes);
    CHECK(sampleimport::importWav(bank, "D", "samples:D.WAV", doubling.data(), doubling.size(), &slot, err, sizeof(err)) ==
          Result::TooLarge);

    // Garbage and truncations never crash and never touch the bank.
    const int before = bank.liveCount();
    srand(7);
    for (int i = 0; i < 500; ++i) {
        std::vector<uint8_t> g = ok;
        for (int k = 0; k < 6; ++k)
            g[(size_t)rand() % g.size()] = (uint8_t)rand();
        g.resize((size_t)rand() % g.size());
        sampleimport::importWav(bank, "G", "samples:G.WAV", g.data(), g.size(), &slot, err, sizeof(err));
        if (slot >= 0)
            bank.discardNew(slot);
    }
    CHECK(bank.liveCount() == before);

    // Memory budget: ~2.9 MiB samples fill the 12 MiB budget after 4.
    SampleBank full;
    auto chunk = wavFile(1, 48000, 16, 1500000); // 3,000,000 bytes
    int loaded = 0;
    Result last = Result::Ok;
    for (int i = 0; i < 6; ++i) {
        char ref[32];
        snprintf(ref, sizeof(ref), "samples:F%d.WAV", i);
        last = sampleimport::importWav(full, "F", ref, chunk.data(), chunk.size(), &slot, err, sizeof(err));
        if (last != Result::Ok)
            break;
        ++loaded;
    }
    CHECK(loaded == 4 && last == Result::BudgetFull);
    CHECK(strstr(err, "memory") != nullptr);
    CHECK(full.externalBytesUsed() <= SampleBank::kMaxExternalBytes);
}

// ---- audio-thread release handshake -----------------------------------------------
struct Harness {
    SampleBank bank;
    AudioEngine engine;
    Session session;
    Harness() : session(engine, bank, noYield) { engine.setSampleBank(&bank); }
    void render(int blocks)
    {
        int16_t buf[cfg::kMaxBlockFrames * 2];
        for (int i = 0; i < blocks; ++i)
            engine.render(buf, 512);
    }
};

static void testReleaseHandshake()
{
    Harness h;
    drumsynth::generateKit(h.bank);
    h.session.resync();
    // A long imported sample, assigned to channel 0, with a voice playing.
    const uint32_t frames = 48000;
    const int slot = h.bank.add("LONG.WAV", pcm(frames), frames, 48000, 1, false, "samples:LONG.WAV");
    CHECK(slot >= 0);
    h.session.setSample(0, slot);
    h.session.previewSample(slot, VoiceMode::Software);
    h.render(2);
    CHECK(h.engine.status().voicesActive == 1);

    CHECK(!h.session.releaseSample(slot));          // refused: channel 0 uses it
    h.session.setSample(0, 0);                      // back to the kick
    CHECK(h.session.releaseSample(slot));
    CHECK(h.bank.isReleasing(slot));
    CHECK(h.bank.reap() == 0);                      // audio has not acknowledged yet
    CHECK(h.bank.peek(slot)->data != nullptr);
    h.render(1);                                    // audio thread processes ReleaseSample
    CHECK(h.engine.status().voicesActive == 0);     // voices reading it were stopped
    CHECK(h.bank.reap() == 1);
    CHECK(h.bank.get(slot) == nullptr);

    // The slot is reusable and a stale channel binding cannot play it.
    const int slot2 = h.bank.add("NEW.WAV", pcm(100), 100, 48000, 1, false, "samples:NEW.WAV");
    CHECK(slot2 == slot);
    h.session.play();
    h.render(30);
    CHECK(h.engine.status().triggerCount[0] > 0); // kick on channel 0 still plays
}

// ---- library with fake USB --------------------------------------------------------
struct MemFiles : SampleFileSource {
    std::map<std::string, std::vector<uint8_t>> files;
    struct H { const std::vector<uint8_t>* f; size_t pos; bool open; };
    std::vector<H> handles;
    int reads = 0, maxChunk = 0, openCount = 0, closeCount = 0;
    int failReadAfter = -1;

    int openSample(const char* rel, uint32_t* size) override
    {
        auto it = files.find(rel);
        if (it == files.end())
            return -1;
        handles.push_back({&it->second, 0, true});
        ++openCount;
        *size = (uint32_t)it->second.size();
        return (int)handles.size() - 1;
    }
    int read(int h, uint8_t* buf, uint32_t n) override
    {
        ++reads;
        if (failReadAfter >= 0 && reads > failReadAfter)
            return -1;
        if ((int)n > maxChunk)
            maxChunk = (int)n;
        H& s = handles[(size_t)h];
        const size_t left = s.f->size() - s.pos;
        const size_t c = left < n ? left : n;
        memcpy(buf, s.f->data() + s.pos, c);
        s.pos += c;
        return (int)c;
    }
    void close(int h) override
    {
        CHECK(handles[(size_t)h].open);
        handles[(size_t)h].open = false;
        ++closeCount;
    }
};

struct FakeHw : HwSink {
    bool res[cfg::kMaxSamples] = {};
    uint32_t used = 0, capacity = 100000;
    int unloads = 0;
    bool uploadPcm(int slot, const Sample& s, char* err, size_t cap) override
    {
        if (s.channels != 1) {
            snprintf(err, cap, "SPU2 needs a mono sample");
            return false;
        }
        const uint32_t b = s.frames / 2;
        if (used + b > capacity) {
            snprintf(err, cap, "SPU2 RAM full");
            return false;
        }
        used += b;
        res[slot] = true;
        return true;
    }
    bool uploadApcm(int slot, const uint8_t*, uint32_t size, char* err, size_t cap) override
    {
        if (size % 16) {
            snprintf(err, cap, "not a valid .adp file");
            return false;
        }
        res[slot] = true;
        used += size;
        return true;
    }
    void unload(int slot) override { res[slot] = false; ++unloads; }
    bool resident(int slot) const override { return res[slot]; }
    uint32_t bytesUsed() const override { return used; }
};

struct LibHarness {
    Harness h;
    MemFiles files;
    FakeHw hw;
    StatusLog log;
    SampleLibrary lib;
    LibHarness() : lib(h.session, h.bank, files, &hw, log) { drumsynth::generateKit(h.bank); h.session.resync(); }
    void pump(int frames)
    {
        for (int i = 0; i < frames; ++i) {
            lib.tick();
            h.render(1);
        }
    }
    void settle() { for (int i = 0; i < 400 && lib.busy(); ++i) pump(1); pump(4); }
};

static void testLibraryLoadAssign()
{
    LibHarness t;
    t.files.files["DRUMS/KICK.WAV"] = wavFile(1, 44100, 16, 40000); // 80 KB: three chunks
    t.pump(2);

    CHECK(t.lib.request("samples:DRUMS/KICK.WAV", SampleLibrary::Action::Assign, 3));
    int ticks = 0;
    int maxProgress = 0;
    while (t.lib.busy() && ticks < 50) {
        t.pump(1);
        if (t.lib.progressPercent() > maxProgress)
            maxProgress = t.lib.progressPercent();
        ++ticks;
    }
    CHECK(ticks >= 3);                        // spread over several frames
    CHECK(t.files.maxChunk <= (int)SampleLibrary::kChunkBytes);
    CHECK(t.files.closeCount == t.files.openCount);
    const ChannelData& c = t.h.session.project().channels[3];
    CHECK(strcmp(c.sampleRef, "samples:DRUMS/KICK.WAV") == 0 && strcmp(c.name, "KICK.WAV") == 0);
    CHECK(c.sampleSlot >= 0 && t.h.bank.get(c.sampleSlot) != nullptr);
    CHECK(t.lib.loadedOk() == 1);

    // Requesting it again needs no I/O.
    const int opens = t.files.openCount;
    t.lib.request("samples:DRUMS/KICK.WAV", SampleLibrary::Action::Load);
    t.settle();
    CHECK(t.files.openCount == opens);

    // Invalid references are refused before any I/O.
    CHECK(!t.lib.request("samples:../x.wav", SampleLibrary::Action::Load));
    CHECK(!t.lib.request("mass0:/PS2DAW/SAMPLES/K.WAV", SampleLibrary::Action::Load));
    CHECK(t.files.openCount == opens);
}

static void testLibraryFailures()
{
    LibHarness t;
    t.files.files["BAD.WAV"] = std::vector<uint8_t>(500, 0x55);
    t.files.files["EMPTY.WAV"] = {};
    t.files.files["NOTES.TXT"] = std::vector<uint8_t>(100, 1);
    t.files.files["BIG.WAV"] = std::vector<uint8_t>(sampleimport::kMaxFileBytes + 1, 0);
    auto good = wavFile(1, 44100, 16, 30000);
    t.files.files["SHORT.WAV"] = good;

    const char* refs[] = {"samples:BAD.WAV", "samples:EMPTY.WAV", "samples:NOTES.TXT", "samples:BIG.WAV", "samples:NOPE.WAV"};
    for (const char* r : refs) {
        const uint32_t serial = t.lib.messageSerial();
        t.lib.request(r, SampleLibrary::Action::Assign, 0);
        t.settle();
        CHECK(t.lib.messageSerial() != serial); // every failure is reported
        CHECK(t.h.session.project().channels[0].sampleSlot == 0); // still the built-in kick
        CHECK(strcmp(t.h.session.project().channels[0].sampleRef, "builtin:KICK") == 0);
    }
    CHECK(t.lib.loadFailed() == 5 && t.log.hasError());
    CHECK(t.files.closeCount == t.files.openCount);

    // Read error half way through: handle closed, buffer freed (ASan checks), no sample added.
    t.files.failReadAfter = t.files.reads + 1;
    t.lib.request("samples:SHORT.WAV", SampleLibrary::Action::Load);
    t.settle();
    CHECK(t.lib.loadFailed() == 6);
    CHECK(t.h.bank.findByRef("samples:SHORT.WAV") < 0);
    CHECK(t.files.closeCount == t.files.openCount);
}

static void testLibraryProjectRoundTrip()
{
    LibHarness t;
    t.files.files["A/SNARE.WAV"] = wavFile(1, 44100, 16, 3000);
    t.files.files["HAT.WAV"] = wavFile(2, 48000, 16, 2000);

    t.lib.request("samples:A/SNARE.WAV", SampleLibrary::Action::Assign, 1);
    t.lib.request("samples:HAT.WAV", SampleLibrary::Action::Assign, 2);
    t.settle();

    static uint8_t buf[projectio::kMaxFileBytes];
    const size_t n = projectio::save(t.h.session.project(), buf, sizeof(buf));
    CHECK(n > 0);

    // A fresh session (no samples resident) loads the project: external
    // samples are fetched by reference, whichever USB port they are on.
    LibHarness u;
    u.files.files = t.files.files;
    Project p;
    char err[64];
    CHECK(projectio::load(buf, n, p, err, sizeof(err)));
    CHECK(strcmp(p.channels[1].sampleRef, "samples:A/SNARE.WAV") == 0);
    u.h.session.loadProject(p);
    CHECK(u.h.session.project().channels[1].sampleSlot == -1); // not resident yet
    u.settle();
    const Project& q = u.h.session.project();
    CHECK(q.channels[1].sampleSlot >= 0 && strcmp(u.h.bank.get(q.channels[1].sampleSlot)->ref, "samples:A/SNARE.WAV") == 0);
    CHECK(q.channels[2].sampleSlot >= 0 && u.h.bank.get(q.channels[2].sampleSlot)->channels == 2);
    CHECK(u.lib.missingCount() == 0);

    // Same project, one sample deleted from the stick: usable, reported, no crash.
    LibHarness m;
    m.files.files["A/SNARE.WAV"] = t.files.files["A/SNARE.WAV"]; // HAT.WAV is gone
    m.h.session.loadProject(p);
    m.settle();
    const Project& r = m.h.session.project();
    CHECK(r.channels[1].sampleSlot >= 0);
    CHECK(r.channels[2].sampleSlot == -1);
    CHECK(strcmp(r.channels[2].sampleRef, "samples:HAT.WAV") == 0); // reference kept for when it returns
    CHECK(m.lib.missingCount() == 1 && strcmp(m.lib.missingRef(0), "samples:HAT.WAV") == 0);
    CHECK(m.log.entry(Subsystem::Samples).health == Health::Warning);
    m.h.session.play();
    m.pump(100); // plays without the missing sample
    CHECK(m.h.engine.status().triggerCount[0] > 0);
    CHECK(m.h.engine.status().triggerCount[2] == 0);
    // Saving again keeps the reference rather than dropping it.
    const size_t n2 = projectio::save(m.h.session.project(), buf, sizeof(buf));
    Project p2;
    CHECK(projectio::load(buf, n2, p2, err, sizeof(err)) && strcmp(p2.channels[2].sampleRef, "samples:HAT.WAV") == 0);

    // The file comes back (stick re-inserted): re-binding finds it.
    m.files.files["HAT.WAV"] = t.files.files["HAT.WAV"];
    m.h.session.loadProject(p2);
    m.settle();
    CHECK(m.h.session.project().channels[2].sampleSlot >= 0 && m.lib.missingCount() == 0);

    // Hostile reference in a loaded project is reported, not followed.
    Project evil = p;
    strcpy(evil.channels[4].sampleRef, "samples:../../SECRET.WAV");
    LibHarness e;
    e.h.session.loadProject(evil);
    e.settle();
    CHECK(e.lib.missingCount() >= 1 && e.files.openCount == 0);
}

static void testLibraryMemoryPressure()
{
    LibHarness t;
    auto big = wavFile(1, 48000, 16, 1400000); // 2.8 MB PCM each
    for (int i = 0; i < 7; ++i) {
        char rel[16];
        snprintf(rel, sizeof(rel), "B%d.WAV", i);
        t.files.files[rel] = big;
    }
    // Load four and keep none assigned: the budget is full of unused samples.
    for (int i = 0; i < 4; ++i) {
        char ref[24];
        snprintf(ref, sizeof(ref), "samples:B%d.WAV", i);
        t.lib.request(ref, SampleLibrary::Action::Load);
        t.settle();
    }
    CHECK(t.h.bank.externalBytesUsed() > SampleBank::kMaxExternalBytes - 3000000);
    // A fifth does not fit; the library frees what nothing uses, then retries.
    t.lib.request("samples:B4.WAV", SampleLibrary::Action::Assign, 5);
    t.settle();
    CHECK(t.h.session.project().channels[5].sampleSlot >= 0);
    CHECK(strcmp(t.h.session.project().channels[5].sampleRef, "samples:B4.WAV") == 0);
    CHECK(t.h.bank.externalBytesUsed() <= SampleBank::kMaxExternalBytes);

    // Samples in use are never reclaimed: fill every channel, then ask for more.
    for (int ch = 0; ch < 4; ++ch) {
        char ref[24];
        snprintf(ref, sizeof(ref), "samples:B%d.WAV", ch + 1);
        t.lib.request(ref, SampleLibrary::Action::Assign, ch);
        t.settle();
    }
    const uint32_t failedBefore = t.lib.loadFailed();
    t.lib.request("samples:B6.WAV", SampleLibrary::Action::Assign, 7);
    t.settle();
    CHECK(t.lib.loadFailed() == failedBefore + 1);
    CHECK(strstr(t.lib.message(), "out of sample memory") != nullptr);
    for (int ch = 0; ch < 4; ++ch)
        CHECK(t.h.bank.get(t.h.session.project().channels[ch].sampleSlot) != nullptr);
    CHECK(t.h.session.project().channels[7].sampleSlot >= 0); // channel 7 keeps its built-in
}

static void testLibrarySpu2()
{
    LibHarness t;
    t.files.files["MONO.WAV"] = wavFile(1, 44100, 16, 8000);
    t.files.files["STEREO.WAV"] = wavFile(2, 44100, 16, 8000);
    t.files.files["LOOP.ADP"] = std::vector<uint8_t>(1600, 3);
    t.files.files["BAD.ADP"] = std::vector<uint8_t>(1601, 3);
    t.files.files["TINY.ADP"] = std::vector<uint8_t>(8, 3);

    t.lib.request("samples:MONO.WAV", SampleLibrary::Action::UploadSpu2);
    t.settle();
    const int mono = t.h.bank.findByRef("samples:MONO.WAV");
    CHECK(mono >= 0 && t.hw.resident(mono));

    t.lib.request("samples:STEREO.WAV", SampleLibrary::Action::UploadSpu2);
    t.settle();
    const int st = t.h.bank.findByRef("samples:STEREO.WAV");
    CHECK(st >= 0 && !t.hw.resident(st));                      // clean refusal, still usable in software
    CHECK(strstr(t.lib.message(), "mono") != nullptr);

    // .adp: hardware-only slot, no PCM in EE RAM.
    t.lib.request("samples:LOOP.ADP", SampleLibrary::Action::Assign, 6);
    t.settle();
    const int adp = t.h.bank.findByRef("samples:LOOP.ADP");
    CHECK(adp >= 0 && t.h.bank.get(adp)->hwOnly && t.hw.resident(adp));
    CHECK(t.h.session.project().channels[6].voiceMode == (uint8_t)VoiceMode::Spu2); // forced to hardware
    const uint32_t pcmBefore = t.h.bank.externalBytesUsed();
    CHECK(pcmBefore == 16000 + 32000); // MONO + STEREO PCM only, nothing for the .adp

    const int slotsBefore = t.h.bank.liveCount();
    t.lib.request("samples:BAD.ADP", SampleLibrary::Action::Load);
    t.lib.request("samples:TINY.ADP", SampleLibrary::Action::Load);
    t.settle();
    CHECK(t.h.bank.liveCount() == slotsBefore);                 // failed uploads leave no slot behind

    // Unloading frees SPU2 first, then the slot; assigned samples are refused.
    CHECK(!t.lib.unload(adp));
    t.h.session.setSample(6, 6);
    CHECK(t.lib.unload(adp));
    t.settle();
    CHECK(!t.hw.resident(adp) && t.h.bank.findByRef("samples:LOOP.ADP") < 0);
    CHECK(!t.lib.unload(0)); // built-ins stay
}

int runSampleTests()
{
    printf("running sample tests\n");
    testRefs();
    testBankLifecycle();
    testImportLimits();
    testReleaseHandshake();
    testLibraryLoadAssign();
    testLibraryFailures();
    testLibraryProjectRoundTrip();
    testLibraryMemoryPressure();
    testLibrarySpu2();
    return g_fail;
}
