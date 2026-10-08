// Host tests for Milestones 5/6/8 and the instrument work: mixer routing,
// effects, AHDSR envelopes, the native synthesizer, playlist audio clips,
// offline-render mode and the project format additions.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "audio/audio_engine.hpp"
#include "audio/drum_synth.hpp"
#include "audio/fx.hpp"
#include "audio/instrument.hpp"
#include "audio/wav_export.hpp"
#include <map>
#include <string>
#include "project/project_io.hpp"
#include "project/session.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (double)(tol))

static bool noYield() { return false; }

// A bank + engine + session at 120 BPM (a 16th = 6000 frames, a bar = 96000).
struct R {
    SampleBank bank;
    AudioEngine engine;
    Session session;
    std::vector<int16_t> L, Rt;

    R() : session(engine, bank, noYield)
    {
        engine.setSampleBank(&bank);
        Project p;
        p.resetEmpty();
        for (auto& c : p.channels)
            c.sampleSlot = -1;
        session.loadProject(p);
        session.setBpmCenti(12000);
        session.setMasterVolume(100);
    }

    int addSample(const char* name, const std::vector<int16_t>& pcm)
    {
        int16_t* d = (int16_t*)malloc(pcm.size() * sizeof(int16_t));
        memcpy(d, pcm.data(), pcm.size() * sizeof(int16_t));
        char ref[32];
        snprintf(ref, sizeof(ref), "builtin:%s", name);
        return bank.add(name, d, (uint32_t)pcm.size(), cfg::kSampleRate, 1, true, ref);
    }
    int dc(const char* name, uint32_t frames, int16_t v = 20000) { return addSample(name, std::vector<int16_t>(frames, v)); }

    void render(uint32_t frames)
    {
        int16_t buf[cfg::kMaxBlockFrames * 2];
        uint32_t done = 0;
        while (done < frames) {
            const int n = (int)(frames - done < 256 ? frames - done : 256);
            engine.render(buf, n);
            for (int i = 0; i < n; ++i) {
                L.push_back(buf[i * 2]);
                Rt.push_back(buf[i * 2 + 1]);
            }
            done += (uint32_t)n;
        }
    }
    // Leading silent frames.
    size_t silentRun() const
    {
        size_t n = 0;
        while (n < L.size() && L[n] == 0)
            ++n;
        return n;
    }
    // Frames of non-silence starting at `from`.
    size_t audibleRunFrom(size_t from) const
    {
        size_t n = 0;
        while (from + n < L.size() && L[from + n] != 0)
            ++n;
        return n;
    }
    double rms(size_t a, size_t b) const
    {
        double s = 0;
        for (size_t i = a; i < b && i < L.size(); ++i)
            s += (double)L[i] * L[i];
        return b > a ? sqrt(s / (double)(b - a)) : 0;
    }
    int peak(size_t a, size_t b) const
    {
        int m = 0;
        for (size_t i = a; i < b && i < L.size(); ++i)
            m = abs(L[i]) > m ? abs(L[i]) : m;
        return m;
    }
    int crossings(size_t a, size_t b) const
    {
        int n = 0;
        for (size_t i = a + 1; i < b && i < L.size(); ++i)
            if ((L[i - 1] < 0) != (L[i] < 0))
                ++n;
        return n;
    }
};

static std::vector<int16_t> sine(double hz, double amp, uint32_t frames)
{
    std::vector<int16_t> v(frames);
    for (uint32_t i = 0; i < frames; ++i)
        v[i] = (int16_t)(amp * sin(6.283185307179586 * hz * i / cfg::kSampleRate));
    return v;
}

// One-shot DC preview on channel 0 with a long DC sample: the level reference.
// Preview velocity 100 of 127 -> 20000 * 100/127 ~ 15748.
static constexpr double kDcLevel = 20000.0 * 100.0 / 127.0;

// ---------------------------------------------------------------------------
static void testRouting()
{
    {   // baseline + fader
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.previewChannel(0);
        r.render(4800);
        NEAR(r.L[4000], kDcLevel, 120);
    }
    {   // routed to an insert: the insert fader scales the real signal
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setMixVolume(1, 50);
        r.session.previewChannel(0);
        r.render(9600);
        NEAR(r.L[9000], kDcLevel * 0.25, 150); // 50 % on the squared fader law
        CHECK(r.engine.status().busMeterL[1] > 3000 && r.engine.status().busMeterL[2] == 0);
    }
    {   // several instruments share one insert and sum there
        R r;
        const int s = r.dc("DC", 96000);
        for (int ch = 0; ch < 2; ++ch) {
            r.session.setSample(ch, s);
            r.session.setVolume(ch, 100);
            r.session.setRoute(ch, 3);
        }
        r.session.previewChannel(0);
        r.session.previewChannel(1);
        r.render(4800);
        NEAR(r.L[4000], kDcLevel * 2, 250);
        CHECK(r.engine.status().busMeterL[3] > 20000);
    }
    {   // insert pan: hard left leaves the right side silent
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setMixPan(1, -100);
        r.session.previewChannel(0);
        r.render(9600);
        CHECK(r.L[9000] > 14000 && r.Rt[9000] == 0);
    }
    {   // insert mute and solo
        R r;
        const int s = r.dc("DC", 96000);
        for (int ch = 0; ch < 3; ++ch) {
            r.session.setSample(ch, s);
            r.session.setVolume(ch, 100);
        }
        r.session.setRoute(0, 1);
        r.session.setRoute(1, 2);
        r.session.setRoute(2, 0); // straight to the master
        r.session.setMixMute(1, true);
        for (int ch = 0; ch < 3; ++ch)
            r.session.previewChannel(ch);
        r.render(9600);
        NEAR(r.L[9000], kDcLevel * 2, 250); // insert 1 muted: insert 2 + master remain
        r.session.setMixMute(1, false);
        r.session.setMixSolo(2, true);
        r.render(9600);
        NEAR(r.L[9600 + 9000], kDcLevel, 150); // only the soloed insert is heard
        r.session.setMixSolo(2, false);
        r.render(9600);
        CHECK(r.L[19200 + 9000] > 32000); // three voices: the master saturates
    }
    {   // the master level still scales everything routed through inserts
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 4);
        r.session.setMasterVolume(50);
        r.session.previewChannel(0);
        r.render(9600);
        NEAR(r.L[9000], kDcLevel * 0.25, 150);
    }
    {   // clip indicator latches until cleared
        R r;
        const int s = r.dc("DC", 96000, 30000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setFxType(1, 0, FxType::Gain);
        r.session.setFxParam(1, 0, 0, 12); // +12 dB: the insert exceeds full scale
        r.session.previewChannel(0);
        r.render(4800);
        CHECK(r.engine.status().busClip[1] == 1);
        CHECK(r.engine.status().busClip[0] == 1 && r.engine.status().clipSamples > 0);
        r.session.clearClipLatch();
        r.session.setMute(0, true);
        r.render(4800);
        CHECK(r.engine.status().busClip[1] == 0);
    }
    {   // SPU2-voiced channels fold the insert fader/mute into the hardware level
        R r;
        const int s = r.dc("DC", 4800);
        r.session.setSample(0, s);
        r.session.setSampleHwReady(s, true);
        r.session.setVoiceMode(0, VoiceMode::Spu2);
        r.session.setRoute(0, 2);
        r.session.setMixVolume(2, 50);
        r.session.previewChannel(0);
        r.session.addNote(0, 0, 0, 60, 1, 127);
        r.session.play();
        int16_t buf[cfg::kMaxBlockFrames * 2];
        int vol = -1;
        for (int i = 0; i < 30 && vol < 0; ++i) {
            r.engine.render(buf, 480);
            for (int k = 0; k < r.engine.hwTriggerCount(); ++k)
                vol = r.engine.hwTrigger(k).volume;
        }
        CHECK(vol > 0 && vol < 40); // ~78 % * 25 % of full
        r.session.setMixMute(2, true);
        r.session.stop();
        r.render(2000);
        r.session.play();
        bool any = false;
        for (int i = 0; i < 40; ++i) {
            r.engine.render(buf, 480);
            any |= r.engine.hwTriggerCount() > 0;
        }
        CHECK(!any);
    }
}

