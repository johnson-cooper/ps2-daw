// Parameter descriptors shared by the engine, the project file and the UI.
//
// Effects, envelopes and synthesizer patches are all "arrays of small
// integers plus a descriptor table". That keeps the realtime command set tiny
// (type, index, value), lets the file format store every parameter the same
// way, and gives the controller-driven editors a single generic widget: step
// a value with the D-pad, show it with format().
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class Fmt : uint8_t {
    Int,      // plain number
    Percent,  // 0..100 %
    Db,       // decibels
    Ms,       // milliseconds
    Cutoff,   // 0..100 index -> 20 Hz .. 18 kHz (exponential)
    Cents,    // fine tuning
    Semis,    // coarse tuning
    Enum,     // label from `names`
    OnOff,
    Ratio,    // n:1
    LfoRate,  // 0..100 index -> 0.1 .. 20 Hz
    Pan,      // L50 / C / R50
};

struct ParamDesc {
    const char* name;
    int16_t min, max, def;
    int16_t step;            // D-pad increment
    Fmt fmt;
    const char* const* names; // for Fmt::Enum, max-min+1 labels
};

namespace params {

// Writes a short human-readable value ("-6 dB", "SAW", "440 Hz").
void format(const ParamDesc& d, int value, char* out, size_t cap);

// Cutoff index (0..100) -> Hz, and LFO rate index (0..100) -> centi-Hz.
int cutoffHz(int idx);
int lfoRateCentiHz(int idx);

inline int clampTo(const ParamDesc& d, int v) { return v < d.min ? d.min : (v > d.max ? d.max : v); }

} // namespace params
