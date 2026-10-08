// Mixer effects: small, allocation-free stereo processors.
//
// Each mixer track owns a chain of cfg::kFxSlots units. Units work on float
// buffers (the R5900 FPU is single precision, which is plenty here). Delay and
// reverb memory comes from a fixed pool (FxMemory) claimed on the audio thread
// when the effect type changes; nothing is ever allocated while rendering.
// When the pool is exhausted the unit passes audio through and reports
// `starved()` so the UI can say so.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "audio/params.hpp"

enum class FxType : uint8_t { None = 0, Gain, Filter, Distortion, Delay, Compressor, Eq, Reverb, Count };
constexpr int kFxTypeCount = (int)FxType::Count;

// Persistent (project) form of one effect slot.
struct FxData {
    uint8_t type;   // FxType
    uint8_t bypass;
    int16_t p[cfg::kFxParams];
};

namespace fx {

const char* typeName(FxType t);
int paramCount(FxType t);
const ParamDesc& param(FxType t, int i);
// Resets `d` to `t` with its default parameters (bypass off).
void setDefaults(FxData& d, FxType t);
// Clamps every parameter into range (after loading untrusted data).
void sanitize(FxData& d);

} // namespace fx

// Fixed delay/reverb storage shared by all units of a mixer (~700 KiB).
struct FxMemory {
    static constexpr int kDelayUnits = 3;
    static constexpr int kDelayFrames = 24000; // 500 ms at 48 kHz
    static constexpr int kReverbUnits = 2;
    static constexpr int kReverbFloats = 13184;

    float delay[kDelayUnits][2][kDelayFrames];
    float reverb[kReverbUnits][kReverbFloats];
    bool delayUsed[kDelayUnits];
    bool reverbUsed[kReverbUnits];

    FxMemory();
};

class FxUnit {
public:
    FxUnit();

    // Audio thread. Changes the effect: releases the old memory, claims new
    // memory, clears all state and loads that effect's default parameters.
    void setType(FxType t, FxMemory& mem);
    FxType type() const { return type_; }
    void setParam(int index, int value);
    int param(int index) const { return p_[index]; }
    void setBypass(bool b) { bypassTarget_ = b ? 0.0f : 1.0f; }
    bool bypassed() const { return bypassTarget_ == 0.0f; }
    // True when the pool had no free memory for this effect (it passes audio through).
    bool starved() const { return starved_; }
    // True while the unit may still produce sound after its input went silent.
    bool hasTail() const { return type_ == FxType::Delay || type_ == FxType::Reverb; }
    bool active() const { return type_ != FxType::None; }
    // Compressor: last gain reduction in tenths of a dB (0 = none).
    int gainReductionDeci() const { return grDeci_; }

    // Processes `n` frames in place. `tl`/`tr` are scratch buffers of at least n.
    void process(float* l, float* r, int n, float* tl, float* tr);

private:
    void update();
    void run(float* l, float* r, int n);
    void runGain(float* l, float* r, int n);
    void runFilter(float* l, float* r, int n);
    void runDistortion(float* l, float* r, int n);
    void runDelay(float* l, float* r, int n);
    void runCompressor(float* l, float* r, int n);
    void runEq(float* l, float* r, int n);
    void runReverb(float* l, float* r, int n);
    void release();

    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1[2], x2[2], y1[2], y2[2];
        bool on;
    };

    FxType type_;
    bool starved_;
    bool fresh_;                      // no audio processed yet: parameter jumps need no glide
    int16_t p_[cfg::kFxParams];
    float bypassMix_, bypassTarget_; // 1 = effect fully in, 0 = bypassed
    FxMemory* mem_;
    int memIdx_;

    // Gain
    float gainCur_, gainTarget_, panL_, panR_, width_, invert_;
    // Filter (TPT state-variable) and the distortion tone filter
    float svfA1_, svfA2_, svfA3_, svfK_;
    float ic1_[2], ic2_[2];
    int filterType_;
    // Distortion
    float drive_, outGain_, toneA_, toneZ_[2], dmix_;
    int distType_;
    // Delay
    int delayPos_;
    float delayCur_, delayTarget_, fb_, delayTone_, delayLp_[2], dlyMix_;
    bool pingPong_;
    // Compressor
    float thrDb_, ratio_, atkCoef_, relCoef_, makeup_, env_, gCur_;
    int grDeci_;
    // EQ
    Biquad bq_[3];
    // Reverb
    float rvFb_, rvDamp_, rvWet1_, rvWet2_, rvMix_;
    int combPos_[2][4], apPos_[2][2];
    float combLp_[2][4];
};