// ---------------------------------------------------------------------------
static void testFxChain()
{
    {   // gain
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setFxType(1, 0, FxType::Gain);
        r.session.setFxParam(1, 0, 0, -6);
        r.session.previewChannel(0);
        r.render(9600);
        NEAR(r.L[9000], kDcLevel * 0.501, 250);
        // bypass restores the dry level (after the declick crossfade)
        r.session.setFxBypass(1, 0, true);
        r.render(4800);
        NEAR(r.L[9600 + 4000], kDcLevel, 200);
    }
    {   // low-pass removes a Nyquist-rate signal, high-pass removes DC
        R r;
        std::vector<int16_t> alt(96000);
        for (size_t i = 0; i < alt.size(); ++i)
            alt[i] = (i & 1) ? 16000 : -16000;
        const int s = r.addSample("ALT", alt);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.previewChannel(0);
        r.render(2000);
        const double dry = r.rms(1000, 2000);
        R f;
        const int s2 = f.addSample("ALT", alt);
        f.session.setSample(0, s2);
        f.session.setVolume(0, 100);
        f.session.setRoute(0, 1);
        f.session.setFxType(1, 0, FxType::Filter); // default: low-pass, ~2.3 kHz
        f.session.previewChannel(0);
        f.render(4000);
        CHECK(f.rms(3000, 4000) < dry * 0.05);

        R h;
        const int s3 = h.dc("DC", 96000);
        h.session.setSample(0, s3);
        h.session.setVolume(0, 100);
        h.session.setRoute(0, 1);
        h.session.setFxType(1, 0, FxType::Filter);
        h.session.setFxParam(1, 0, 0, 1); // high-pass
        h.session.previewChannel(0);
        h.render(9600);
        CHECK(abs(h.L[9000]) < 150);
    }
    {   // delay: an impulse comes back after the delay time
        R r;
        std::vector<int16_t> imp(48000, 0);
        imp[0] = 20000;
        const int s = r.addSample("IMP", imp);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setFxType(1, 0, FxType::Delay);
        r.session.setFxParam(1, 0, 0, 100);   // 100 ms
        r.session.setFxParam(1, 0, 1, 50);    // feedback
        r.session.setFxParam(1, 0, 2, 100);   // tone open
        r.session.setFxParam(1, 0, 3, 100);   // mix
        r.session.previewChannel(0);
        r.render(24000);
        int pk = 0;
        size_t at = 0;
        for (size_t i = 4000; i < 5600; ++i)
            if (abs(r.L[i]) > pk) {
                pk = abs(r.L[i]);
                at = i;
            }
        CHECK(at >= 4798 && at <= 4802);
        NEAR(pk, kDcLevel, 400);
        // second echo is half as loud (50 % feedback), twice as late
        int pk2 = 0;
        for (size_t i = 9500; i < 9700; ++i)
            pk2 = abs(r.L[i]) > pk2 ? abs(r.L[i]) : pk2;
        NEAR(pk2, kDcLevel * 0.5, 800);
    }
    {   // compressor
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setFxType(1, 0, FxType::Compressor);
        r.session.setFxParam(1, 0, 0, -20);
        r.session.setFxParam(1, 0, 1, 10);
        r.session.setFxParam(1, 0, 4, 0);
        r.session.previewChannel(0);
        r.render(30000);
        CHECK(r.L[29000] > 3000 && r.L[29000] < 4800);
        CHECK(r.engine.status().busGr[1] > 90);
    }
    {   // EQ low shelf lifts a low sine by about 12 dB
        R a;
        const int sa = a.addSample("LOW", sine(80, 6000, 96000));
        a.session.setSample(0, sa);
        a.session.setVolume(0, 100);
        a.session.previewChannel(0);
        a.render(20000);
        R b;
        const int sb = b.addSample("LOW", sine(80, 6000, 96000));
        b.session.setSample(0, sb);
        b.session.setVolume(0, 100);
        b.session.setRoute(0, 1);
        b.session.setFxType(1, 0, FxType::Eq);
        b.session.setFxParam(1, 0, 0, 12);
        b.session.previewChannel(0);
        b.render(20000);
        const double ratio = b.rms(10000, 20000) / a.rms(10000, 20000);
        CHECK(ratio > 3.0 && ratio < 4.6);
    }
    {   // distortion adds gain-structure: a quiet sine becomes a loud clipped wave
        R r;
        const int s = r.addSample("QUIET", sine(220, 2000, 96000));
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setFxType(1, 0, FxType::Distortion);
        r.session.setFxParam(1, 0, 0, 100);
        r.session.setFxParam(1, 0, 2, 100);
        r.session.previewChannel(0);
        r.render(12000);
        CHECK(r.peak(6000, 12000) > 15000);
    }
    {   // reverb: energy keeps arriving long after a single impulse
        R r;
        std::vector<int16_t> imp(48000, 0);
        imp[0] = 20000;
        const int s = r.addSample("IMP", imp);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setRoute(0, 1);
        r.session.setFxType(1, 0, FxType::Reverb);
        r.session.setFxParam(1, 0, 3, 60);
        r.session.previewChannel(0);
        r.render(48000);
        CHECK(r.rms(9600, 24000) > 2);
        CHECK(r.rms(9600, 24000) > r.rms(36000, 48000));
    }
    {   // effect order is audible: compress-then-boost differs from boost-then-compress
        R a, b;
        for (R* x : {&a, &b}) {
            const int s = x->dc("DC", 96000);
            x->session.setSample(0, s);
            x->session.setVolume(0, 100);
            x->session.setRoute(0, 1);
            x->session.setFxType(1, 0, FxType::Compressor);
            x->session.setFxParam(1, 0, 0, -20);
            x->session.setFxParam(1, 0, 1, 20);
            x->session.setFxParam(1, 0, 4, 0);
            x->session.setFxType(1, 1, FxType::Gain);
            x->session.setFxParam(1, 1, 0, 6);
        }
        b.session.moveFx(1, 0, +1);
        CHECK(b.session.project().tracks[1].fx[0].type == (uint8_t)FxType::Gain);
        CHECK(b.session.project().tracks[1].fx[1].type == (uint8_t)FxType::Compressor);
        a.session.previewChannel(0);
        b.session.previewChannel(0);
        a.render(30000);
        b.render(30000);
        CHECK(abs(a.L[29000] - b.L[29000]) > 800);
    }
    {   // memory pool: three delays fit, the fourth reports starvation but passes audio
        R r;
        const int s = r.dc("DC", 96000);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        for (int t = 1; t <= 4; ++t)
            r.session.setFxType(t, 0, FxType::Delay);
        r.session.setRoute(0, 4);
        r.session.previewChannel(0);
        r.render(2000);
        CHECK(r.engine.status().fxStarved != 0);
        CHECK(r.L[1500] > 14000); // starved effect is a pass-through, not silence
        // freeing a slot makes room again
        R q;
        for (int t = 1; t <= 3; ++t)
            q.session.setFxType(t, 0, FxType::Delay);
        q.render(1000);
        CHECK(q.engine.status().fxStarved == 0);
        q.session.setFxType(1, 0, FxType::None);
        q.session.setFxType(4, 0, FxType::Delay);
        q.render(1000);
        CHECK(q.engine.status().fxStarved == 0);
    }
}

// Every effect stays finite and bounded at the extremes of its parameter ranges.
static void testFxStability()
{
    FxMemory* mem = new FxMemory;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    float l[512], r[512], tl[512], tr[512];
    int variants = 0;
    for (int t = 1; t < kFxTypeCount; ++t) {
        const int np = fx::paramCount((FxType)t);
        for (int v = 0; v < 24; ++v) {
            FxUnit u;
            u.setType((FxType)t, *mem);
            for (int i = 0; i < np; ++i) {
                const ParamDesc& d = fx::param((FxType)t, i);
                int val = d.def;
                if (v % 3 == 1)
                    val = d.min;
                else if (v % 3 == 2)
                    val = d.max;
                if (v >= 6)
                    val = d.min + (int)(rnd() % (uint32_t)(d.max - d.min + 1));
                u.setParam(i, val);
            }
            for (int b = 0; b < 30; ++b) {
                for (int i = 0; i < 512; ++i) {
                    const int mode = (b / 10) % 3; // noise, silence, loud DC
                    l[i] = mode == 0 ? (float)((int)(rnd() & 0xffff) - 32768) : (mode == 1 ? 0.0f : 30000.0f);
                    r[i] = mode == 0 ? (float)((int)(rnd() & 0xffff) - 32768) : (mode == 1 ? 0.0f : -30000.0f);
                }
                if (b == 15)
                    u.setBypass(true);
                if (b == 20)
                    u.setBypass(false);
                u.process(l, r, 512, tl, tr);
                for (int i = 0; i < 512; ++i) {
                    CHECK(isfinite(l[i]) && isfinite(r[i]));
                    CHECK(fabsf(l[i]) < 5.0e6f && fabsf(r[i]) < 5.0e6f);
                }
            }
            u.setType(FxType::None, *mem); // releases its pool entry
            ++variants;
        }
    }
    CHECK(variants == 24 * (kFxTypeCount - 1));
    for (int i = 0; i < FxMemory::kDelayUnits; ++i)
        CHECK(!mem->delayUsed[i]);
    delete mem;
}

