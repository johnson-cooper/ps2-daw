#include "audio/fx.hpp"

#include <math.h>
#include <string.h>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kFs = (float)cfg::kSampleRate;

const char* const kFilterTypes[] = {"LOW-PASS", "HIGH-PASS", "BAND-PASS"};
const char* const kDistTypes[] = {"SOFT", "HARD", "FOLD"};

// clang-format off
const ParamDesc kGainParams[] = {
    {"GAIN",    -60, 24,   0, 1,  Fmt::Db,      nullptr},
    {"PAN",     -100, 100, 0, 5,  Fmt::Pan,     nullptr},
    {"WIDTH",   0,   200, 100, 5, Fmt::Percent, nullptr},
    {"INVERT",  0,   1,    0, 1,  Fmt::OnOff,   nullptr},
};
const ParamDesc kFilterParams[] = {
    {"TYPE",    0, 2,   0,  1, Fmt::Enum,   kFilterTypes},
    {"CUTOFF",  0, 100, 70, 2, Fmt::Cutoff, nullptr},
    {"RESO",    0, 100, 20, 5, Fmt::Percent, nullptr},
};
const ParamDesc kDistParams[] = {
    {"DRIVE",   0, 100, 40, 5, Fmt::Percent, nullptr},
    {"TYPE",    0, 2,   0,  1, Fmt::Enum,    kDistTypes},
    {"TONE",    0, 100, 80, 5, Fmt::Percent, nullptr},
    {"MIX",     0, 100, 100, 5, Fmt::Percent, nullptr},
    {"LEVEL",   -24, 6, -3, 1, Fmt::Db,      nullptr},
};
const ParamDesc kDelayParams[] = {
    {"TIME",     10, 500, 250, 10, Fmt::Ms,      nullptr},
    {"FEEDBACK", 0,  95,  40,  5,  Fmt::Percent, nullptr},
    {"TONE",     0,  100, 70,  5,  Fmt::Percent, nullptr},
    {"MIX",      0,  100, 30,  5,  Fmt::Percent, nullptr},
    {"PINGPONG", 0,  1,   0,   1,  Fmt::OnOff,   nullptr},
};
const ParamDesc kCompParams[] = {
    {"THRESH",  -60, 0,   -18, 1,  Fmt::Db,   nullptr},
    {"RATIO",   1,   20,  4,   1,  Fmt::Ratio, nullptr},
    {"ATTACK",  1,   100, 10,  1,  Fmt::Ms,   nullptr},
    {"RELEASE", 10,  1000, 150, 10, Fmt::Ms,  nullptr},
    {"MAKEUP",  0,   24,  3,   1,  Fmt::Db,   nullptr},
};
const ParamDesc kEqParams[] = {
    {"LOW",     -12, 12, 0,  1, Fmt::Db,     nullptr},
    {"MID",     -12, 12, 0,  1, Fmt::Db,     nullptr},
    {"MID FREQ", 0, 100, 60, 2, Fmt::Cutoff, nullptr},
    {"MID Q x10", 2, 40, 10, 1, Fmt::Int,    nullptr},
    {"HIGH",    -12, 12, 0,  1, Fmt::Db,     nullptr},
};
const ParamDesc kReverbParams[] = {
    {"SIZE",  0, 100, 50,  5, Fmt::Percent, nullptr},
    {"DAMP",  0, 100, 50,  5, Fmt::Percent, nullptr},
    {"WIDTH", 0, 100, 100, 5, Fmt::Percent, nullptr},
    {"MIX",   0, 100, 25,  5, Fmt::Percent, nullptr},
};
const ParamDesc kNoParam = {"", 0, 0, 0, 1, Fmt::Int, nullptr};
// clang-format on

struct Table {
    const ParamDesc* d;
    int n;
};

Table tableFor(FxType t)
{
    switch (t) {
    case FxType::Gain: return {kGainParams, 4};
    case FxType::Filter: return {kFilterParams, 3};
    case FxType::Distortion: return {kDistParams, 5};
    case FxType::Delay: return {kDelayParams, 5};
    case FxType::Compressor: return {kCompParams, 5};
    case FxType::Eq: return {kEqParams, 5};
    case FxType::Reverb: return {kReverbParams, 4};
    default: return {&kNoParam, 0};
    }
}

inline float dbToLin(float db) { return powf(10.0f, db * 0.05f); }

