#include "audio/instrument.hpp"

#include <math.h>
#include <string.h>

namespace {

const char* const kWaves[] = {"SINE", "SQUARE", "SAW", "TRIANGLE", "NOISE"};
const char* const kModes[] = {"MIX", "FM"};
const char* const kFilters[] = {"OFF", "LOW-PASS", "HIGH-PASS", "BAND-PASS"};
const char* const kLfoDest[] = {"PITCH", "FILTER", "AMP"};

// clang-format off
const ParamDesc kEnvParams[cfg::kEnvParams] = {
    {"ENVELOPE", 0, 1,    0,   1, Fmt::OnOff,  nullptr},
    {"ATTACK",   1, 5000, 5,   5, Fmt::Ms,     nullptr},
    {"HOLD",     0, 2000, 0,   5, Fmt::Ms,     nullptr},
    {"DECAY",    0, 5000, 200, 10, Fmt::Ms,    nullptr},
    {"SUSTAIN",  0, 100,  100, 2, Fmt::Percent, nullptr},
    {"RELEASE",  5, 5000, 50,  10, Fmt::Ms,    nullptr},
};

constexpr int kSynthCount = 20;
const ParamDesc kSynthParams[kSynthCount] = {
    {"OSC1 WAVE",  0, 4,    2,   1, Fmt::Enum,    kWaves},
    {"OSC1 LEVEL", 0, 100,  100, 5, Fmt::Percent, nullptr},
    {"OSC1 FINE",  -50, 50, 0,   1, Fmt::Cents,   nullptr},
    {"OSC2 WAVE",  0, 4,    2,   1, Fmt::Enum,    kWaves},
    {"OSC2 LEVEL", 0, 100,  0,   5, Fmt::Percent, nullptr},
    {"OSC2 COARSE", -24, 24, 0,  1, Fmt::Semis,   nullptr},
    {"OSC2 DETUNE", -50, 50, 7,  1, Fmt::Cents,   nullptr},
    {"SUB LEVEL",  0, 100,  0,   5, Fmt::Percent, nullptr},
    {"NOISE",      0, 100,  0,   5, Fmt::Percent, nullptr},
    {"OSC MODE",   0, 1,    0,   1, Fmt::Enum,    kModes},
    {"FM AMOUNT",  0, 100,  30,  5, Fmt::Percent, nullptr},
    {"FILTER",     0, 3,    1,   1, Fmt::Enum,    kFilters},
    {"CUTOFF",     0, 100,  80,  2, Fmt::Cutoff, nullptr},
    {"RESONANCE",  0, 100,  15,  5, Fmt::Percent, nullptr},
    {"FILTER ENV", -100, 100, 0, 5, Fmt::Percent, nullptr},
    {"LFO RATE",   0, 100,  40,  2, Fmt::LfoRate, nullptr},
    {"LFO DEPTH",  0, 100,  0,   5, Fmt::Percent, nullptr},
    {"LFO DEST",   0, 2,    0,   1, Fmt::Enum,    kLfoDest},
    {"TRANSPOSE",  -24, 24, 0,   1, Fmt::Semis,   nullptr},
    {"LEVEL",      0, 100,  80,  5, Fmt::Percent, nullptr},
};
// clang-format on

using P = instr::Preset;
// env: enabled, A, H, D, S, R
// synth order: see SynthParam
const P kPresets[] = {
    {"INIT SAW", {1, 5, 0, 200, 80, 80},
     {2, 100, 0, 2, 0, 0, 7, 0, 0, 0, 30, 1, 85, 10, 0, 40, 0, 0, 0, 80, 0, 0, 0, 0}},
    {"PLUCK", {1, 1, 0, 260, 0, 120},
     {2, 100, 0, 2, 60, 0, 9, 0, 0, 0, 30, 1, 35, 25, 60, 40, 0, 0, 0, 80, 0, 0, 0, 0}},
    {"SUB BASS", {1, 3, 0, 120, 90, 70},
     {0, 100, 0, 1, 0, 0, 0, 60, 0, 0, 30, 1, 55, 5, 0, 40, 0, 0, -12, 85, 0, 0, 0, 0}},
    {"SQUARE LEAD", {1, 4, 0, 150, 75, 140},
     {1, 100, 0, 1, 70, 0, 6, 0, 0, 0, 30, 1, 72, 20, 0, 52, 12, 0, 0, 70, 0, 0, 0, 0}},
    {"WARM PAD", {1, 600, 0, 500, 80, 900},
     {2, 100, 0, 2, 80, 0, 12, 0, 0, 0, 30, 1, 52, 10, 0, 30, 6, 0, 0, 70, 0, 0, 0, 0}},
    {"FM BELL", {1, 1, 0, 900, 0, 700},
     {0, 100, 0, 0, 100, 19, 20, 0, 0, 1, 45, 0, 80, 0, 0, 40, 0, 0, 0, 75, 0, 0, 0, 0}},
    {"WOBBLE BASS", {1, 5, 0, 200, 100, 100},
     {2, 100, 0, 1, 0, 0, 0, 50, 0, 0, 30, 1, 42, 55, 0, 55, 75, 1, -12, 80, 0, 0, 0, 0}},
    {"NOISE HAT", {1, 1, 0, 70, 0, 30},
     {4, 100, 0, 4, 0, 0, 0, 0, 0, 0, 30, 2, 80, 20, 0, 40, 0, 0, 0, 60, 0, 0, 0, 0}},
    {"TRI KEYS", {1, 3, 0, 420, 45, 220},
     {3, 100, 0, 0, 40, 12, 3, 0, 0, 0, 30, 1, 78, 5, 0, 40, 0, 0, 0, 80, 0, 0, 0, 0}},
};

struct EnvPreset {
    const char* name;
    int16_t env[cfg::kEnvParams];
};
const EnvPreset kEnvPresets[] = {
    {"OFF (ONE-SHOT)", {0, 5, 0, 200, 100, 50}},
    {"GATE", {1, 1, 0, 0, 100, 20}},
    {"PLUCK", {1, 1, 0, 300, 0, 60}},
    {"PAD", {1, 400, 0, 300, 100, 800}},
    {"SWELL", {1, 900, 0, 0, 100, 400}},
    {"STAB", {1, 1, 40, 120, 30, 90}},
};

float g_filterG[1024];
bool g_ready = false;

void buildTables()
{
    if (g_ready)
        return;
    for (int i = 0; i <= 1024; ++i)
        synth::g_sine[i] = (int16_t)(32767.0 * sin(6.283185307179586 * i / 1024.0));
    for (int i = 0; i < 1024; ++i) {
        float fc = 20.0f * powf(900.0f, (float)i / 1023.0f);
        const float maxFc = 0.45f * (float)cfg::kSampleRate;
        if (fc > maxFc)
            fc = maxFc;
        g_filterG[i] = tanf(3.14159265f * fc / (float)cfg::kSampleRate);
    }
    g_ready = true; // idempotent: a racing second init writes identical values
}

} // namespace