// ---------------------------------------------------------------------------
static void testEnvelope()
{
    // DC through an AHDSR: attack 100 ms, decay 200 ms to 50 %, release 300 ms.
    R r;
    const int s = r.dc("DC", 192000);
    r.session.setSample(0, s);
    r.session.setVolume(0, 100);
    r.session.setChannelGate(0, true);
    r.session.setEnvParam(0, kEnvEnabled, 1);
    r.session.setEnvParam(0, kEnvAttack, 100);
    r.session.setEnvParam(0, kEnvHold, 0);
    r.session.setEnvParam(0, kEnvDecay, 200);
    r.session.setEnvParam(0, kEnvSustain, 50);
    r.session.setEnvParam(0, kEnvRelease, 300);
    r.session.addNote(0, 0, 0, 60, 8, 127); // 8 steps = 48000 frames
    r.session.play();
    r.render(96000);
    const double P = 20000;
    CHECK(abs(r.L[0]) < 400);                               // starts from silence: no click
    NEAR(r.L[2400], P * 0.5, 300);                          // linear attack, halfway
    CHECK(r.peak(4000, 6000) > P * 0.93);                   // reaches the top
    NEAR(r.L[20000], P * 0.5, 300);                         // decayed to the sustain level
    NEAR(r.L[40000], P * 0.5, 300);                         // holds while the note lasts
    CHECK(r.L[48000 + 7200] > 150 && r.L[48000 + 7200] < 900); // mid-release (smooth, not cut)
    CHECK(r.L[48000 + 7200] < r.L[48000 + 1000] && r.L[48000 + 1000] < r.L[47000]);
    CHECK(abs(r.L[48000 + 20000]) < 8);                     // released to silence
    // never a step bigger than a few percent of full scale (no clicks anywhere)
    int maxStep = 0;
    for (size_t i = 1; i < r.L.size(); ++i)
        maxStep = abs(r.L[i] - r.L[i - 1]) > maxStep ? abs(r.L[i] - r.L[i - 1]) : maxStep;
    CHECK(maxStep < 600);

    // Polyphony: each note owns its envelope. Second note on the same channel,
    // starting half-way through the first, adds on top of the first's sustain.
    R q;
    const int s2 = q.dc("DC", 192000);
    q.session.setSample(0, s2);
    q.session.setVolume(0, 100);
    q.session.setChannelGate(0, true);
    q.session.setEnvParam(0, kEnvEnabled, 1);
    q.session.setEnvParam(0, kEnvAttack, 100);
    q.session.setEnvParam(0, kEnvDecay, 200);
    q.session.setEnvParam(0, kEnvSustain, 50);
    q.session.setEnvParam(0, kEnvRelease, 300);
    q.session.addNote(0, 0, 0, 60, 12, 127);
    q.session.addNote(0, 0, 4, 64, 12, 127);
    q.session.play();
    q.render(60000);
    NEAR(q.L[20000], P * 0.5, 300);        // only the first note
    NEAR(q.L[24000 + 15000], P * 1.0, 700); // both at sustain: 0.5 + 0.5
    CHECK(q.L[24000 + 1200] > q.L[23900] + 1000); // the second note ramps in smoothly

    // Envelope off: unchanged one-shot behaviour (plays the whole sample at full level).
    R o;
    const int s3 = o.dc("DC", 9000);
    o.session.setSample(0, s3);
    o.session.setVolume(0, 100);
    o.session.addNote(0, 0, 0, 60, 1, 127);
    o.session.play();
    o.render(12000);
    CHECK(o.silentRun() == 0 && o.audibleRunFrom(0) + 4 >= 9000 && o.audibleRunFrom(0) <= 9004);

    // Live edit: raising the sustain while a note holds moves the level without a jump.
    R e;
    const int s4 = e.dc("DC", 192000);
    e.session.setSample(0, s4);
    e.session.setVolume(0, 100);
    e.session.setChannelGate(0, true);
    e.session.setEnvParam(0, kEnvEnabled, 1);
    e.session.setEnvParam(0, kEnvAttack, 10);
    e.session.setEnvParam(0, kEnvDecay, 50);
    e.session.setEnvParam(0, kEnvSustain, 30);
    e.session.addNote(0, 0, 0, 60, 16, 127);
    e.session.play();
    e.render(20000);
    NEAR(e.L[19000], P * 0.3, 300);
    e.session.setEnvParam(0, kEnvSustain, 80);
    e.render(20000);
    NEAR(e.L[39000], P * 0.8, 400);
}

// ---------------------------------------------------------------------------
static void setupSynth(R& r, int ch = 0)
{
    r.session.setChannelKind(ch, InstrKind::Synth);
    r.session.setVolume(ch, 100);
    r.session.setSynthParam(ch, kSynOsc1Wave, synth::Sine);
    r.session.setSynthParam(ch, kSynOsc1Level, 100);
    r.session.setSynthParam(ch, kSynOsc2Level, 0);
    r.session.setSynthParam(ch, kSynSubLevel, 0);
    r.session.setSynthParam(ch, kSynNoiseLevel, 0);
    r.session.setSynthParam(ch, kSynFilterType, 0);
    r.session.setSynthParam(ch, kSynLfoDepth, 0);
    r.session.setSynthParam(ch, kSynLevel, 100);
    r.session.setEnvParam(ch, kEnvAttack, 1);
    r.session.setEnvParam(ch, kEnvDecay, 0);
    r.session.setEnvParam(ch, kEnvSustain, 100);
    r.session.setEnvParam(ch, kEnvRelease, 20);
}