// Freeverb tunings rescaled from 44.1 kHz to 48 kHz.
constexpr int kCombLen[4] = {1214, 1293, 1390, 1476};
constexpr int kApLen[2] = {605, 480};
constexpr int kStereoSpread = 23;

constexpr int kCombTotal = kCombLen[0] + kCombLen[1] + kCombLen[2] + kCombLen[3];
constexpr int combOffset(int ch, int i)
{
    int o = ch * kCombTotal; // the left bank is first; the right bank follows it
    for (int k = 0; k < i; ++k)
        o += kCombLen[k] + (ch ? kStereoSpread : 0);
    return o;
}
constexpr int apOffset(int ch, int i)
{
    // laid out after both comb banks
    int o = 2 * kCombTotal + 4 * kStereoSpread;
    o += ch * (kApLen[0] + kApLen[1] + 2 * kStereoSpread);
    if (i)
        o += kApLen[0] + (ch ? kStereoSpread : 0);
    return o;
}
static_assert(apOffset(1, 1) + kApLen[1] + kStereoSpread <= FxMemory::kReverbFloats, "reverb pool too small");

constexpr int kCombOff[2][4] = {{combOffset(0, 0), combOffset(0, 1), combOffset(0, 2), combOffset(0, 3)},
                                {combOffset(1, 0), combOffset(1, 1), combOffset(1, 2), combOffset(1, 3)}};
constexpr int kCombLenCh[2][4] = {{kCombLen[0], kCombLen[1], kCombLen[2], kCombLen[3]},
                                  {kCombLen[0] + kStereoSpread, kCombLen[1] + kStereoSpread, kCombLen[2] + kStereoSpread, kCombLen[3] + kStereoSpread}};
constexpr int kApOff[2][2] = {{apOffset(0, 0), apOffset(0, 1)}, {apOffset(1, 0), apOffset(1, 1)}};
constexpr int kApLenCh[2][2] = {{kApLen[0], kApLen[1]}, {kApLen[0] + kStereoSpread, kApLen[1] + kStereoSpread}};

// log2 / 2^x approximations (|error| < 0.001 octave): libm's powf/log10f are software
// routines on the EE and cost thousands of cycles each; the compressor needs two per chunk.
inline float fastLog2(float x)
{
    union { float f; uint32_t u; } v = {x};
    const float e = (float)((int)((v.u >> 23) & 255) - 127);
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    const float m = v.f;
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

inline float fastExp2(float x)
{
    if (x < -126.0f)
        return 0.0f;
    const float fl = (float)(int)(x < 0 ? x - 1.0f : x); // floor for the range used here
    const float f = x - fl;
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)((int)fl + 127) << 23;
    return v.f * (1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f))));
}

inline float flush(float v) { return (v > -1e-15f && v < 1e-15f) ? 0.0f : v; }

} // namespace

namespace fx {

const char* typeName(FxType t)
{
    switch (t) {
    case FxType::Gain: return "GAIN";
    case FxType::Filter: return "FILTER";
    case FxType::Distortion: return "DISTORT";
    case FxType::Delay: return "DELAY";
    case FxType::Compressor: return "COMP";
    case FxType::Eq: return "EQ";
    case FxType::Reverb: return "REVERB";
    default: return "EMPTY";
    }
}

int paramCount(FxType t) { return tableFor(t).n; }

const ParamDesc& param(FxType t, int i)
{
    const Table tb = tableFor(t);
    return (i >= 0 && i < tb.n) ? tb.d[i] : kNoParam;
}

void setDefaults(FxData& d, FxType t)
{
    memset(&d, 0, sizeof(d));
    d.type = (uint8_t)t;
    const Table tb = tableFor(t);
    for (int i = 0; i < tb.n; ++i)
        d.p[i] = tb.d[i].def;
}

void sanitize(FxData& d)
{
    if (d.type >= kFxTypeCount)
        d.type = 0;
    d.bypass = d.bypass ? 1 : 0;
    const Table tb = tableFor((FxType)d.type);
    for (int i = 0; i < cfg::kFxParams; ++i)
        d.p[i] = i < tb.n ? (int16_t)params::clampTo(tb.d[i], d.p[i]) : 0;
}

} // namespace fx

FxMemory::FxMemory()
{
    memset(delay, 0, sizeof(delay));
    memset(reverb, 0, sizeof(reverb));
    memset(delayUsed, 0, sizeof(delayUsed));
    memset(reverbUsed, 0, sizeof(reverbUsed));
}