namespace instr {

int envParamCount() { return cfg::kEnvParams; }
const ParamDesc& envParam(int i) { return kEnvParams[i < 0 ? 0 : (i >= cfg::kEnvParams ? cfg::kEnvParams - 1 : i)]; }
int synthParamCount() { return kSynthCount; }
const ParamDesc& synthParam(int i) { return kSynthParams[i < 0 ? 0 : (i >= kSynthCount ? kSynthCount - 1 : i)]; }

void setSynthDefaults(InstrumentData& d)
{
    for (int i = 0; i < cfg::kSynthParams; ++i)
        d.synth[i] = i < kSynthCount ? kSynthParams[i].def : 0;
}

void setDefaults(InstrumentData& d)
{
    memset(&d, 0, sizeof(d));
    d.kind = (uint8_t)InstrKind::Sampler;
    for (int i = 0; i < cfg::kEnvParams; ++i)
        d.env[i] = kEnvParams[i].def;
    setSynthDefaults(d);
}

void sanitize(InstrumentData& d)
{
    if (d.kind > (uint8_t)InstrKind::Synth)
        d.kind = (uint8_t)InstrKind::Sampler;
    for (int i = 0; i < cfg::kEnvParams; ++i)
        d.env[i] = (int16_t)params::clampTo(kEnvParams[i], d.env[i]);
    for (int i = 0; i < cfg::kSynthParams; ++i)
        d.synth[i] = i < kSynthCount ? (int16_t)params::clampTo(kSynthParams[i], d.synth[i]) : 0;
}

int synthPresetCount() { return (int)(sizeof(kPresets) / sizeof(kPresets[0])); }
const Preset& synthPreset(int i)
{
    const int n = synthPresetCount();
    return kPresets[i < 0 ? 0 : (i >= n ? n - 1 : i)];
}

void applySynthPreset(InstrumentData& d, int i)
{
    const Preset& p = synthPreset(i);
    memcpy(d.env, p.env, sizeof(d.env));
    memcpy(d.synth, p.synth, sizeof(d.synth));
    sanitize(d);
}

int envPresetCount() { return (int)(sizeof(kEnvPresets) / sizeof(kEnvPresets[0])); }
const char* envPresetName(int i) { return kEnvPresets[i < 0 ? 0 : (i >= envPresetCount() ? envPresetCount() - 1 : i)].name; }
void applyEnvPreset(InstrumentData& d, int i)
{
    const EnvPreset& p = kEnvPresets[i < 0 ? 0 : (i >= envPresetCount() ? envPresetCount() - 1 : i)];
    memcpy(d.env, p.env, sizeof(d.env));
    sanitize(d);
}

} // namespace instr

namespace synth {

int16_t g_sine[1025];

void initTables() { buildTables(); }

uint32_t phaseInc(int note, int cents)
{
    const float semis = (float)(note - 69) + (float)cents * 0.01f;
    const float hz = 440.0f * exp2f(semis / 12.0f);
    const float inc = hz / (float)cfg::kSampleRate * 4294967296.0f;
    if (inc >= 2147483648.0f) // never above Nyquist
        return 0x7fffffffu;
    return inc <= 0.0f ? 0u : (uint32_t)inc;
}

float filterG(int fineIdx)
{
    initTables();
    if (fineIdx < 0)
        fineIdx = 0;
    if (fineIdx > 1023)
        fineIdx = 1023;
    return g_filterG[fineIdx];
}

} // namespace synth