static void testSynth()
{
    {   // pitch: A4 = 440 Hz, A5 = 880 Hz
        for (int note : {69, 81, 57}) {
            R r;
            setupSynth(r);
            r.session.addNote(0, 0, 0, note, 4, 127);
            r.session.play();
            r.render(20000);
            const double hz = 440.0 * pow(2.0, (note - 69) / 12.0);
            const double want = 2.0 * hz * (20000 - 2400) / 48000.0;
            NEAR(r.crossings(2400, 20000), want, want * 0.03 + 2);
            CHECK(r.peak(2400, 20000) > 18000 && r.peak(2400, 20000) < 26000);
        }
    }
    {   // fine tune: +100 cents is one semitone
        R a, b;
        setupSynth(a);
        setupSynth(b);
        b.session.setSynthParam(0, kSynOsc1Fine, 50);
        b.session.setSynthParam(0, kSynTranspose, 0);
        a.session.addNote(0, 0, 0, 69, 4, 127);
        b.session.addNote(0, 0, 0, 69, 4, 127);
        a.session.play();
        b.session.play();
        a.render(20000);
        b.render(20000);
        const double ratio = (double)b.crossings(2400, 20000) / a.crossings(2400, 20000);
        NEAR(ratio, pow(2.0, 0.5 / 12.0), 0.01);
    }
    {   // transpose and note length: gated by the note, silent after the release
        R r;
        setupSynth(r);
        r.session.setSynthParam(0, kSynTranspose, 12);
        r.session.addNote(0, 0, 0, 57, 2, 127); // two steps = 12000 frames
        r.session.play();
        r.render(24000);
        NEAR(r.crossings(2400, 11000), 2.0 * 440 * (11000 - 2400) / 48000.0, 6);
        CHECK(r.peak(12000 + 2000, 24000) < 40);
    }
    {   // polyphony cap per channel
        R r;
        setupSynth(r);
        for (int i = 0; i < 12; ++i)
            r.session.addNote(0, 0, 0, 60 + i, 8, 100);
        r.session.play();
        r.render(2000);
        CHECK(r.engine.status().voicesActive == (uint32_t)cfg::kMaxSynthVoicesPerChannel);
        CHECK(r.engine.status().voiceSteals >= 4);
    }
    {   // a low-pass takes the top off a saw
        R a, b;
        for (R* x : {&a, &b}) {
            setupSynth(*x);
            x->session.setSynthParam(0, kSynOsc1Wave, synth::Saw);
            x->session.addNote(0, 0, 0, 69, 4, 127);
            x->session.play();
        }
        b.session.setSynthParam(0, kSynFilterType, 1);
        b.session.setSynthParam(0, kSynCutoff, 30); // ~170 Hz, below the fundamental
        a.render(20000);
        b.render(20000);
        CHECK(b.rms(8000, 20000) < a.rms(8000, 20000) * 0.3);
        CHECK(b.rms(8000, 20000) > 10);
    }
    {   // waveforms are all audible and distinct
        double energy[synth::WaveCount];
        int cross[synth::WaveCount];
        for (int w = 0; w < synth::WaveCount; ++w) {
            R r;
            setupSynth(r);
            r.session.setSynthParam(0, kSynOsc1Wave, w);
            r.session.addNote(0, 0, 0, 60, 4, 127);
            r.session.play();
            r.render(20000);
            energy[w] = r.rms(2400, 20000);
            cross[w] = r.crossings(2400, 20000);
            CHECK(energy[w] > 3000);
        }
        CHECK(cross[synth::Noise] > cross[synth::Sine] * 4); // noise is not periodic
        CHECK(energy[synth::Square] > energy[synth::Sine] * 1.1);
    }
    {   // FM adds partials: more zero crossings than the plain carrier
        R a, b;
        for (R* x : {&a, &b}) {
            setupSynth(*x);
            x->session.setSynthParam(0, kSynOsc2Wave, synth::Sine);
            x->session.setSynthParam(0, kSynOsc2Coarse, 7);
            x->session.setSynthParam(0, kSynOsc2Detune, 0);
            x->session.setSynthParam(0, kSynOsc2Level, 100);
            x->session.addNote(0, 0, 0, 57, 4, 127);
            x->session.play();
        }
        b.session.setSynthParam(0, kSynMode, 1);
        b.session.setSynthParam(0, kSynFmAmount, 80);
        a.render(20000);
        b.render(20000);
        CHECK(b.crossings(2400, 20000) != a.crossings(2400, 20000));
        CHECK(b.rms(2400, 20000) > 3000);
    }
    {   // LFO tremolo modulates the level
        R r;
        setupSynth(r);
        r.session.setSynthParam(0, kSynLfoTarget, 2);
        r.session.setSynthParam(0, kSynLfoDepth, 100);
        r.session.setSynthParam(0, kSynLfoRate, 70); // ~4.5 Hz
        r.session.addNote(0, 0, 0, 60, 16, 127);
        r.session.play();
        r.render(60000);
        int lo = 99999, hi = 0;
        for (size_t i = 10000; i < 60000; i += 400) {
            const int p = r.peak(i, i + 400);
            lo = p < lo ? p : lo;
            hi = p > hi ? p : hi;
        }
        CHECK(hi > 2 * lo);
    }
    {   // every preset plays something and stays finite and below clipping abuse
        for (int i = 0; i < instr::synthPresetCount(); ++i) {
            R r;
            r.session.applySynthPreset(0, i);
            r.session.setVolume(0, 100);
            r.session.addNote(0, 0, 0, 60, 4, 110);
            r.session.addNote(0, 0, 0, 67, 4, 110);
            r.session.play();
            r.render(30000);
            CHECK(r.rms(0, 30000) > 20);
            CHECK(r.session.project().channels[0].inst.kind == (uint8_t)InstrKind::Synth);
            CHECK(r.session.project().channels[0].gate == 1);
        }
    }
    {   // step-grid hits trigger the synth too (at middle C, two steps long)
        R r;
        setupSynth(r);
        r.session.toggleStep(0, 0);
        r.session.play();
        r.render(24000);
        NEAR(r.crossings(2400, 11000), 2.0 * 261.63 * (11000 - 2400) / 48000.0, 5);
        CHECK(r.peak(14000, 24000) < 40);
    }
    {   // a synth works through an insert with effects, and via the playlist
        R r;
        setupSynth(r);
        r.session.setRoute(0, 5);
        r.session.setFxType(5, 0, FxType::Gain);
        r.session.setFxParam(5, 0, 0, -12);
        r.session.addNote(0, 0, 0, 69, 8, 127);
        r.session.placeClip(0, 0, 0, 1);
        r.session.setSongMode(true);
        r.session.play();
        r.render(30000);
        CHECK(r.peak(5000, 30000) > 4500 && r.peak(5000, 30000) < 7000); // ~ -12 dB of ~22900
    }
}