FxUnit::FxUnit() : type_(FxType::None), starved_(false), fresh_(true), bypassMix_(1.0f), bypassTarget_(1.0f), mem_(nullptr), memIdx_(-1)
{
    memset(p_, 0, sizeof(p_));
    memset(bq_, 0, sizeof(bq_));
    gainCur_ = gainTarget_ = 1.0f;
    panL_ = panR_ = width_ = 1.0f;
    invert_ = 1.0f;
    svfA1_ = svfA2_ = svfA3_ = 0;
    svfK_ = 1;
    memset(ic1_, 0, sizeof(ic1_));
    memset(ic2_, 0, sizeof(ic2_));
    filterType_ = 0;
    drive_ = 1;
    outGain_ = 1;
    toneA_ = 1;
    memset(toneZ_, 0, sizeof(toneZ_));
    dmix_ = 1;
    distType_ = 0;
    delayPos_ = 0;
    delayCur_ = delayTarget_ = 12000;
    fb_ = 0;
    delayTone_ = 1;
    memset(delayLp_, 0, sizeof(delayLp_));
    dlyMix_ = 0.3f;
    pingPong_ = false;
    thrDb_ = -18;
    ratio_ = 4;
    atkCoef_ = relCoef_ = 0.01f;
    makeup_ = 1;
    env_ = 0;
    gCur_ = 1;
    grDeci_ = 0;
    rvFb_ = rvDamp_ = rvWet1_ = rvWet2_ = rvMix_ = 0;
    memset(combPos_, 0, sizeof(combPos_));
    memset(apPos_, 0, sizeof(apPos_));
    memset(combLp_, 0, sizeof(combLp_));
}

void FxUnit::release()
{
    if (mem_ && memIdx_ >= 0) {
        if (type_ == FxType::Delay)
            mem_->delayUsed[memIdx_] = false;
        else if (type_ == FxType::Reverb)
            mem_->reverbUsed[memIdx_] = false;
    }
    memIdx_ = -1;
}

void FxUnit::setType(FxType t, FxMemory& mem)
{
    release();
    mem_ = &mem;
    type_ = (int)t < kFxTypeCount ? t : FxType::None;
    starved_ = false;
    fresh_ = true;
    FxData d;
    fx::setDefaults(d, type_);
    memcpy(p_, d.p, sizeof(p_));

    // Reset state.
    memset(ic1_, 0, sizeof(ic1_));
    memset(ic2_, 0, sizeof(ic2_));
    memset(toneZ_, 0, sizeof(toneZ_));
    memset(delayLp_, 0, sizeof(delayLp_));
    memset(combPos_, 0, sizeof(combPos_));
    memset(apPos_, 0, sizeof(apPos_));
    memset(combLp_, 0, sizeof(combLp_));
    memset(bq_, 0, sizeof(bq_));
    delayPos_ = 0;
    env_ = 0;
    gCur_ = 1;
    grDeci_ = 0;

    if (type_ == FxType::Delay) {
        for (int i = 0; i < FxMemory::kDelayUnits; ++i)
            if (!mem.delayUsed[i]) {
                mem.delayUsed[i] = true;
                memIdx_ = i;
                memset(mem.delay[i], 0, sizeof(mem.delay[i]));
                break;
            }
        starved_ = memIdx_ < 0;
    } else if (type_ == FxType::Reverb) {
        for (int i = 0; i < FxMemory::kReverbUnits; ++i)
            if (!mem.reverbUsed[i]) {
                mem.reverbUsed[i] = true;
                memIdx_ = i;
                memset(mem.reverb[i], 0, sizeof(mem.reverb[i]));
                break;
            }
        starved_ = memIdx_ < 0;
    }
    update();
    gainCur_ = gainTarget_;
    delayCur_ = delayTarget_;
    gCur_ = 1;
}

void FxUnit::setParam(int index, int value)
{
    if (index < 0 || index >= fx::paramCount(type_))
        return;
    p_[index] = (int16_t)params::clampTo(fx::param(type_, index), value);
    update();
}

