// Host-side tests for the platform-independent core. Not part of the PS2
// build; run with tests/host/run.sh (g++ with ASan/UBSan).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "audio/adpcm.hpp"
#include "audio/audio_engine.hpp"
#include "audio/drum_synth.hpp"
#include "audio/wav.hpp"
#include "project/project_io.hpp"
#include "project/session.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static bool noYield() { return false; }

// Collects step marks while rendering with irregular block sizes.
struct Harness {
    SampleBank bank;
    AudioEngine engine;
    Session session;
    std::vector<StepMark> marks;
    std::vector<int16_t> out;
    uint32_t seenSerial = 0;

    Harness() : session(engine, bank, noYield) { engine.setSampleBank(&bank); }

    void render(uint32_t totalFrames, bool keepAudio = false)
    {
        static const int sizes[] = {512, 333, 1, 77, 256, 500, 2, 129};
        int16_t buf[cfg::kMaxBlockFrames * 2];
        uint32_t done = 0;
        int k = 0;
        while (done < totalFrames) {
            int n = sizes[k++ % 8];
            if (done + n > totalFrames)
                n = (int)(totalFrames - done);
            engine.render(buf, n);
            if (keepAudio)
                out.insert(out.end(), buf, buf + n * 2);
            const EngineStatus& st = engine.status();
            while (seenSerial < st.markSerial) {
                marks.push_back(st.marks[seenSerial % EngineStatus::kMarks]);
                ++seenSerial;
            }
            done += (uint32_t)n;
        }
    }
};

static uint64_t expectedStepFrame(uint64_t k, uint32_t bpmCenti)
{
    const uint64_t D = Transport::kDenominator;
    const uint64_t inc = (uint64_t)bpmCenti * cfg::kPpq;
    const uint64_t target = k * cfg::kTicksPerStep * D;
    return (target + inc - 1) / inc;
}

static void testTimingExact(uint32_t bpmCenti)
{
    Harness h;
    h.session.setBpmCenti((int)bpmCenti);
    h.session.play();
    h.render(cfg::kSampleRate * 30); // 30 s, irregular block sizes
    CHECK(h.marks.size() > 100);
    // Pattern loops every 16 steps; frames must follow the exact rational
    // schedule computed from the absolute step number: no drift, no jitter.
    for (size_t k = 0; k < h.marks.size(); ++k)
        CHECK(h.marks[k].step == k % 16);
    // Absolute check: step k fires at ceil(k*24*D/inc) because loop re-basing
    // keeps the sub-tick remainder.
    for (size_t k = 0; k < h.marks.size(); ++k) {
        const uint64_t want = expectedStepFrame(k, bpmCenti);
        if (h.marks[k].frame != (uint32_t)want) {
            fprintf(stderr, "bpm %u step %zu: got %u want %llu\n", bpmCenti, k, h.marks[k].frame, (unsigned long long)want);
            CHECK(false);
            break;
        }
    }
}

static void testSampleAccurateOnset()
{
    Harness h;
    // A DC "sample" makes the onset frame unambiguous in the output.
    int16_t* dc = (int16_t*)malloc(200 * sizeof(int16_t));
    for (int i = 0; i < 200; ++i)
        dc[i] = 20000;
    const int slot = h.bank.add("DC", dc, 200, cfg::kSampleRate, 1, true);
    CHECK(slot == 0);
    Project p;
    p.resetEmpty();
    p.channels[0].sampleSlot = (int8_t)slot;
    for (int ch = 1; ch < cfg::kMaxChannels; ++ch)
        p.channels[ch].sampleSlot = -1;
    p.channels[0].volume = 100;
    p.masterVolume = 100;
    p.patterns[0].velocity[0][0] = 127;
    p.patterns[0].velocity[0][5] = 127;
    h.session.loadProject(p);
    h.session.setBpmCenti(12000);
    h.session.play();
    h.render(cfg::kSampleRate * 3, true);
    // 120 BPM: a 16th = 6000 frames. Onsets at 0, 30000, then next loop at 96000.
    auto left = [&](uint32_t f) { return h.out[f * 2]; };
    CHECK(left(0) != 0);
    CHECK(left(199) != 0);
    CHECK(left(200) == 0);
    CHECK(left(29999) == 0);
    CHECK(left(30000) != 0);
    CHECK(left(95999) == 0);
    CHECK(left(96000) != 0);
}