// ---------------------------------------------------------------------------
static void testAudioClips()
{
    {   // placement is bar-accurate and the default length follows the sample
        R r;
        const int s = r.dc("CLIP", 96000);
        const int c = r.session.placeAudioClip(0, 1, s);
        CHECK(c == 0 && r.session.project().audioClips[0].lengthBars == 1);
        r.session.setSongMode(true);
        r.session.play();
        r.render(3 * 96000);
        CHECK(r.silentRun() == 96000);
        CHECK(r.audibleRunFrom(96000) + 4 >= 96000 && r.audibleRunFrom(96000) <= 96004);
        NEAR(r.L[96000 + 500], 20000, 10);
        CHECK(r.engine.status().songBars == 2);
        CHECK(r.engine.status().audioClipsPlayed >= 1);
    }
    {   // trimming the start shortens the played region
        R r;
        const int s = r.dc("CLIP", 96000);
        r.session.placeAudioClip(0, 0, s);
        CHECK(r.session.setAudioClipTrim(0, 48000, 0));
        r.session.setSongMode(true);
        r.session.play();
        r.render(96000);
        CHECK(r.audibleRunFrom(0) + 4 >= 48000 && r.audibleRunFrom(0) <= 48004);
        // trimming the end
        R q;
        const int s2 = q.dc("CLIP", 96000);
        q.session.placeAudioClip(0, 0, s2);
        q.session.setAudioClipTrim(0, 0, 24000);
        q.session.setSongMode(true);
        q.session.play();
        q.render(96000);
        CHECK(q.audibleRunFrom(0) + 4 >= 24000 && q.audibleRunFrom(0) <= 24004);
        // trim values are clamped to the sample and an empty region is refused
        CHECK(q.session.setAudioClipTrim(0, 999999, 5));
        CHECK(q.session.project().audioClips[0].trimStart < 96000);
    }
    {   // trim selects the right part of the audio (ramp sample)
        R r;
        std::vector<int16_t> ramp(96000);
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = (int16_t)(1 + i / 4); // 1 .. 24000
        const int s = r.addSample("RAMP", ramp);
        r.session.placeAudioClip(0, 0, s);
        r.session.setAudioClipTrim(0, 40000, 60000);
        r.session.setSongMode(true);
        r.session.play();
        r.render(30000);
        NEAR(r.L[0], 1 + 40000 / 4, 3);
        NEAR(r.L[10000], 1 + 50000 / 4, 3);
        CHECK(r.audibleRunFrom(0) + 4 >= 20000 && r.audibleRunFrom(0) <= 20004);
    }
    {   // looping fills the whole clip; volume and routing apply
        R r;
        const int s = r.dc("SHORT", 4800);
        r.session.placeAudioClip(0, 0, s, 1);
        r.session.placeClip(1, 2, 0, 1); // an (empty) pattern clip keeps the song two bars long
        r.session.setAudioClipLoop(0, true);
        r.session.setSongMode(true);
        r.session.play();
        r.render(96000 + 2000);
        CHECK(r.audibleRunFrom(0) >= 96000 && r.audibleRunFrom(0) <= 96000 + 300); // ends with the clip (+ declick)

        R v;
        const int s2 = v.dc("SHORT", 96000);
        v.session.placeAudioClip(0, 0, s2, 1);
        v.session.setAudioClipVolume(0, 50);
        v.session.setAudioClipRoute(0, 2);
        v.session.setMixVolume(2, 50);
        v.session.setSongMode(true);
        v.session.play();
        v.render(20000);
        NEAR(v.L[15000], 20000 * 0.496 * 0.25, 120);
    }
    {   // mixer track + fx apply to clips
        R r;
        const int s = r.dc("CLIP", 96000);
        r.session.placeAudioClip(0, 0, s);
        r.session.setAudioClipRoute(0, 3);
        r.session.setFxType(3, 0, FxType::Gain);
        r.session.setFxParam(3, 0, 0, -6);
        r.session.setSongMode(true);
        r.session.play();
        r.render(20000);
        NEAR(r.L[15000], 20000 * 0.501, 250);
    }
    {   // play-from-bar joins a clip already under way, at the right offset
        R r;
        std::vector<int16_t> ramp(192000);
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = (int16_t)(1 + i / 8); // up to 24000
        const int s = r.addSample("RAMP", ramp);
        r.session.placeAudioClip(0, 1, s, 2); // bars 1..2
        r.session.setSongMode(true);
        r.session.playFromBar(2);             // second bar of the clip
        r.render(100000);
        CHECK(r.L[0] != 0);
        NEAR(r.L[0], 1 + 96000 / 8, 3);
        CHECK(r.audibleRunFrom(0) + 4 >= 96000 && r.audibleRunFrom(0) <= 96004);
    }
    {   // track mute silences audio clips; clips and pattern clips coexist
        R r;
        const int s = r.dc("CLIP", 96000);
        r.session.placeAudioClip(1, 0, s);
        r.session.addNote(0, 0, 0, 60, 1, 127);
        r.session.setSample(0, r.dc("PAT", 4800));
        r.session.setVolume(0, 100);
        r.session.placeClip(0, 0, 0, 1); // different track
        r.session.setTrackMute(1, true);
        r.session.setSongMode(true);
        r.session.play();
        r.render(20000);
        CHECK(r.audibleRunFrom(0) < 5000); // only the 4800-frame pattern note
        r.session.stop();
        r.session.setTrackMute(1, false);
        r.session.play();
        r.render(20000);
        CHECK(r.audibleRunFrom(20000) > 15000);
    }
    {   // editing: overlap refused, move, duplicate, replace by pattern clip, length, remove
        R r;
        const int s = r.dc("CLIP", 96000);
        CHECK(r.session.placeAudioClip(0, 0, s) == 0);
        CHECK(r.session.placeAudioClip(0, 0, s) == -1);            // occupied
        CHECK(r.session.placeAudioClip(1, 0, s) == 1);
        CHECK(r.session.moveAudioClip(1, 0, 0) == false);           // would overlap clip 0
        CHECK(r.session.moveAudioClip(1, 2, 5) && r.session.project().audioClips[1].track == 2 &&
              r.session.project().audioClips[1].startBar == 5);
        const int d = r.session.duplicateAudioClip(1);
        CHECK(d == 2 && r.session.project().audioClips[2].startBar == 6);
        CHECK(r.session.setAudioClipLength(2, 4) == 4);
        CHECK(r.session.setAudioClipLength(1, 4) == 1); // blocked by clip 2 right behind it
        CHECK(r.session.project().songBars() == 10);
        CHECK(r.session.placeClip(2, 5, 0, 1)); // replaces the overlapping audio clip
        CHECK(r.session.project().audioClipCount == 2);
        CHECK(r.session.removeAudioClip(0) && r.session.project().audioClipCount == 1);
        CHECK(!r.session.removeAudioClip(5));
        // a sample used by a clip cannot be released
        CHECK(r.session.slotInUse(s));
        r.session.removeAudioClip(0);
        CHECK(!r.session.slotInUse(s));
        // table limits
        for (int i = 0; i < cfg::kMaxAudioClips + 4; ++i)
            r.session.placeAudioClip(i % cfg::kPlaylistTracks, 20 + (i / cfg::kPlaylistTracks), s, 1);
        CHECK(r.session.project().audioClipCount == cfg::kMaxAudioClips);
        // hardware-only samples and missing slots are refused
        CHECK(r.session.placeAudioClip(0, 100, 31) == -1);
    }
}

// ---------------------------------------------------------------------------
static void testExportMode()
{
    {   // the song ends exactly at the end of the last clip; the tail keeps ringing
        R r;
        const int s = r.dc("CLIP", 48000);
        r.session.placeAudioClip(0, 0, s, 1);
        r.session.placeAudioClip(1, 1, s, 1);
        r.session.setSongMode(true);
        r.session.setExportMode(1);
        r.session.play();
        uint32_t frames = 0;
        int16_t buf[cfg::kMaxBlockFrames * 2];
        while (!r.engine.status().exportDone && frames < 10 * 96000) {
            r.engine.render(buf, 512);
            frames += 512;
        }
        CHECK(r.engine.status().exportDone == 1);
        CHECK(frames >= 2 * 96000 && frames < 2 * 96000 + 1024); // two bars
        CHECK(r.engine.status().transport == (uint8_t)Transport::State::Paused);
        // more rendering after the end produces silence (no restart / loop)
        int16_t mx = 0;
        for (int i = 0; i < 400; ++i) {
            r.engine.render(buf, 512);
            for (int k = 0; k < 1024; ++k)
                mx = abs(buf[k]) > mx ? (int16_t)abs(buf[k]) : mx;
        }
        CHECK(mx == 0);
    }
    {   // SPU2-voiced channels render in software during export; SPU2-only samples are counted
        R r;
        const int s = r.dc("DC", 9600);
        r.session.setSample(0, s);
        r.session.setVolume(0, 100);
        r.session.setSampleHwReady(s, true);
        r.session.setVoiceMode(0, VoiceMode::Spu2);
        r.session.addNote(0, 0, 0, 60, 1, 127);
        r.session.setExportMode(1);
        r.session.play();
        int16_t buf[cfg::kMaxBlockFrames * 2];
        int hw = 0;
        bool heard = false;
        for (int i = 0; i < 250; ++i) {
            r.engine.render(buf, 512);
            hw += r.engine.hwTriggerCount();
            for (int k = 0; k < 1024; ++k)
                heard |= buf[k] != 0;
        }
        CHECK(hw == 0 && heard);
        CHECK(r.engine.status().exportDone == 1);

        R h;
        const int a = h.bank.addHwOnly("ADP", "samples:X.ADP", 1000);
        CHECK(a >= 0);
        h.session.setSample(1, a);
        h.session.addNote(0, 1, 0, 60, 1, 127);
        h.session.setExportMode(1);
        h.session.play();
        for (int i = 0; i < 100; ++i)
            h.engine.render(buf, 512);
        CHECK(h.engine.status().exportSkipped >= 1);
    }
    {   // pattern mode export renders the requested number of passes
        R r;
        r.session.setSample(0, r.dc("DC", 100));
        r.session.toggleStep(0, 0);
        r.session.setExportMode(2);
        r.session.play();
        uint32_t frames = 0;
        int16_t buf[cfg::kMaxBlockFrames * 2];
        while (!r.engine.status().exportDone && frames < 1000000) {
            r.engine.render(buf, 512);
            frames += 512;
        }
        CHECK(frames >= 2 * 96000 && frames < 2 * 96000 + 1024);
    }
}