void FxUnit::update()
{
    switch (type_) {
    case FxType::Gain: {
        gainTarget_ = p_[0] <= -60 ? 0.0f : dbToLin((float)p_[0]);
        panL_ = p_[1] > 0 ? (100 - p_[1]) / 100.0f : 1.0f;
        panR_ = p_[1] < 0 ? (100 + p_[1]) / 100.0f : 1.0f;
        width_ = p_[2] / 100.0f;
        invert_ = p_[3] ? -1.0f : 1.0f;
        break;
    }
    case FxType::Filter: {
        filterType_ = p_[0];
        float fc = (float)params::cutoffHz(p_[1]);
        if (fc > 0.45f * kFs)
            fc = 0.45f * kFs;
        const float g = tanf(kPi * fc / kFs);
        const float q = 0.55f * powf(20.0f, p_[2] / 100.0f); // 0.55 .. 11
        svfK_ = 1.0f / q;
        svfA1_ = 1.0f / (1.0f + g * (g + svfK_));
        svfA2_ = g * svfA1_;
        svfA3_ = g * svfA2_;
        break;
    }
    case FxType::Distortion: {
        drive_ = 1.0f + p_[0] * 0.3f; // up to ~31x
        distType_ = p_[1];
        // Tone: one-pole low-pass after the shaper, 1.2 kHz .. open
        const float fc = 1200.0f * powf(12.0f, p_[2] / 100.0f);
        toneA_ = p_[2] >= 100 ? 1.0f : 1.0f - expf(-2.0f * kPi * fc / kFs);
        dmix_ = p_[3] / 100.0f;
        outGain_ = dbToLin((float)p_[4]);
        break;
    }
    case FxType::Delay: {
        delayTarget_ = p_[0] * 0.001f * kFs;
        if (delayTarget_ > FxMemory::kDelayFrames - 2)
            delayTarget_ = (float)(FxMemory::kDelayFrames - 2);
        if (fresh_)
            delayCur_ = delayTarget_; // a project load must not make the first echoes glide in
        fb_ = p_[1] / 100.0f;
        // Tone darkens the repeats: one-pole low-pass in the feedback path.
        const float fc = 800.0f * powf(20.0f, p_[2] / 100.0f);
        delayTone_ = p_[2] >= 100 ? 1.0f : 1.0f - expf(-2.0f * kPi * fc / kFs);
        dlyMix_ = p_[3] / 100.0f;
        pingPong_ = p_[4] != 0;
        break;
    }
    case FxType::Compressor: {
        thrDb_ = (float)p_[0];
        ratio_ = (float)p_[1];
        atkCoef_ = 1.0f - expf(-1.0f / (p_[2] * 0.001f * kFs));
        relCoef_ = 1.0f - expf(-1.0f / (p_[3] * 0.001f * kFs));
        makeup_ = dbToLin((float)p_[4]);
        break;
    }
    case FxType::Eq: {
        auto setup = [&](Biquad& b, int kind, float f, float dB, float q) {
            b.on = dB != 0.0f;
            if (f > 0.45f * kFs)
                f = 0.45f * kFs;
            const float A = powf(10.0f, dB / 40.0f);
            const float w0 = 2.0f * kPi * f / kFs;
            const float c = cosf(w0), s = sinf(w0);
            float b0, b1, b2, a0, a1, a2;
            if (kind == 0) { // peaking
                const float alpha = s / (2.0f * q);
                b0 = 1 + alpha * A;
                b1 = -2 * c;
                b2 = 1 - alpha * A;
                a0 = 1 + alpha / A;
                a1 = -2 * c;
                a2 = 1 - alpha / A;
            } else {
                const float alpha = s * 0.5f * 1.41421356f;
                const float sa = 2.0f * sqrtf(A) * alpha;
                if (kind == 1) { // low shelf
                    b0 = A * ((A + 1) - (A - 1) * c + sa);
                    b1 = 2 * A * ((A - 1) - (A + 1) * c);
                    b2 = A * ((A + 1) - (A - 1) * c - sa);
                    a0 = (A + 1) + (A - 1) * c + sa;
                    a1 = -2 * ((A - 1) + (A + 1) * c);
                    a2 = (A + 1) + (A - 1) * c - sa;
                } else { // high shelf
                    b0 = A * ((A + 1) + (A - 1) * c + sa);
                    b1 = -2 * A * ((A - 1) + (A + 1) * c);
                    b2 = A * ((A + 1) + (A - 1) * c - sa);
                    a0 = (A + 1) - (A - 1) * c + sa;
                    a1 = 2 * ((A - 1) - (A + 1) * c);
                    a2 = (A + 1) - (A - 1) * c - sa;
                }
            }
            b.b0 = b0 / a0;
            b.b1 = b1 / a0;
            b.b2 = b2 / a0;
            b.a1 = a1 / a0;
            b.a2 = a2 / a0;
        };
        setup(bq_[0], 1, 250.0f, (float)p_[0], 0.7f);
        setup(bq_[1], 0, (float)params::cutoffHz(p_[2]), (float)p_[1], p_[3] / 10.0f);
        setup(bq_[2], 2, 4000.0f, (float)p_[4], 0.7f);
        break;
    }
    case FxType::Reverb: {
        rvFb_ = 0.70f + 0.28f * (p_[0] / 100.0f);
        rvDamp_ = 0.05f + 0.85f * (p_[1] / 100.0f);
        const float w = p_[2] / 100.0f;
        rvMix_ = p_[3] / 100.0f;
        rvWet1_ = w * 0.5f + 0.5f;
        rvWet2_ = (1.0f - w) * 0.5f;
        break;
    }
    default:
        break;
    }
}

