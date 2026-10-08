// Instrument layer: what a rack channel *is* besides its sample.
//
//  * Sampler (default): plays the channel's sample, optionally shaped by an
//    AHDSR envelope.
//  * Synth: the native two-oscillator subtractive/FM synthesizer. Same
//    envelope, same piano roll, same mixer routing.
//
// Everything is stored as small integer arrays described by ParamDesc tables
// (see params.hpp), so the editors, the command set and the project file all
// handle envelopes and synth patches the same generic way.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "audio/params.hpp"

enum class InstrKind : uint8_t { Sampler = 0, Synth = 1 };

// AHDSR envelope parameters (indices into InstrumentData::env).
enum EnvParam { kEnvEnabled = 0, kEnvAttack, kEnvHold, kEnvDecay, kEnvSustain, kEnvRelease };

enum SynthParam {
    kSynOsc1Wave = 0,
    kSynOsc1Level,
    kSynOsc1Fine,     // cents
    kSynOsc2Wave,
    kSynOsc2Level,
    kSynOsc2Coarse,   // semitones
    kSynOsc2Detune,   // cents
    kSynSubLevel,
    kSynNoiseLevel,
    kSynMode,         // 0 mix, 1 FM (osc2 modulates osc1)
    kSynFmAmount,
    kSynFilterType,   // 0 off, 1 LP, 2 HP, 3 BP
    kSynCutoff,
    kSynResonance,
    kSynFilterEnv,    // -100..100 cutoff follows the amp envelope
    kSynLfoRate,
    kSynLfoDepth,
    kSynLfoTarget,    // 0 pitch, 1 filter, 2 amp
    kSynTranspose,    // semitones, whole instrument
    kSynLevel,
    kSynGlide,        // reserved (kept in the file format)
    kSynReserved1,
    kSynReserved2,
    kSynReserved3,
};

struct InstrumentData {
    uint8_t kind;                            // InstrKind
    int16_t env[cfg::kEnvParams];            // see EnvParam
    int16_t synth[cfg::kSynthParams];        // see SynthParam; used when kind == Synth
};

namespace instr {

int envParamCount();
const ParamDesc& envParam(int i);
int synthParamCount();
const ParamDesc& synthParam(int i);

// Defaults: sampler with the envelope off; init synth patch.
void setDefaults(InstrumentData& d);
void setSynthDefaults(InstrumentData& d);
// Clamps every field (after loading untrusted data).
void sanitize(InstrumentData& d);

struct Preset {
    const char* name;
    int16_t env[cfg::kEnvParams];
    int16_t synth[cfg::kSynthParams];
};
int synthPresetCount();
const Preset& synthPreset(int i);
void applySynthPreset(InstrumentData& d, int i);

int envPresetCount();
const char* envPresetName(int i);
void applyEnvPreset(InstrumentData& d, int i);

} // namespace instr

namespace synth {

// Oscillator waves.
enum Wave { Sine = 0, Square, Saw, Triangle, Noise, WaveCount };

// Builds the lookup tables. Must run once before osc()/filterG() are used (the Mixer constructor does).
void initTables();
extern int16_t g_sine[1025];

// 32-bit phase accumulator oscillator output, -32768..32767. Inline: it runs per sample per voice.
inline int32_t osc(int wave, uint32_t phase, uint32_t& noise)
{
    switch (wave) {
    case Sine: {
        const uint32_t idx = phase >> 22; // 10 bits
        const int32_t frac = (int32_t)((phase >> 6) & 0xffff);
        const int32_t a = g_sine[idx], b = g_sine[idx + 1];
        return a + (((b - a) * frac) >> 16);
    }
    case Square:
        return (phase & 0x80000000u) ? -26000 : 26000;
    case Saw:
        return (int32_t)(phase >> 16) - 32768;
    case Triangle: {
        int32_t t = (int32_t)(phase >> 15) - 65536; // rising -65536..65535
        if (t < 0)
            t = -t;
        return t - 32768 > 32767 ? 32767 : t - 32768;
    }
    default:
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        return (int32_t)(noise >> 16) - 32768;
    }
}
// Phase increment per output frame for a MIDI note + cents (A4 = 440 Hz).
uint32_t phaseInc(int note, int cents);
// State-variable filter coefficient g = tan(pi*fc/fs) for a cutoff index
// 0..1023 (fine steps of the 0..100 UI scale), table driven.
float filterG(int fineIdx);

} // namespace synth