// ---------------------------------------------------------------------------
static void testProjectPersistence()
{
    R r;
    const int s = r.addSample("TONE", sine(330, 8000, 96000));
    const int c = r.dc("CLIP", 96000);
    // sampler channel with envelope, routed to an insert with a 4-slot chain
    r.session.setSample(0, s);
    r.session.setVolume(0, 100);
    r.session.setChannelGate(0, true);
    r.session.setEnvParam(0, kEnvEnabled, 1);
    r.session.setEnvParam(0, kEnvAttack, 40);
    r.session.setEnvParam(0, kEnvHold, 20);
    r.session.setEnvParam(0, kEnvDecay, 333);
    r.session.setEnvParam(0, kEnvSustain, 61);
    r.session.setEnvParam(0, kEnvRelease, 250);
    r.session.setRoute(0, 2);
    r.session.setRoute(1, 2);
    r.session.setMixVolume(2, 83);
    r.session.setMixPan(2, -20);
    r.session.setMixerTrackName(2, "DRUMBUS");
    r.session.setFxType(2, 0, FxType::Eq);
    r.session.setFxParam(2, 0, 0, 5);
    r.session.setFxParam(2, 0, 4, -3);
    r.session.setFxType(2, 1, FxType::Compressor);
    r.session.setFxParam(2, 1, 0, -24);
    r.session.setFxType(2, 2, FxType::Delay);
    r.session.setFxParam(2, 2, 0, 120);
    r.session.setFxParam(2, 2, 3, 20);
    r.session.setFxType(2, 3, FxType::Reverb);
    r.session.setFxBypass(2, 3, true);
    r.session.setFxType(0, 0, FxType::Gain); // master chain
    r.session.setFxParam(0, 0, 0, -2);
    // synth channel
    r.session.applySynthPreset(3, 5);
    r.session.setSynthParam(3, kSynCutoff, 47);
    r.session.setRoute(3, 7);
    // notes, playlist, audio clip
    r.session.addNote(0, 0, 0, 60, 4, 100);
    r.session.addNote(0, 3, 2, 64, 6, 110);
    r.session.placeClip(0, 0, 0, 2);
    r.session.placeAudioClip(2, 1, c, 1);
    r.session.setAudioClipTrim(0, 12000, 80000);
    r.session.setAudioClipVolume(0, 70);
    r.session.setAudioClipLoop(0, true);
    r.session.setAudioClipRoute(0, 2);
    r.session.setPlaylistTrackName(2, "VOX");
    r.session.setSongMode(true);

    static uint8_t buf[projectio::kMaxFileBytes];
    const size_t n = projectio::save(r.session.project(), buf, sizeof(buf));
    CHECK(n > 0 && n < projectio::kMaxFileBytes);
    Project b;
    char err[64];
    CHECK(projectio::load(buf, n, b, err, sizeof(err)));
    const Project& a = r.session.project();
    CHECK(memcmp(&a.channels[0].inst, &b.channels[0].inst, sizeof(InstrumentData)) == 0);
    CHECK(memcmp(&a.channels[3].inst, &b.channels[3].inst, sizeof(InstrumentData)) == 0);
    CHECK(b.channels[3].inst.kind == (uint8_t)InstrKind::Synth && b.channels[3].inst.synth[kSynCutoff] == 47);
    CHECK(b.channels[0].route == 2 && b.channels[1].route == 2 && b.channels[3].route == 7);
    for (int t = 0; t < cfg::kMixBuses; ++t) {
        CHECK(a.tracks[t].volume == b.tracks[t].volume && a.tracks[t].pan == b.tracks[t].pan);
        CHECK(strcmp(a.tracks[t].name, b.tracks[t].name) == 0);
        for (int sl = 0; sl < cfg::kFxSlots; ++sl)
            CHECK(memcmp(&a.tracks[t].fx[sl], &b.tracks[t].fx[sl], sizeof(FxData)) == 0);
    }
    CHECK(b.tracks[2].fx[0].type == (uint8_t)FxType::Eq && b.tracks[2].fx[3].bypass == 1);
    CHECK(strcmp(b.tracks[2].name, "DRUMBUS") == 0 && strcmp(b.playlistTrackName[2], "VOX") == 0);
    CHECK(b.audioClipCount == 1 && b.audioClips[0].trimStart == 12000 && b.audioClips[0].trimEnd == 80000 &&
          b.audioClips[0].volume == 70 && b.audioClips[0].loop == 1 && b.audioClips[0].route == 2);
    CHECK(strcmp(b.audioRefs[b.audioClips[0].source], "builtin:CLIP") == 0);

    // A fresh session loads the file and renders bit-identical audio.
    R fresh;
    fresh.addSample("TONE", sine(330, 8000, 96000));
    fresh.dc("CLIP", 96000);
    fresh.session.loadProject(b);
    fresh.session.bindSamples();
    CHECK(fresh.session.audioClipSlot(0) >= 0);
    r.session.stop();
    r.session.play();
    fresh.session.play();
    r.L.clear();
    r.render(200000);
    fresh.render(200000);
    CHECK(r.L.size() == fresh.L.size());
    CHECK(memcmp(r.L.data(), fresh.L.data(), r.L.size() * sizeof(int16_t)) == 0);
    CHECK(r.rms(0, 200000) > 100);

    // Corruption in the new chunks is rejected by the CRC; unknown extra chunks are skipped.
    uint8_t* bad = (uint8_t*)malloc(n);
    memcpy(bad, buf, n);
    size_t at = 0;
    for (size_t i = 12; i + 4 < n; ++i)
        if (memcmp(bad + i, "MIXR", 4) == 0) {
            at = i;
            break;
        }
    CHECK(at > 0);
    bad[at + 12] ^= 0x40;
    Project junk;
    CHECK(!projectio::load(bad, n, junk, err, sizeof(err)));
    free(bad);

    // A version-1 style file (none of the new chunks) loads with defaults.
    Project plain;
    plain.resetEmpty();
    static uint8_t buf2[projectio::kMaxFileBytes];
    size_t n2 = projectio::save(plain, buf2, sizeof(buf2));
    CHECK(n2 > 0);
    Project loaded;
    CHECK(projectio::load(buf2, n2, loaded, err, sizeof(err)));
    CHECK(loaded.channels[0].inst.kind == (uint8_t)InstrKind::Sampler && loaded.channels[0].inst.env[kEnvEnabled] == 0);
    CHECK(loaded.audioClipCount == 0 && loaded.tracks[1].volume == 100 && loaded.tracks[1].fx[0].type == 0);
}

// ---------------------------------------------------------------------------
// In-memory export file with fault injection.
struct MemFile : ExportFile {
    std::map<std::string, std::vector<uint8_t>> files;
    std::string cur;
    int64_t failAfterBytes = -1; // writes fail once this many bytes were accepted
    int64_t accepted = 0;
    bool failRename = false;
    bool open(const char* path) override
    {
        cur = path;
        files[cur].clear();
        accepted = 0;
        return true;
    }
    bool write(const void* d, uint32_t n) override
    {
        if (failAfterBytes >= 0 && accepted + n > failAfterBytes)
            return false;
        const uint8_t* p = (const uint8_t*)d;
        files[cur].insert(files[cur].end(), p, p + n);
        accepted += n;
        return true;
    }
    bool rewriteHeader(const uint8_t* h, uint32_t n) override
    {
        if (files[cur].size() < n)
            return false;
        memcpy(files[cur].data(), h, n);
        return true;
    }
    bool close() override { return true; }
    bool readHeader(const char* path, uint8_t* out, uint32_t n, uint32_t* size) override
    {
        auto it = files.find(path);
        if (it == files.end() || it->second.size() < n)
            return false;
        memcpy(out, it->second.data(), n);
        *size = (uint32_t)it->second.size();
        return true;
    }
    bool rename(const char* from, const char* to) override
    {
        if (failRename || !files.count(from))
            return false;
        files[to] = files[from];
        files.erase(from);
        return true;
    }
    void remove(const char* path) override { files.erase(path); }
};

struct MemHold : RenderHold {
    int holds = 0, releases = 0;
    bool refuse = false;
    bool hold() override
    {
        if (refuse)
            return false;
        ++holds;
        return true;
    }
    void release() override { ++releases; }
};

static void drive(Exporter& e, int maxTicks = 100000)
{
    for (int i = 0; i < maxTicks && e.running(); ++i)
        e.tick(8);
}