void FxUnit::process(float* l, float* r, int n, float* tl, float* tr)
{
    if (type_ == FxType::None || (starved_ && (type_ == FxType::Delay || type_ == FxType::Reverb)))
        return;
    if (bypassMix_ == bypassTarget_ && bypassTarget_ == 0.0f)
        return; // fully bypassed: skip the work entirely

    if (bypassMix_ == 1.0f && bypassTarget_ == 1.0f) {
        run(l, r, n);
        return;
    }
    // Bypass toggled: crossfade dry and processed over this block (declick).
    memcpy(tl, l, sizeof(float) * (size_t)n);
    memcpy(tr, r, sizeof(float) * (size_t)n);
    run(l, r, n);
    const float from = bypassMix_, to = bypassTarget_;
    for (int i = 0; i < n; ++i) {
        const float m = from + (to - from) * ((float)(i + 1) / (float)n);
        l[i] = tl[i] + (l[i] - tl[i]) * m;
        r[i] = tr[i] + (r[i] - tr[i]) * m;
    }
    bypassMix_ = to;
}

void FxUnit::run(float* l, float* r, int n)
{
    switch (type_) {
    case FxType::Gain: runGain(l, r, n); break;
    case FxType::Filter: runFilter(l, r, n); break;
    case FxType::Distortion: runDistortion(l, r, n); break;
    case FxType::Delay: runDelay(l, r, n); break;
    case FxType::Compressor: runCompressor(l, r, n); break;
    case FxType::Eq: runEq(l, r, n); break;
    case FxType::Reverb: runReverb(l, r, n); break;
    default: break;
    }
}

void FxUnit::runGain(float* l, float* r, int n)
{
    const float g0 = gainCur_;
    const float step = (gainTarget_ - g0) / (float)n;
    float g = g0;
    for (int i = 0; i < n; ++i) {
        g += step;
        float a = l[i], b = r[i];
        if (width_ != 1.0f) {
            const float m = (a + b) * 0.5f, s = (a - b) * 0.5f * width_;
            a = m + s;
            b = m - s;
        }
        l[i] = a * g * panL_ * invert_;
        r[i] = b * g * panR_ * invert_;
    }
    gainCur_ = gainTarget_;
}

void FxUnit::runFilter(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
        float ic1 = ic1_[c], ic2 = ic2_[c];
        float* x = ch[c];
        for (int i = 0; i < n; ++i) {
            const float in = x[i] * (1.0f / 32768.0f);
            const float v3 = in - ic2;
            const float v1 = svfA1_ * ic1 + svfA2_ * v3;
            const float v2 = ic2 + svfA2_ * ic1 + svfA3_ * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            float out;
            if (filterType_ == 0)
                out = v2;
            else if (filterType_ == 1)
                out = in - svfK_ * v1 - v2;
            else
                out = v1;
            x[i] = out * 32768.0f;
        }
        ic1_[c] = flush(ic1);
        ic2_[c] = flush(ic2);
    }
}