static void testBpmChangeNoJump()
{
    Harness h;
    h.session.play();
    h.render(cfg::kSampleRate * 2);
    const size_t before = h.marks.size();
    h.session.setBpmCenti(17500);
    h.render(cfg::kSampleRate * 2);
    CHECK(h.marks.size() > before);
    // Intervals after the change must equal the new step length (+/-1 frame
    // from ceil rounding), and the boundary interval must lie between them.
    const double newStep = 60.0 / 175.0 / 4.0 * cfg::kSampleRate;
    for (size_t k = before + 1; k + 1 < h.marks.size(); ++k) {
        const double d = (double)(h.marks[k + 1].frame - h.marks[k].frame);
        CHECK(fabs(d - newStep) <= 1.0);
    }
}

static void testPatternLengthShrink()
{
    Harness h;
    h.session.setPatternLength(0, 32);
    h.session.play();
    h.render(6000 * 20); // reach step ~20
    h.session.setPatternLength(0, 8);
    h.render(6000 * 20);
    bool sawWrap = false;
    for (size_t k = 1; k < h.marks.size(); ++k) {
        CHECK(h.marks[k].step < 32);
        if (h.marks[k - 1].step >= 8 && h.marks[k].step == 0)
            sawWrap = true;
    }
    CHECK(sawWrap);
    CHECK(h.marks.back().step < 8);
}

static void testClipping()
{
    Harness h;
    CHECK(drumsynth::generateKit(h.bank) == (int)drumsynth::Kind::Count);
    Project p;
    p.resetEmpty();
    p.masterVolume = 100;
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        p.channels[ch].sampleSlot = (int8_t)drumsynth::Kind::Kick;
        p.channels[ch].volume = 100;
        p.patterns[0].velocity[ch][0] = 127;
    }
    h.session.loadProject(p);
    h.session.play();
    h.render(4800, true);
    CHECK(h.engine.status().clipSamples > 0);
    // Saturation, never wrap-around: kick starts positive and must stay at the rails.
    int16_t maxv = 0;
    for (size_t i = 0; i < h.out.size(); ++i)
        maxv = h.out[i] > maxv ? h.out[i] : maxv;
    CHECK(maxv == 32767);
}

static void testMuteSolo()
{
    Harness h;
    drumsynth::generateKit(h.bank);
    h.session.resync();
    h.session.setSolo(1, true);
    h.session.play();
    h.render(cfg::kSampleRate);
    const EngineStatus& st = h.engine.status();
    CHECK(st.triggerCount[0] == 0); // kick silenced by snare solo
    CHECK(st.triggerCount[1] > 0);
    h.session.setSolo(1, false);
    h.session.setMute(0, true);
    const uint32_t kicks = st.triggerCount[0];
    h.render(cfg::kSampleRate);
    CHECK(st.triggerCount[0] == kicks);
}