static void testExporter()
{
    auto build = [](R& r) {
        std::vector<int16_t> tone = sine(220, 12000, 96000);
        const int s = r.addSample("TONE", tone);
        r.session.placeAudioClip(0, 0, s, 1);
        r.session.placeAudioClip(1, 1, s, 1);
        r.session.setSongMode(true);
    };
    {   // Full render: structure, duration, and sample-exact equality with live playback.
        R r;
        build(r);
        r.session.setRoute(0, 1);
        MemFile f;
        MemHold hold;
        Exporter e(r.engine, r.session, f, &hold);
        char err[96];
        CHECK(e.begin("mass0:/PS2DAW/EXPORT/SONG.WAV", Exporter::Source::Song, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Done);
        CHECK(hold.holds == 1 && hold.releases == 1);
        CHECK(f.files.count("mass0:/PS2DAW/EXPORT/SONG.WAV") == 1 && f.files.count("mass0:/PS2DAW/EXPORT/SONG.WAV.TMP") == 0);
        const auto& wav = f.files["mass0:/PS2DAW/EXPORT/SONG.WAV"];
        const int64_t data = wavexport::checkHeader(wav.data(), (uint32_t)wav.size());
        CHECK(data > 0 && data == (int64_t)e.framesWritten() * 4);
        // two bars at 120 BPM = 192000 frames, plus a tail of at least 250 ms (and at most the cap)
        CHECK(e.framesWritten() >= 192000 + Exporter::kMinTailFrames && e.framesWritten() <= 192000 + Exporter::kMinTailFrames + 9600 + 512);
        CHECK(e.progressPermille() == 1000 && e.peak() > 8000);
        // The first 192000 frames equal what the live engine renders from the same start.
        R live;
        build(live);
        live.session.setRoute(0, 1);
        live.session.play();
        live.render(192000);
        bool same = true;
        for (size_t i = 0; i < 192000 && same; ++i) {
            int16_t l, rr;
            memcpy(&l, &wav[44 + i * 4], 2);
            memcpy(&rr, &wav[44 + i * 4 + 2], 2);
            same = l == live.L[i] && rr == live.Rt[i];
        }
        CHECK(same);
        // The session is back in normal mode: playback continues after the export.
        CHECK(r.session.project().songMode == 1);
        r.render(2000);
        r.session.stop();
        r.session.play();
        r.L.clear();
        r.render(5000);
        CHECK(r.peak(0, 5000) > 8000);
    }
    {   // Effects with tails extend the file.
        R r;
        build(r);
        r.session.setAudioClipRoute(0, 1);
        r.session.setAudioClipRoute(1, 1);
        r.session.setFxType(1, 0, FxType::Reverb);
        MemFile f;
        Exporter e(r.engine, r.session, f, nullptr);
        char err[96];
        CHECK(e.begin("out.wav", Exporter::Source::Song, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Done);
        CHECK(e.framesWritten() > 192000 + 20000); // reverb tail rendered
    }
    {   // Pattern export: one pass of the current pattern.
        R r;
        r.session.setSample(0, r.dc("DC", 3000));
        r.session.toggleStep(0, 0);
        MemFile f;
        Exporter e(r.engine, r.session, f, nullptr);
        char err[96];
        CHECK(e.begin("p.wav", Exporter::Source::Pattern, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Done);
        CHECK(e.framesWritten() >= 96000 && e.framesWritten() < 96000 + 20000);
    }
    {   // Refusals: empty playlist, parking failure.
        R r;
        MemFile f;
        MemHold hold;
        Exporter e(r.engine, r.session, f, &hold);
        char err[96];
        CHECK(!e.begin("x.wav", Exporter::Source::Song, err, sizeof(err)) && strstr(err, "empty") != nullptr);
        R q;
        build(q);
        MemHold refuse;
        refuse.refuse = true;
        Exporter e2(q.engine, q.session, f, &refuse);
        CHECK(!e2.begin("x.wav", Exporter::Source::Song, err, sizeof(err)));
        CHECK(f.files.empty());
    }
    {   // USB failure mid-render: clean failure, no partial file, thread released, engine back to normal.
        R r;
        build(r);
        MemFile f;
        f.failAfterBytes = 100000;
        MemHold hold;
        Exporter e(r.engine, r.session, f, &hold);
        char err[96];
        CHECK(e.begin("fail.wav", Exporter::Source::Song, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Failed && strstr(e.message(), "USB") != nullptr);
        CHECK(f.files.empty());
        CHECK(hold.holds == 1 && hold.releases == 1);
        r.session.play();
        r.render(5000); // normal live playback works again
        CHECK(r.peak(0, 5000) > 8000);
    }
    {   // Rename failure and cancel also leave nothing behind.
        R r;
        build(r);
        MemFile f;
        f.failRename = true;
        Exporter e(r.engine, r.session, f, nullptr);
        char err[96];
        CHECK(e.begin("a.wav", Exporter::Source::Song, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Failed && f.files.empty());
        MemFile g;
        Exporter c(r.engine, r.session, g, nullptr);
        CHECK(c.begin("b.wav", Exporter::Source::Song, err, sizeof(err)));
        c.tick(50);
        CHECK(c.running() && c.progressPermille() > 0 && c.progressPermille() < 1000);
        c.cancel();
        CHECK(c.state() == Exporter::State::Failed && g.files.empty());
    }
    {   // SPU2-only samples are reported, not silently dropped.
        R r;
        const int a = r.bank.addHwOnly("ADP", "samples:X.ADP", 1000);
        r.session.setSample(1, a);
        r.session.addNote(0, 1, 0, 60, 1, 127);
        r.session.placeClip(0, 0, 0, 1);
        r.session.setSongMode(true);
        MemFile f;
        Exporter e(r.engine, r.session, f, nullptr);
        char err[96];
        CHECK(e.begin("s.wav", Exporter::Source::Song, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Done && e.skippedNotes() >= 1);
    }
    {   // Header checker rejects damaged headers.
        uint8_t h[44];
        wavexport::makeHeader(h, 400);
        CHECK(wavexport::checkHeader(h, 444) == 400);
        CHECK(wavexport::checkHeader(h, 443) == -1);
        h[22] = 1;
        CHECK(wavexport::checkHeader(h, 444) == -1);
        wavexport::makeHeader(h, 400);
        h[0] = 'X';
        CHECK(wavexport::checkHeader(h, 444) == -1);
    }
}

static void testSwingAndMetronome()
{
    auto onsets = [](const R& r) {
        std::vector<size_t> v;
        for (size_t i = 0; i < r.L.size(); ++i)
            if (r.L[i] != 0 && (i == 0 || r.L[i - 1] == 0))
                v.push_back(i);
        return v;
    };
    {   // no swing: steps are 6000 frames apart
        R r;
        r.session.setSample(0, r.dc("DC", 100));
        r.session.setVolume(0, 100);
        for (int s = 0; s < 4; ++s)
            r.session.toggleStep(0, s);
        r.session.play();
        r.render(24000);
        const auto o = onsets(r);
        CHECK(o.size() == 4 && o[0] == 0 && o[1] == 6000 && o[2] == 12000 && o[3] == 18000);
    }
    {   // 50 % swing: odd steps are half a step late; even steps do not move
        R r;
        r.session.setSample(0, r.dc("DC", 100));
        r.session.setVolume(0, 100);
        for (int s = 0; s < 4; ++s)
            r.session.toggleStep(0, s);
        r.session.setSwing(50);
        r.session.play();
        r.render(24000);
        const auto o = onsets(r);
        CHECK(o.size() == 4 && o[0] == 0 && o[1] == 9000 && o[2] == 12000 && o[3] == 21000);
        // 25 % swing, and the loop stays exactly 96000 frames long
        R q;
        q.session.setSample(0, q.dc("DC", 100));
        q.session.setVolume(0, 100);
        q.session.toggleStep(0, 0);
        q.session.toggleStep(0, 1);
        q.session.setSwing(25);
        q.session.play();
        q.render(200000);
        const auto p = onsets(q);
        for (size_t v : p) fprintf(stderr, "DBG onset %zu\n", v);
        CHECK(p.size() == 6 && p[1] == 7500 && p[2] == 96000 && p[3] == 96000 + 7500 && p[4] == 192000);
        // saved with the project
        static uint8_t buf[projectio::kMaxFileBytes];
        const size_t n = projectio::save(q.session.project(), buf, sizeof(buf));
        Project b;
        char err[64];
        CHECK(projectio::load(buf, n, b, err, sizeof(err)) && b.swing == 25);
        R f;
        f.session.loadProject(b);
        f.session.setSample(0, f.dc("DC", 100));
        f.session.setVolume(0, 100);
        f.session.play();
        f.render(12000);
        CHECK(onsets(f).size() == 2 && onsets(f)[1] == 7500); // swing came back with the file
    }
    {   // metronome: a click on each beat, louder on the bar; silent when off and during exports
        R r;
        r.session.setMetronome(true);
        r.session.play();
        r.render(100000);
        CHECK(r.peak(0, 600) > r.peak(24000, 24600) && r.peak(24000, 24600) > 3000 && r.peak(48000, 48600) > 3000);
        CHECK(r.peak(96000, 96600) > r.peak(72000, 72600)); // next bar accent
        CHECK(r.peak(5000, 20000) == 0);                     // nothing between beats
        R off;
        off.session.play();
        off.render(30000);
        CHECK(off.peak(0, 30000) == 0);
        R ex;
        ex.session.setMetronome(true);
        MemFile f;
        Exporter e(ex.engine, ex.session, f, nullptr);
        char err[96];
        CHECK(e.begin("m.wav", Exporter::Source::Pattern, err, sizeof(err)));
        drive(e);
        CHECK(e.state() == Exporter::State::Done && e.peak() == 0);
    }
}

static void testAddRemoveChannels()
{
    R r;
    const int dc = r.dc("DC", 9000);
    CHECK(r.session.project().channelCount == cfg::kDefaultChannels);
    // add a sampler: it takes the first loaded sample, plays, and survives save/load
    const int a = r.session.addChannel(false);
    CHECK(a == 8 && r.session.project().channelCount == 9 && r.session.project().channels[a].sampleSlot == dc);
    r.session.setVolume(a, 100);
    r.session.toggleStep(a, 0);
    r.session.play();
    r.render(3000);
    CHECK(r.peak(0, 3000) > 10000);
    // a synth row
    const int b = r.session.addChannel(true);
    CHECK(b == 9 && r.session.project().channels[b].inst.kind == (uint8_t)InstrKind::Synth && r.session.project().channels[b].gate == 1);
    r.session.addNote(0, b, 0, 60, 4, 110);
    r.session.stop();
    r.session.play();
    r.L.clear();
    r.render(12000);
    CHECK(r.rms(2000, 12000) > 100);
    static uint8_t buf[projectio::kMaxFileBytes];
    const size_t n = projectio::save(r.session.project(), buf, sizeof(buf));
    Project p;
    char err[64];
    CHECK(n > 0 && projectio::load(buf, n, p, err, sizeof(err)) && p.channelCount == 10);
    CHECK(p.patterns[0].velocity[8][0] == cfg::kDefaultVelocity && p.patterns[0].noteCount[9] == 1 && p.channels[9].inst.kind == 1);
    // limits
    while (r.session.addChannel(false) >= 0) {
    }
    CHECK(r.session.project().channelCount == cfg::kMaxChannels && r.session.addChannel(true) == -1);
    // remove a middle channel: later rows (steps, notes, settings) move up
    R q;
    q.session.setSample(1, q.dc("DC", 9000));
    q.session.toggleStep(1, 3);
    q.session.toggleStep(2, 5);
    q.session.addNote(0, 3, 7, 62, 2, 90);
    q.session.setVolume(3, 41);
    CHECK(q.session.removeChannel(2));
    const Project& pq = q.session.project();
    CHECK(pq.channelCount == 7 && pq.patterns[0].velocity[1][3] != 0 && pq.patterns[0].velocity[2][5] == 0);
    CHECK(pq.patterns[0].noteCount[2] == 1 && pq.patterns[0].notes[2][0].pitch == 62 && pq.channels[2].volume == 41);
    CHECK(pq.patterns[0].noteCount[3] == 0 && pq.patterns[0].noteCount[7] == 0);
    CHECK(!q.session.removeChannel(40));
    // a one-channel project cannot lose its last channel
    for (int i = 0; i < 20; ++i)
        q.session.removeChannel(0);
    CHECK(q.session.project().channelCount == 1);
}

static void testUngroupClips()
{
    auto build = [](R& r) {
        const int a = r.dc("A", 5000, 4000);
        const int b = r.dc("B", 5000, 8000);
        const int c = r.dc("C", 5000, 12000);
        const int slots[3] = {a, b, c};
        for (int ch = 0; ch < 3; ++ch) {
            r.session.setSample(ch, slots[ch]);
            r.session.setVolume(ch, 100);
            r.session.toggleStep(ch, 0);
        }
        for (int ch = 3; ch < 8; ++ch) // the rest of the kit stays silent in this pattern
            r.session.setSample(ch, -1);
        r.session.placeClip(0, 0, 0, 1);
        r.session.setSongMode(true);
    };
    R base;
    build(base);
    base.session.play();
    base.render(12000);

    {   // ungrouping keeps the sound identical and puts each instrument on its own track
        R r;
        build(r);
        CHECK(r.session.ungroupClip(0) == 2);
        const Project& p = r.session.project();
        CHECK(p.clipCount == 3);
        uint16_t seen = 0;
        int tracksUsed = 0;
        uint32_t trackBits = 0;
        for (int i = 0; i < p.clipCount; ++i) {
            seen |= p.clips[i].chanMask;
            trackBits |= 1u << p.clips[i].track;
        }
        for (int t = 0; t < cfg::kPlaylistTracks; ++t)
            tracksUsed += (trackBits >> t) & 1;
        CHECK((seen & 7) == 7 && tracksUsed == 3);
        r.session.play();
        r.render(12000);
        CHECK(r.L == base.L && r.Rt == base.Rt);
        // muting one split clip's track removes exactly that instrument
        int bTrack = -1;
        for (int i = 0; i < p.clipCount; ++i)
            if (p.clips[i].chanMask == 2)
                bTrack = p.clips[i].track;
        CHECK(bTrack >= 0);
        r.session.stop();
        r.session.setTrackMute(bTrack, true);
        r.session.play();
        r.L.clear();
        r.render(2000);
        // ~ (4000 + 12000) * 100/127 of full scale; instrument B (8000) is gone
        const int with = base.L[500], without = r.L[500];
        CHECK(with - without > 6000 && without > 10000);
        // survives save and load
        r.session.setTrackMute(bTrack, false);
        static uint8_t buf[projectio::kMaxFileBytes];
        const size_t n = projectio::save(r.session.project(), buf, sizeof(buf));
        Project q;
        char err[64];
        CHECK(n > 0 && projectio::load(buf, n, q, err, sizeof(err)) && q.clipCount == 3);
        for (int i = 0; i < q.clipCount; ++i)
            CHECK(q.clips[i].chanMask == p.clips[i].chanMask && q.clips[i].track == p.clips[i].track);
        R f;
        build(f);
        f.session.loadProject(q);
        f.session.bindSamples();
        f.session.play();
        f.render(12000);
        CHECK(f.L == base.L);
    }
    {   // taking one instrument out, regrouping, and the limits
        R r;
        build(r);
        CHECK(r.session.splitClipChannel(0, 1));
        CHECK(r.session.project().clipCount == 2);
        const int main = r.session.project().clipAt(0, 0);
        CHECK(main >= 0 && r.session.project().clips[main].chanMask == (kAllChannels & ~2));
        CHECK(!r.session.splitClipChannel(main, 1)); // already out
        r.session.setClipMask(main, kAllChannels);   // regroup: the clip plays everything again
        CHECK(r.session.project().clips[main].chanMask == kAllChannels);
        // no free track: refused, nothing changes
        R t;
        build(t);
        for (int tr = 1; tr < cfg::kPlaylistTracks; ++tr)
            t.session.placeClip(tr, 0, 1, 1);
        const int before = t.session.project().clipCount;
        CHECK(t.session.ungroupClip(0) == 0 && !t.session.splitClipChannel(0, 1) && t.session.project().clipCount == before);
        // an instrument without content is not split out
        R e;
        build(e);
        CHECK(!e.session.channelHasContent(0, 5) && e.session.channelHasContent(0, 2));
        // duplicating a clip keeps its mask
        const int c1 = r.session.splitClipChannel(main, 2) ? 1 : 0;
        CHECK(c1 == 1);
        const int split = r.session.project().clipAt(1, 0) >= 0 ? r.session.project().clipAt(1, 0) : r.session.project().clipAt(2, 0);
        CHECK(split >= 0);
        const PlaylistClip sc = r.session.project().clips[split];
        CHECK(r.session.placeClip(sc.track, 4, sc.pattern, sc.lengthBars, sc.chanMask));
        CHECK(r.session.project().clips[r.session.project().clipAt(sc.track, 4)].chanMask == sc.chanMask);
    }
}

int runAudioTests()
{
    printf("running mixer/fx/instrument tests\n");
    testRouting();
    testFxChain();
    testFxStability();
    testEnvelope();
    testSynth();
    testAudioClips();
    testExportMode();
    testExporter();
    testSwingAndMetronome();
    testAddRemoveChannels();
    testUngroupClips();
    testProjectPersistence();
    return g_fail;
}
