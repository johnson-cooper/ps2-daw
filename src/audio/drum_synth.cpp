#include "audio/drum_synth.hpp"

#include <math.h>
#include <stdlib.h>

namespace drumsynth {
namespace {

constexpr float kSr = (float)cfg::kSampleRate;

// Deterministic white noise in [-1, 1).
struct Noise {
    uint32_t state = 0x12345678u;
    float next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float)(int32_t)state * (1.0f / 2147483648.0f);
    }
};

// Sine of a phase given in turns, via a corrected parabola (max error ~0.1%).
float sinTurns(float phase)
{
    phase -= floorf(phase);
    float x = phase * 6.28318531f - 3.14159265f; // [-pi, pi)
    float y = 1.27323954f * x - 0.405284735f * x * fabsf(x);
    y = 0.225f * (y * fabsf(y) - y) + y;
    return -y; // shift by pi so phase 0 starts at 0 going positive
}

float decayCoeff(float seconds)
{
    return expf(-1.0f / (seconds * kSr));
}

struct Buffer {
    float* data = nullptr;
    int frames = 0;
    explicit Buffer(float seconds)
    {
        frames = (int)(seconds * kSr);
        data = (float*)calloc((size_t)frames, sizeof(float));
    }
    ~Buffer() { free(data); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

void renderKick(Buffer& b)
{
    Noise n;
    float phase = 0.0f, pitchEnv = 1.0f, ampEnv = 1.0f;
    const float pd = decayCoeff(0.030f), ad = decayCoeff(0.160f);
    for (int i = 0; i < b.frames; ++i) {
        float f = 45.0f + 115.0f * pitchEnv;
        phase += f / kSr;
        float s = sinTurns(phase) * ampEnv;
        if (i < 96) // ~2 ms beater click
            s += n.next() * 0.35f * (1.0f - (float)i / 96.0f);
        b.data[i] = s;
        pitchEnv *= pd;
        ampEnv *= ad;
    }
}

void renderSnare(Buffer& b)
{
    Noise n;
    float phase = 0.0f, toneEnv = 1.0f, noiseEnv = 1.0f, prev = 0.0f;
    const float td = decayCoeff(0.045f), nd = decayCoeff(0.070f);
    for (int i = 0; i < b.frames; ++i) {
        phase += 185.0f / kSr;
        float raw = n.next();
        float hp = raw - prev; // first-difference high-pass: brighter rattle
        prev = raw;
        b.data[i] = sinTurns(phase) * toneEnv * 0.55f + hp * noiseEnv * 0.45f;
        toneEnv *= td;
        noiseEnv *= nd;
    }
}

void renderHat(Buffer& b, float tau, float gain)
{
    Noise n;
    n.state = 0x9e3779b9u;
    float env = 1.0f, p1 = 0.0f, p2 = 0.0f;
    const float d = decayCoeff(tau);
    for (int i = 0; i < b.frames; ++i) {
        float raw = n.next();
        float h1 = raw - p1; // two cascaded differences: steep high end
        p1 = raw;
        float h2 = h1 - p2;
        p2 = h1;
        b.data[i] = h2 * env * gain;
        env *= d;
    }
}

void renderClap(Buffer& b)
{
    Noise n;
    n.state = 0xc0ffee11u;
    float lp = 0.0f, prevLp = 0.0f;
    const float tail = decayCoeff(0.090f), burst = decayCoeff(0.008f);
    float tailEnv = 1.0f;
    const int burstStarts[3] = {0, (int)(0.011f * kSr), (int)(0.023f * kSr)};
    for (int i = 0; i < b.frames; ++i) {
        // Band-pass the noise around ~1.2 kHz: one-pole low-pass, then difference.
        lp += 0.35f * (n.next() - lp);
        float bp = lp - prevLp;
        prevLp = lp;
        float env = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (i >= burstStarts[k] && i < burstStarts[k] + (int)(0.010f * kSr))
                env += powf(burst, (float)(i - burstStarts[k]));
        }
        if (i >= burstStarts[2]) {
            env += tailEnv * 0.6f;
            tailEnv *= tail;
        }
        b.data[i] = bp * env * 2.2f;
    }
}

void renderTom(Buffer& b)
{
    float phase = 0.0f, pitchEnv = 1.0f, ampEnv = 1.0f;
    const float pd = decayCoeff(0.090f), ad = decayCoeff(0.130f);
    for (int i = 0; i < b.frames; ++i) {
        phase += (78.0f + 52.0f * pitchEnv) / kSr;
        b.data[i] = sinTurns(phase) * ampEnv;
        pitchEnv *= pd;
        ampEnv *= ad;
    }
}

void renderRim(Buffer& b)
{
    float p1 = 0.0f, p2 = 0.0f, e1 = 1.0f, e2 = 1.0f;
    const float d1 = decayCoeff(0.007f), d2 = decayCoeff(0.016f);
    for (int i = 0; i < b.frames; ++i) {
        p1 += 1700.0f / kSr;
        p2 += 455.0f / kSr;
        b.data[i] = sinTurns(p1) * e1 * 0.5f + sinTurns(p2) * e2 * 0.6f;
        e1 *= d1;
        e2 *= d2;
    }
}

void renderBass(Buffer& b)
{
    // Filtered sawtooth at A1 (55 Hz) with a decaying low-pass sweep.
    float phase = 0.0f, lp1 = 0.0f, lp2 = 0.0f, ampEnv = 1.0f, cutEnv = 1.0f;
    const float ad = decayCoeff(0.300f), cd = decayCoeff(0.080f);
    const int attack = (int)(0.003f * kSr);
    for (int i = 0; i < b.frames; ++i) {
        phase += 55.0f / kSr;
        phase -= floorf(phase);
        float saw = phase * 2.0f - 1.0f;
        float cutoff = 0.015f + 0.12f * cutEnv; // one-pole coefficient
        lp1 += cutoff * (saw - lp1);
        lp2 += cutoff * (lp1 - lp2);
        float a = (i < attack) ? (float)i / (float)attack : ampEnv;
        b.data[i] = lp2 * a;
        if (i >= attack)
            ampEnv *= ad;
        cutEnv *= cd;
    }
}

void renderTestTone(Buffer& b)
{
    // 1 kHz reference beep with 5 ms fades: easy to recognise on any TV.
    const int fade = (int)(0.005f * kSr);
    float phase = 0.0f;
    for (int i = 0; i < b.frames; ++i) {
        phase += 1000.0f / kSr;
        float g = 1.0f;
        if (i < fade)
            g = (float)i / (float)fade;
        else if (i > b.frames - fade)
            g = (float)(b.frames - i) / (float)fade;
        b.data[i] = sinTurns(phase) * g;
    }
}

// Normalises to `peak` (fraction of full scale), fades the last few ms to
// avoid an end click, and converts to int16. Returns nullptr on OOM.
int16_t* finish(const Buffer& b, float peak)
{
    if (!b.data)
        return nullptr;
    float maxAbs = 1e-9f;
    for (int i = 0; i < b.frames; ++i)
        maxAbs = fmaxf(maxAbs, fabsf(b.data[i]));
    const float scale = peak * 32767.0f / maxAbs;
    int16_t* out = (int16_t*)malloc((size_t)b.frames * sizeof(int16_t));
    if (!out)
        return nullptr;
    const int fade = 192;
    for (int i = 0; i < b.frames; ++i) {
        float v = b.data[i] * scale;
        int remaining = b.frames - i;
        if (remaining < fade)
            v *= (float)remaining / (float)fade;
        int s = (int)v;
        out[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
    }
    return out;
}

struct Spec {
    Kind kind;
    float seconds;
    float peak;
};

const Spec kSpecs[] = {
    {Kind::Kick, 0.45f, 0.89f},
    {Kind::Snare, 0.28f, 0.70f},
    {Kind::ClosedHat, 0.08f, 0.45f},
    {Kind::OpenHat, 0.50f, 0.42f},
    {Kind::Clap, 0.35f, 0.65f},
    {Kind::Tom, 0.40f, 0.75f},
    {Kind::Rim, 0.07f, 0.55f},
    {Kind::Bass, 0.60f, 0.80f},
    {Kind::TestTone, 0.25f, 0.50f},
};

} // namespace

const char* name(Kind k)
{
    switch (k) {
    case Kind::Kick: return "KICK";
    case Kind::Snare: return "SNARE";
    case Kind::ClosedHat: return "CL HAT";
    case Kind::OpenHat: return "OP HAT";
    case Kind::Clap: return "CLAP";
    case Kind::Tom: return "TOM";
    case Kind::Rim: return "RIM";
    case Kind::Bass: return "BASS";
    case Kind::TestTone: return "TONE 1K";
    default: return "?";
    }
}

int generateKit(SampleBank& bank)
{
    int made = 0;
    for (const Spec& spec : kSpecs) {
        Buffer b(spec.seconds);
        if (!b.data)
            break;
        switch (spec.kind) {
        case Kind::Kick: renderKick(b); break;
        case Kind::Snare: renderSnare(b); break;
        case Kind::ClosedHat: renderHat(b, 0.016f, 1.0f); break;
        case Kind::OpenHat: renderHat(b, 0.130f, 1.0f); break;
        case Kind::Clap: renderClap(b); break;
        case Kind::Tom: renderTom(b); break;
        case Kind::Rim: renderRim(b); break;
        case Kind::Bass: renderBass(b); break;
        case Kind::TestTone: renderTestTone(b); break;
        default: break;
        }
        int16_t* pcm = finish(b, spec.peak);
        if (!pcm)
            break;
        if (bank.add(name(spec.kind), pcm, (uint32_t)b.frames, cfg::kSampleRate, 1, true) < 0) {
            free(pcm);
            break;
        }
        ++made;
    }
    return made;
}

} // namespace drumsynth