static void testProjectRoundTrip()
{
    Project a;
    a.resetDemo();
    a.bpmCenti = 13750;
    a.channels[3].pan = -42;
    a.channels[4].mute = 1;
    a.channels[5].solo = 1;
    a.channels[6].voiceMode = (uint8_t)VoiceMode::Spu2;
    a.patterns[2].length = 48;
    a.patterns[2].velocity[7][47] = 99;
    a.currentPattern = 2;
    a.clipCount = 2;
    a.clips[0] = {0, 0, 0, 4};
    a.clips[1] = {3, 2, 8, 4};

    static uint8_t buf[projectio::kMaxFileBytes];
    const size_t n = projectio::save(a, buf, sizeof(buf));
    CHECK(n > 0);

    Project b;
    char err[64] = "";
    CHECK(projectio::load(buf, n, b, err, sizeof(err)));
    CHECK(strcmp(b.name, a.name) == 0);
    CHECK(b.bpmCenti == a.bpmCenti);
    CHECK(b.currentPattern == 2);
    CHECK(b.channels[3].pan == -42);
    CHECK(b.channels[4].mute == 1 && b.channels[5].solo == 1);
    CHECK(b.channels[6].voiceMode == (uint8_t)VoiceMode::Spu2);
    CHECK(strcmp(b.channels[0].sampleRef, "builtin:KICK") == 0);
    CHECK(b.patterns[2].length == 48 && b.patterns[2].velocity[7][47] == 99);
    CHECK(memcmp(b.patterns, a.patterns, sizeof(a.patterns)) == 0);
    CHECK(b.clipCount == 2 && b.clips[1].startBar == 8);

    // Corruption is detected by the checksum.
    buf[40] ^= 0x55;
    CHECK(!projectio::load(buf, n, b, err, sizeof(err)));
    buf[40] ^= 0x55;
    // Truncation at every length must fail cleanly (ASan catches overreads).
    for (size_t cut = 0; cut < n; ++cut)
        CHECK(!projectio::load(buf, cut, b, err, sizeof(err)));
    // Too small a buffer refuses to save.
    CHECK(projectio::save(a, buf, 100) == 0);
}

static void putU16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void putU32(std::vector<uint8_t>& v, uint32_t x) { putU16(v, (uint16_t)x); putU16(v, (uint16_t)(x >> 16)); }

static std::vector<uint8_t> makeWav(uint16_t ch, uint32_t rate, uint16_t bits, uint32_t frames, bool oddChunk)
{
    std::vector<uint8_t> v;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    putU32(v, 0);
    v.insert(v.end(), {'W', 'A', 'V', 'E'});
    if (oddChunk) { // a 3-byte LIST chunk exercises the pad byte rule
        v.insert(v.end(), {'L', 'I', 'S', 'T'});
        putU32(v, 3);
        v.insert(v.end(), {1, 2, 3, 0});
    }
    v.insert(v.end(), {'f', 'm', 't', ' '});
    putU32(v, 16);
    putU16(v, 1);
    putU16(v, ch);
    putU32(v, rate);
    putU32(v, rate * ch * bits / 8);
    putU16(v, (uint16_t)(ch * bits / 8));
    putU16(v, bits);
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    putU32(v, frames * ch * bits / 8);
    for (uint32_t i = 0; i < frames * ch; ++i) {
        if (bits == 16)
            putU16(v, (uint16_t)(int16_t)(i * 37));
        else
            v.push_back((uint8_t)(i * 3));
    }
    const uint32_t riff = (uint32_t)v.size() - 8;
    v[4] = (uint8_t)riff; v[5] = (uint8_t)(riff >> 8); v[6] = (uint8_t)(riff >> 16); v[7] = (uint8_t)(riff >> 24);
    return v;
}

static void testWav()
{
    char err[64];
    wav::Info info;
    auto a = makeWav(2, 44100, 16, 1000, true);
    CHECK(wav::parse(a.data(), a.size(), info, err, sizeof(err)));
    CHECK(info.channels == 2 && info.sampleRate == 44100 && info.frames == 1000);
    int16_t* pcm = wav::toInt16(info);
    CHECK(pcm && pcm[1] == 37);
    free(pcm);

    auto b = makeWav(1, 22050, 8, 500, false);
    CHECK(wav::parse(b.data(), b.size(), info, err, sizeof(err)));
    CHECK(info.bitsPerSample == 8 && info.frames == 500);

    // Truncated data chunk: keep only frames actually present.
    auto c = makeWav(1, 48000, 16, 1000, false);
    c.resize(c.size() - 101);
    CHECK(wav::parse(c.data(), c.size(), info, err, sizeof(err)));
    CHECK(info.frames < 1000 && info.frames > 900);

    // Unsupported formats are rejected, not crashed on.
    auto d = makeWav(3, 48000, 16, 10, false);
    CHECK(!wav::parse(d.data(), d.size(), info, err, sizeof(err)));
    auto e = makeWav(1, 48000, 20, 10, false); // 24-bit is supported now
    CHECK(!wav::parse(e.data(), e.size(), info, err, sizeof(err)));

    // Every truncation of a valid file, and random garbage, must not crash.
    for (size_t cut = 0; cut < a.size(); ++cut)
        wav::parse(a.data(), cut, info, err, sizeof(err));
    srand(1234);
    for (int iter = 0; iter < 2000; ++iter) {
        std::vector<uint8_t> f = a;
        for (int k = 0; k < 8; ++k)
            f[(size_t)rand() % f.size()] = (uint8_t)rand();
        if (wav::parse(f.data(), f.size(), info, err, sizeof(err))) {
            CHECK(info.pcm >= f.data() && info.pcm + info.pcmBytes <= f.data() + f.size());
        }
    }
}