void FxUnit::runDistortion(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
        float z = toneZ_[c];
        float* x = ch[c];
        for (int i = 0; i < n; ++i) {
            const float in = x[i] * (1.0f / 32768.0f);
            float v = in * drive_;
            switch (distType_) {
            case 0: { // soft: cubic saturation, smooth into full scale (no division)
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                v = v * (1.5f - 0.5f * v * v);
                break;
            }
            case 1: // hard clip
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                break;
            default: { // fold back at +-1
                if (v > 8.0f) v = 8.0f;
                if (v < -8.0f) v = -8.0f;
                while (v > 1.0f || v < -1.0f)
                    v = v > 1.0f ? 2.0f - v : -2.0f - v;
                break;
            }
            }
            z += toneA_ * (v - z);
            const float wet = z * outGain_;
            x[i] = (in + (wet - in) * dmix_) * 32768.0f;
        }
        toneZ_[c] = flush(z);
    }
}

void FxUnit::runDelay(float* l, float* r, int n)
{
    float* bufL = mem_->delay[memIdx_][0];
    float* bufR = mem_->delay[memIdx_][1];
    constexpr int kLen = FxMemory::kDelayFrames;
    int pos = delayPos_;
    float cur = delayCur_;
    float lpL = delayLp_[0], lpR = delayLp_[1];
    if (fabsf(delayTarget_ - cur) < 0.5f) {
        // Settled (the usual case): the delay is a whole number of frames, so no
        // interpolation and no slew are needed.
        cur = delayTarget_;
        int rp = pos - (int)cur;
        if (rp < 0)
            rp += kLen;
        for (int i = 0; i < n; ++i) {
            const float dl = bufL[rp], dr = bufR[rp];
            lpL += delayTone_ * (dl - lpL);
            lpR += delayTone_ * (dr - lpR);
            const float inL = l[i], inR = r[i];
            if (pingPong_) {
                bufL[pos] = (inL + inR) * 0.5f + fb_ * lpR;
                bufR[pos] = fb_ * lpL;
            } else {
                bufL[pos] = inL + fb_ * lpL;
                bufR[pos] = inR + fb_ * lpR;
            }
            l[i] = inL + dl * dlyMix_;
            r[i] = inR + dr * dlyMix_;
            if (++rp >= kLen)
                rp = 0;
            if (++pos >= kLen)
                pos = 0;
        }
        fresh_ = false;
        delayPos_ = pos;
        delayCur_ = cur;
        delayLp_[0] = flush(lpL);
        delayLp_[1] = flush(lpR);
        return;
    }
    for (int i = 0; i < n; ++i) {
        cur += (delayTarget_ - cur) * 0.0005f; // slewed time: no zipper clicks, tape-like glide
        float rp = (float)pos - cur;
        while (rp < 0)
            rp += (float)kLen;
        int i0 = (int)rp;
        const float fr = rp - (float)i0;
        int i1 = i0 + 1;
        if (i0 >= kLen)
            i0 -= kLen;
        if (i1 >= kLen)
            i1 -= kLen;
        const float dl = bufL[i0] + (bufL[i1] - bufL[i0]) * fr;
        const float dr = bufR[i0] + (bufR[i1] - bufR[i0]) * fr;
        lpL += delayTone_ * (dl - lpL);
        lpR += delayTone_ * (dr - lpR);
        const float inL = l[i], inR = r[i];
        if (pingPong_) {
            bufL[pos] = (inL + inR) * 0.5f + fb_ * lpR;
            bufR[pos] = fb_ * lpL;
        } else {
            bufL[pos] = inL + fb_ * lpL;
            bufR[pos] = inR + fb_ * lpR;
        }
        l[i] = inL + dl * dlyMix_;
        r[i] = inR + dr * dlyMix_;
        if (++pos >= kLen)
            pos = 0;
    }
    fresh_ = false;
    delayPos_ = pos;
    delayCur_ = cur;
    delayLp_[0] = flush(lpL);
    delayLp_[1] = flush(lpR);
}

void FxUnit::runCompressor(float* l, float* r, int n)
{
    const float thrLin = dbToLin(thrDb_) * 32768.0f;
    const float slope = 1.0f - 1.0f / ratio_;
    float env = env_;
    float g = gCur_;
    int worst = 0;
    constexpr int kChunk = 32;
    for (int base = 0; base < n; base += kChunk) {
        const int m = (n - base) < kChunk ? (n - base) : kChunk;
        // Peak detector with separate attack/release, then one gain
        // computation per chunk (log/exp are too costly per sample on the EE).
        for (int i = 0; i < m; ++i) {
            const float a = fabsf(l[base + i]), b = fabsf(r[base + i]);
            const float pk = a > b ? a : b;
            env += (pk > env ? atkCoef_ : relCoef_) * (pk - env);
        }
        float target = 1.0f;
        if (env > thrLin && thrLin > 0.0f) {
            const float overDb = 6.0205999f * fastLog2(env / thrLin);
            const float grDb = overDb * slope;
            target = fastExp2(-grDb * 0.16609640f);
            const int deci = (int)(grDb * 10.0f);
            if (deci > worst)
                worst = deci;
        }
        const float step = (target - g) / (float)m;
        for (int i = 0; i < m; ++i) {
            g += step;
            l[base + i] *= g * makeup_;
            r[base + i] *= g * makeup_;
        }
        g = target;
    }
    env_ = flush(env);
    gCur_ = g;
    grDeci_ = worst;
}