static void testAdpcm()
{
    const uint32_t n = 4800;
    std::vector<int16_t> in(n), out(adpcm::encodedSize(n) / 16 * 28);
    for (uint32_t i = 0; i < n; ++i)
        in[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * i / 48000.0) * exp(-3.0 * i / n));
    std::vector<uint8_t> enc(adpcm::encodedSize(n));
    adpcm::encodeMono(in.data(), n, enc.data());
    adpcm::decodeMono(enc.data(), enc.size(), out.data());
    double sig = 0, noise = 0;
    for (uint32_t i = 0; i < n; ++i) {
        sig += (double)in[i] * in[i];
        noise += (double)(in[i] - out[i]) * (in[i] - out[i]);
    }
    const double snr = 10.0 * log10(sig / (noise + 1e-9));
    printf("  adpcm SNR: %.1f dB\n", snr);
    CHECK(snr > 25.0);
    for (size_t b = 0; b < enc.size(); b += 16)
        CHECK(enc[b + 1] == 0 && (enc[b] >> 4) <= 4 && (enc[b] & 15) <= 12);
}

static void testHwTriggers()
{
    Harness h;
    drumsynth::generateKit(h.bank);
    h.session.resync();
    h.session.setSampleHwReady(0, true);
    h.session.setVoiceMode(0, VoiceMode::Spu2);
    h.session.play();
    int16_t buf[cfg::kMaxBlockFrames * 2];
    int hw = 0;
    uint32_t firstFrame = 0xffffffff;
    for (int i = 0; i < 400; ++i) {
        h.engine.render(buf, 480);
        for (int k = 0; k < h.engine.hwTriggerCount(); ++k) {
            const HwTrigger& t = h.engine.hwTrigger(k);
            CHECK(t.channel == 0 && t.sample == 0 && t.volume > 0 && t.volume <= 100);
            if (firstFrame == 0xffffffff)
                firstFrame = t.frame;
            ++hw;
        }
    }
    CHECK(hw > 0);
    CHECK(firstFrame == 0);
    CHECK(h.engine.status().triggerCount[0] == (uint32_t)hw);
}

static void testQueue()
{
    SpscQueue<int, 4> q;
    int v;
    CHECK(!q.pop(v));
    for (int i = 0; i < 4; ++i)
        CHECK(q.push(i));
    CHECK(!q.push(9));
    for (int i = 0; i < 4; ++i)
        CHECK(q.pop(v) && v == i);
    CHECK(!q.pop(v));
}

int runSampleTests();
int runWorkflowTests();
int runPlaylistTests();
int runPianoRollTests();
int runAudioTests();
int runNoteEditTests();

int main()
{
    printf("running host tests\n");
    testQueue();
    testTimingExact(12000);
    testTimingExact(12800);
    testTimingExact(13333);
    testTimingExact(17437);
    testSampleAccurateOnset();
    testBpmChangeNoJump();
    testPatternLengthShrink();
    testClipping();
    testMuteSolo();
    testHwTriggers();
    testProjectRoundTrip();
    testWav();
    testAdpcm();
    g_failures += runSampleTests();
    g_failures += runWorkflowTests();
    g_failures += runPlaylistTests();
    g_failures += runPianoRollTests();
    g_failures += runAudioTests();
    g_failures += runNoteEditTests();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