void FxUnit::runEq(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int k = 0; k < 3; ++k) {
        Biquad& b = bq_[k];
        if (!b.on)
            continue;
        for (int c = 0; c < 2; ++c) {
            float x1 = b.x1[c], x2 = b.x2[c], y1 = b.y1[c], y2 = b.y2[c];
            float* x = ch[c];
            for (int i = 0; i < n; ++i) {
                const float in = x[i];
                const float out = b.b0 * in + b.b1 * x1 + b.b2 * x2 - b.a1 * y1 - b.a2 * y2;
                x2 = x1;
                x1 = in;
                y2 = y1;
                y1 = out;
                x[i] = out;
            }
            b.x1[c] = x1;
            b.x2[c] = x2;
            b.y1[c] = flush(y1);
            b.y2[c] = flush(y2);
        }
    }
}

void FxUnit::runReverb(float* l, float* r, int n)
{
    // Offsets and lengths are compile-time constants; everything the inner loop touches is
    // hoisted into locals so the per-sample work is just the filter arithmetic.
    float* base = mem_->reverb[memIdx_];
    float* cb[2][4];
    float* ab[2][2];
    int cp[2][4], ap[2][2];
    float lp[2][4];
    for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < 4; ++k) {
            cb[c][k] = base + kCombOff[c][k];
            cp[c][k] = combPos_[c][k];
            lp[c][k] = combLp_[c][k];
        }
        for (int k = 0; k < 2; ++k) {
            ab[c][k] = base + kApOff[c][k];
            ap[c][k] = apPos_[c][k];
        }
    }
    const float damp = rvDamp_, ndamp = 1.0f - rvDamp_, fb = rvFb_;
    const float w1 = rvWet1_ * 3.0f, w2 = rvWet2_ * 3.0f, mix = rvMix_, dryGain = 1.0f - rvMix_ * 0.5f;

#define RV_COMB(c, k, acc)                                      {                                                               float* b = cb[c][k];                                        int p = cp[c][k];                                           const float o = b[p];                                       const float f = o * ndamp + lp[c][k] * damp;                lp[c][k] = f;                                               b[p] = in + f * fb;                                         cp[c][k] = ++p >= kCombLenCh[c][k] ? 0 : p;                 acc += o;                                               }
#define RV_AP(c, k, acc)                                        {                                                               float* b = ab[c][k];                                        int p = ap[c][k];                                           const float bo = b[p];                                      b[p] = acc + bo * 0.5f;                                     acc = bo - acc;                                             ap[c][k] = ++p >= kApLenCh[c][k] ? 0 : p;               }

    for (int i = 0; i < n; ++i) {
        const float inL = l[i], inR = r[i];
        const float in = (inL + inR) * 0.015f;
        float a0 = 0.0f, a1 = 0.0f;
        RV_COMB(0, 0, a0) RV_COMB(0, 1, a0) RV_COMB(0, 2, a0) RV_COMB(0, 3, a0)
        RV_COMB(1, 0, a1) RV_COMB(1, 1, a1) RV_COMB(1, 2, a1) RV_COMB(1, 3, a1)
        RV_AP(0, 0, a0) RV_AP(0, 1, a0)
        RV_AP(1, 0, a1) RV_AP(1, 1, a1)
        const float wl = a0 * w1 + a1 * w2, wr = a1 * w1 + a0 * w2;
        l[i] = inL * dryGain + wl * mix;
        r[i] = inR * dryGain + wr * mix;
    }
#undef RV_COMB
#undef RV_AP

    for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < 4; ++k) {
            combPos_[c][k] = cp[c][k];
            combLp_[c][k] = flush(lp[c][k]);
        }
        for (int k = 0; k < 2; ++k)
            apPos_[c][k] = ap[c][k];
    }
}
