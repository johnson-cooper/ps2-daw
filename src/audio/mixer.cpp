#include "audio/mixer.hpp"

#include <math.h>
#include <string.h>

#include "audio/pitch.hpp"

namespace {

constexpr int32_t kUnity = 32767;
constexpr int kReleaseFrames = 256; // ~5 ms declick
constexpr int32_t kEnvOne = 1 << 24; // envelope level 1.0
constexpr uint32_t kShortTail = 4096;
constexpr uint32_t kLongTail = 10u * (uint32_t)cfg::kSampleRate; // delay / reverb ring-out

inline int32_t absi(int32_t v) { return v < 0 ? -v : v; }

// Per-`n`-frames multiplier of an exponential that reaches -60 dB after `ms` milliseconds.
inline int32_t expCoef(int ms, int n) { return (int32_t)(expf(-6.9078f * (float)n / ((float)ms * 48.0f)) * 65536.0f); }

inline int16_t saturate16(int32_t v, uint32_t& clips)
{
    if (v > 32767) {
        ++clips;
        return 32767;
    }
    if (v < -32768) {
        ++clips;
        return -32768;
    }
    return (int16_t)v;
}

inline uint16_t decayMeter(uint16_t m, int32_t p)
{
    // Instant attack, ~-0.56 dB per block release.
    int32_t d = m - (m >> 4);
    if (p > d)
        d = p;
    return (uint16_t)(d > 32767 ? 32767 : d);
}

// Float bus -> integer bus. Plain truncation (|error| < 1 LSB, no bias): the EE's cvt.w.s
// already saturates out-of-range values, so no clamps or rounding branches are needed.
inline int32_t roundToInt(float x) { return (int32_t)x; }

} // namespace

Mixer::Mixer()
    : fxMem_(new FxMemory), meterL_(0), meterR_(0), masterPercent_(0), masterGain_(0), anyTrackSolo_(false), serial_(0), steals_(0)
{
    memset(voices_, 0, sizeof(voices_));
    memset(acc_, 0, sizeof(acc_));
    memset(fxBuf_, 0, sizeof(fxBuf_));
    memset(fxTmp_, 0, sizeof(fxTmp_));
    memset(blockPeakCh_, 0, sizeof(blockPeakCh_));
    memset(meterCh_, 0, sizeof(meterCh_));
    memset(trackMeterL_, 0, sizeof(trackMeterL_));
    memset(trackMeterR_, 0, sizeof(trackMeterR_));
    memset(trackClip_, 0, sizeof(trackClip_));
    memset(trackGr_, 0, sizeof(trackGr_));
    memset(busTail_, 0, sizeof(busTail_));
    memset(busDirty_, 0, sizeof(busDirty_));
    for (int t = 0; t < cfg::kMixBuses; ++t)
        trackGainL_[t] = trackGainR_[t] = volumeToQ15(100);
    for (auto& d : inst_)
        instr::setDefaults(d);
    synth::initTables(); // the inline oscillators need their tables before the first block
    // Metronome clicks: 50 ms decaying sine bursts, a higher one for the bar start.
    for (int k = 0; k < 2; ++k) {
        const float hz = k ? 1900.0f : 1250.0f, amp = k ? 15000.0f : 10000.0f;
        for (int i = 0; i < 2400; ++i)
            clickPcm_[k][i] = (int16_t)(amp * expf(-(float)i / 450.0f) * sinf(6.2831853f * hz * (float)i / (float)cfg::kSampleRate));
        memset(&click_[k], 0, sizeof(Sample));
        click_[k].data = clickPcm_[k];
        click_[k].frames = 2400;
        click_[k].sampleRate = (uint32_t)cfg::kSampleRate;
        click_[k].channels = 1;
        click_[k].builtin = 1;
        click_[k].state = (uint8_t)SlotState::Ready;
    }
    setMasterVolume(80);
}

Mixer::~Mixer() { delete fxMem_; }

int32_t Mixer::volumeToQ15(int percent)
{
    if (percent <= 0)
        return 0;
    if (percent >= 100)
        return kUnity;
    // Squared law: 78% ~ -4.3 dB, 50% ~ -12 dB. Feels closer to a fader than linear.
    return (int32_t)(percent * percent) * kUnity / 10000;
}

void Mixer::panToQ15(int pan, int32_t& left, int32_t& right)
{
    if (pan < -100)
        pan = -100;
    if (pan > 100)
        pan = 100;
    // Balance law (centre = unity on both sides), matching the audio
    // library's SPU2 pan semantics so the two voice paths agree.
    left = pan > 0 ? kUnity * (100 - pan) / 100 : kUnity;
    right = pan < 0 ? kUnity * (100 + pan) / 100 : kUnity;
}

void Mixer::setMasterVolume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    masterPercent_ = percent;
    masterGain_ = volumeToQ15(percent);
}

bool Mixer::audible(int ch) const
{
    if (ch < 0 || ch >= cfg::kMaxChannels)
        return true;
    if (strips_[ch].mute)
        return false;
    for (const auto& s : strips_)
        if (s.solo)
            return strips_[ch].solo != 0;
    return true;
}

void Mixer::refreshSolo()
{
    anyTrackSolo_ = false;
    for (int t = 1; t < cfg::kMixBuses; ++t)
        if (tracks_[t].solo)
            anyTrackSolo_ = true;
}

bool Mixer::trackAudible(int t) const
{
    if (t <= 0 || t >= cfg::kMixBuses)
        return !anyTrackSolo_; // master-routed channels are silenced while an insert is soloed
    if (tracks_[t].mute)
        return false;
    return !anyTrackSolo_ || tracks_[t].solo;
}

void Mixer::clearClips()
{
    memset(trackClip_, 0, sizeof(trackClip_));
}

Mixer::Voice* Mixer::allocVoice(int synthChannel)
{
    Voice* oldest = nullptr;
    if (synthChannel >= 0) {
        // Per-channel polyphony cap for synths: reuse this channel's oldest voice.
        int count = 0;
        for (auto& v : voices_)
            if (v.active && v.isSynth && v.channel == synthChannel) {
                ++count;
                if (!oldest || (int32_t)(v.serial - oldest->serial) < 0)
                    oldest = &v;
            }
        if (count >= cfg::kMaxSynthVoicesPerChannel) {
            ++steals_;
            return oldest;
        }
        oldest = nullptr;
    }
    Voice* releasingOldest = nullptr;
    for (auto& v : voices_) {
        if (!v.active)
            return &v;
        if (!oldest || (int32_t)(v.serial - oldest->serial) < 0)
            oldest = &v;
        if ((v.releasing || v.envStage == kEnvR) && (!releasingOldest || (int32_t)(v.serial - releasingOldest->serial) < 0))
            releasingOldest = &v;
    }
    ++steals_;
    return releasingOldest ? releasingOldest : oldest;
}

void Mixer::declick(Voice& v)
{
    if (v.releasing)
        return;
    v.releasing = 1;
    v.releaseGain = kUnity;
    v.releaseStep = kUnity / kReleaseFrames;
}

void Mixer::endNote(Voice& v)
{
    if (v.releasing || v.envStage == kEnvR)
        return;
    if (v.envOn && v.channel >= 0) {
        int rel = inst_[(int)v.channel].env[kEnvRelease];
        if (rel < 5)
            rel = 5;
        v.envCoef = expCoef(rel, 1);
        v.envCoef16 = expCoef(rel, 16);
        v.envStage = kEnvR;
    } else {
        declick(v);
    }
}

void Mixer::startEnvelope(Voice& v)
{
    v.envLevel = 0;
    v.envCoef = 0;
    v.envCount = 0;
    const int16_t* e = inst_[(int)v.channel].env;
    int a = e[kEnvAttack];
    if (a < 1)
        a = 1;
    v.envInc = kEnvOne / (a * 48);
    if (v.envInc < 1)
        v.envInc = 1;
    v.envStage = kEnvA;
}

// Advances the envelope by `n` frames (n <= 16) and returns the Q24 level at the end. The mixer
// calls this once per 16-frame control block and interpolates the gain in between, so the
// per-sample loops contain no envelope logic. Decay and release use a coefficient for the whole
// block (envCoef16) or, for the short blocks at segment ends, the per-frame coefficient.
int32_t Mixer::envAdvance(Voice& v, const int16_t* e, int n)
{
    const int32_t sus = e[kEnvSustain] * (kEnvOne / 100);
    switch (v.envStage) {
    case kEnvA:
        v.envLevel += v.envInc * n;
        if (v.envLevel >= kEnvOne) {
            v.envLevel = kEnvOne;
            if (e[kEnvHold] > 0) {
                v.envCount = (uint32_t)e[kEnvHold] * 48u;
                v.envStage = kEnvH;
            } else {
                beginDecay(v, e);
            }
        }
        break;
    case kEnvH:
        if (v.envCount > (uint32_t)n) {
            v.envCount -= (uint32_t)n;
        } else {
            v.envCount = 0;
            beginDecay(v, e);
        }
        break;
    case kEnvD:
        if (n == 16) {
            v.envLevel = sus + (int32_t)(((int64_t)(v.envLevel - sus) * v.envCoef16) >> 16);
        } else {
            for (int k = 0; k < n; ++k)
                v.envLevel = sus + (int32_t)(((int64_t)(v.envLevel - sus) * v.envCoef) >> 16);
        }
        if (v.envLevel - sus < (kEnvOne >> 12) && v.envLevel - sus > -(kEnvOne >> 12)) {
            v.envLevel = sus;
            v.envStage = kEnvS;
        }
        break;
    case kEnvS: {
        // Follow live edits of the sustain level without a step.
        const int32_t d = sus - v.envLevel;
        if (d > -64 && d < 64)
            v.envLevel += d;
        else
            v.envLevel += (int32_t)(((int64_t)d * (n > 16 ? 16 : n)) >> 6);
        break;
    }
    case kEnvR:
        if (n == 16) {
            v.envLevel = (int32_t)(((int64_t)v.envLevel * v.envCoef16) >> 16);
        } else {
            for (int k = 0; k < n; ++k)
                v.envLevel = (int32_t)(((int64_t)v.envLevel * v.envCoef) >> 16);
        }
        if (v.envLevel < (kEnvOne >> 11)) { // -66 dB
            v.envLevel = 0;
            v.envStage = kEnvDone;
        }
        break;
    default:
        break;
    }
    return v.envLevel;
}

void Mixer::beginDecay(Voice& v, const int16_t* e)
{
    v.envStage = kEnvD;
    const int d = e[kEnvDecay];
    v.envCoef = d > 0 ? expCoef(d, 1) : 0;
    v.envCoef16 = d > 0 ? expCoef(d, 16) : 0;
}

bool Mixer::trigger(int channel, const Sample* sample, int velocity, int semis, uint32_t gateFrames, bool choke)
{
    if (!sample || !sample->data || sample->frames == 0)
        return false;

    // Choke: fade any voice already sounding on this channel.
    if (channel >= 0 && choke) {
        for (auto& v : voices_)
            if (v.active && v.channel == channel && !v.releasing)
                declick(v);
    }

    Voice* v = allocVoice(-1);
    if (velocity < 0)
        velocity = 0;
    if (velocity > 127)
        velocity = 127;

    memset(v, 0, sizeof(*v));
    v->sample = sample;
    v->endPos = sample->frames;
    // 16.16 rate step = sampleRate / outputRate (built-ins are 48 kHz so this is exactly 1.0).
    uint64_t step64 = ((uint64_t)sample->sampleRate << 16) / (uint32_t)cfg::kSampleRate;
    if (semis != 0)
        step64 = (step64 * pitch::ratioQ16(semis)) >> 16;
    if (step64 > (16ull << 16))
        step64 = 16ull << 16; // never skip more than 16 source frames per output frame
    if (step64 == 0)
        step64 = 1;
    const uint32_t step = (uint32_t)step64;
    v->incInt = step >> 16;
    v->incFrac = step & 0xffff;
    v->velGain = velocity * kUnity / 127;
    v->releaseGain = kUnity;
    v->gateLeft = gateFrames;
    v->serial = ++serial_;
    v->channel = (int8_t)channel;
    v->route = -1;
    v->active = 1;
    if (channel >= 0 && channel < cfg::kMaxChannels && inst_[channel].env[kEnvEnabled]) {
        v->envOn = 1;
        startEnvelope(*v);
    }
    return true;
}

void Mixer::triggerClick(bool accent)
{
    trigger(-1, &click_[accent ? 1 : 0], 110, 0, 0, false);
}

bool Mixer::triggerSynth(int channel, int note, int velocity, uint32_t gateFrames)
{
    if (channel < 0 || channel >= cfg::kMaxChannels)
        return false;
    const int16_t* P = inst_[channel].synth;
    if (velocity < 0)
        velocity = 0;
    if (velocity > 127)
        velocity = 127;
    note += P[kSynTranspose];
    if (note < 0)
        note = 0;
    if (note > 127)
        note = 127;

    Voice* v = allocVoice(channel);
    memset(v, 0, sizeof(*v));
    v->isSynth = 1;
    v->inc1 = synth::phaseInc(note, P[kSynOsc1Fine]);
    v->inc2 = synth::phaseInc(note + P[kSynOsc2Coarse], P[kSynOsc2Detune] + P[kSynOsc1Fine]);
    v->incSub = synth::phaseInc(note - 12, P[kSynOsc1Fine]);
    v->noise = 0x9e3779b9u ^ (uint32_t)(++serial_ * 2654435761u);
    // Keep every voice of the channel in phase-relationship-free start: a fixed phase makes
    // repeated notes identical (and avoids a click from a random start on saw waves).
    v->velGain = velocity * kUnity / 127;
    v->releaseGain = kUnity;
    v->gateLeft = gateFrames ? gateFrames : 1;
    v->serial = serial_;
    v->channel = (int8_t)channel;
    v->route = -1;
    v->active = 1;
    v->envOn = 1;
    startEnvelope(*v);
    return true;
}

bool Mixer::triggerClip(const Sample* sample, int route, int volume, uint32_t startFrame, uint32_t loopStart, uint32_t endFrame,
                        bool loop, uint32_t gateFrames)
{
    if (!sample || !sample->data || sample->frames == 0)
        return false;
    if (endFrame == 0 || endFrame > sample->frames)
        endFrame = sample->frames;
    if (startFrame >= endFrame)
        return false;
    if (route < 0 || route >= cfg::kMixBuses)
        route = 0;
    if (volume < 0)
        volume = 0;
    if (volume > 127)
        volume = 127;

    Voice* v = allocVoice(-1);
    memset(v, 0, sizeof(*v));
    v->sample = sample;
    v->pos = startFrame;
    v->loopStart = loopStart < endFrame ? loopStart : 0;
    v->endPos = endFrame;
    uint64_t step64 = ((uint64_t)sample->sampleRate << 16) / (uint32_t)cfg::kSampleRate;
    if (step64 == 0)
        step64 = 1;
    v->incInt = (uint32_t)(step64 >> 16);
    v->incFrac = (uint32_t)(step64 & 0xffff);
    v->velGain = volume * kUnity / 127;
    v->releaseGain = kUnity;
    v->gateLeft = gateFrames;
    v->serial = ++serial_;
    v->channel = -1;
    v->route = (int8_t)route;
    v->loop = loop ? 1 : 0;
    v->active = 1;
    return true;
}

void Mixer::releaseAll()
{
    for (auto& v : voices_)
        if (v.active && !v.releasing)
            declick(v);
}

int Mixer::stopSample(const Sample* sample)
{
    int n = 0;
    for (auto& v : voices_) {
        if (v.active && v.sample == sample) {
            v.active = 0;
            v.sample = nullptr;
            ++n;
        }
    }
    return n;
}

int Mixer::activeVoices() const
{
    int n = 0;
    for (const auto& v : voices_)
        n += v.active ? 1 : 0;
    return n;
}

void Mixer::beginBlock(int frames)
{
    for (int b = 0; b < cfg::kMixBuses; ++b) {
        memset(acc_[b][0], 0, sizeof(int32_t) * (size_t)frames);
        memset(acc_[b][1], 0, sizeof(int32_t) * (size_t)frames);
    }
    memset(blockPeakCh_, 0, sizeof(blockPeakCh_));
    memset(busDirty_, 0, sizeof(busDirty_));
}

void Mixer::mixSegment(int offset, int frames)
{
    if (frames <= 0)
        return;
    for (auto& v : voices_) {
        if (!v.active)
            continue;
        if (v.gateLeft) {
            if (v.gateLeft > (uint32_t)frames) {
                v.gateLeft -= (uint32_t)frames;
            } else {
                // The note ends inside this segment: mix up to it, then release.
                const int first = (int)v.gateLeft;
                v.gateLeft = 0;
                mixVoice(v, offset, first);
                if (v.active) {
                    endNote(v);
                    if (frames - first > 0)
                        mixVoice(v, offset + first, frames - first);
                }
                continue;
            }
        }
        mixVoice(v, offset, frames);
    }
}

void Mixer::voiceGains(const Voice& v, int32_t& gL, int32_t& gR, int& bus) const
{
    gL = gR = v.velGain;
    bus = 0;
    const int ch = v.channel;
    if (ch >= 0) {
        bus = v.route >= 0 ? v.route : strips_[ch].route;
        if (bus >= cfg::kMixBuses)
            bus = 0;
        if (!audible(ch)) {
            gL = gR = 0;
        } else {
            int32_t pl, pr;
            panToQ15(strips_[ch].pan, pl, pr);
            const int32_t vol = (v.velGain * volumeToQ15(strips_[ch].volume)) >> 15;
            gL = (vol * pl) >> 15;
            gR = (vol * pr) >> 15;
        }
    } else if (v.route >= 0) {
        bus = v.route;
    }
    // A soloed insert silences every other route (including the master bus).
    if (anyTrackSolo_ && (ch >= 0 || v.route >= 0) && !trackAudible(bus))
        gL = gR = 0;
}

void Mixer::mixVoice(Voice& v, int offset, int frames)
{
    if (v.isSynth) {
        mixSynth(v, offset, frames);
        return;
    }
    const Sample* s = v.sample;
    const int ch = v.channel;

    int32_t gL, gR;
    int bus;
    voiceGains(v, gL, gR, bus);
    busDirty_[bus] = 1;

    const int16_t* data = s->data;
    const uint32_t end = v.endPos;
    const bool stereo = s->channels == 2;
    int32_t* outL = acc_[bus][0] + offset;
    int32_t* outR = acc_[bus][1] + offset;
    int32_t peak = 0;

    if (!v.releasing && !v.envOn && !v.loop && v.incInt == 1 && v.incFrac == 0) {
        // Fast path: native-rate playback, no interpolation, no ramp.
        uint32_t n = v.pos < end ? end - v.pos : 0;
        if (n > (uint32_t)frames)
            n = (uint32_t)frames;
        if (stereo) {
            const int16_t* p = data + v.pos * 2;
            for (uint32_t i = 0; i < n; ++i) {
                const int32_t l = p[i * 2], r = p[i * 2 + 1];
                outL[i] += (l * gL) >> 15;
                outR[i] += (r * gR) >> 15;
                const int32_t a = absi(l) > absi(r) ? absi(l) : absi(r);
                if (a > peak)
                    peak = a;
            }
        } else {
            const int16_t* p = data + v.pos;
            for (uint32_t i = 0; i < n; ++i) {
                const int32_t x = p[i];
                outL[i] += (x * gL) >> 15;
                outR[i] += (x * gR) >> 15;
                const int32_t a = absi(x);
                if (a > peak)
                    peak = a;
            }
        }
        v.pos += n;
        if (v.pos >= end)
            v.active = 0;
    } else {
        // General path: interpolation, loops, envelope and/or release ramp. The envelope
        // advances every 16 frames and its gain is interpolated in between (control rate).
        const int16_t* env = (v.envOn && ch >= 0) ? inst_[ch].env : nullptr;
        int i = 0;
        bool stop = false;
        while (i < frames && !stop) {
            int blk = frames - i;
            int32_t eg = 0, egStep = 0; // Q23 (Q15 << 8)
            if (env) {
                if (blk > 16)
                    blk = 16;
                const int32_t a = v.envLevel >> 9;
                const int32_t b = envAdvance(v, env, blk) >> 9;
                eg = a << 8;
                egStep = ((b - a) << 8) / blk;
            }
            for (int j = 0; j < blk; ++j, ++i) {
                if (v.pos >= end) {
                    if (v.loop && end > v.loopStart) {
                        v.pos = v.loopStart + (v.pos - end) % (end - v.loopStart);
                    } else {
                        stop = true;
                        break;
                    }
                }
                if (v.releaseGain <= 0) {
                    stop = true;
                    break;
                }
                const uint32_t nextPos = (v.pos + 1 < end) ? v.pos + 1 : (v.loop ? v.loopStart : v.pos);
                int32_t l, r;
                if (stereo) {
                    const int32_t l0 = data[v.pos * 2], l1 = data[nextPos * 2];
                    const int32_t r0 = data[v.pos * 2 + 1], r1 = data[nextPos * 2 + 1];
                    l = l0 + (((l1 - l0) * (int32_t)v.frac) >> 16);
                    r = r0 + (((r1 - r0) * (int32_t)v.frac) >> 16);
                } else {
                    const int32_t x0 = data[v.pos], x1 = data[nextPos];
                    l = r = x0 + (((x1 - x0) * (int32_t)v.frac) >> 16);
                }
                if (env) {
                    const int32_t e = eg >> 8;
                    eg += egStep;
                    l = (l * e) >> 15;
                    r = (r * e) >> 15;
                }
                if (v.releasing) {
                    l = (l * v.releaseGain) >> 15;
                    r = (r * v.releaseGain) >> 15;
                    v.releaseGain -= v.releaseStep;
                }
                outL[i] += (l * gL) >> 15;
                outR[i] += (r * gR) >> 15;
                const int32_t a = absi(l) > absi(r) ? absi(l) : absi(r);
                if (a > peak)
                    peak = a;

                v.frac += v.incFrac;
                v.pos += v.incInt + (v.frac >> 16);
                v.frac &= 0xffff;
            }
            if (env && v.envStage == kEnvDone)
                stop = true;
        }
        if (stop || (!v.loop && v.pos >= end))
            v.active = 0;
    }

    if (ch >= 0) {
        const int32_t g = gL > gR ? gL : gR;
        const int32_t level = (peak * g) >> 15;
        if (level > blockPeakCh_[ch])
            blockPeakCh_[ch] = level;
    }
}

void Mixer::mixSynth(Voice& v, int offset, int frames)
{
    const int ch = v.channel;
    const int16_t* P = inst_[ch].synth;
    const int16_t* E = inst_[ch].env;

    int32_t gL, gR;
    int bus;
    voiceGains(v, gL, gR, bus);
    busDirty_[bus] = 1;

    const int w1 = P[kSynOsc1Wave], w2 = P[kSynOsc2Wave];
    const bool fm = P[kSynMode] == 1;
    const int32_t l1 = P[kSynOsc1Level] * 328, l2 = P[kSynOsc2Level] * 328;
    const int32_t lsub = P[kSynSubLevel] * 328, lnoise = P[kSynNoiseLevel] * 328;
    const int32_t fmAmt = P[kSynFmAmount];
    // output level and mixing headroom (~0.7), folded into one Q15 gain
    const int32_t levelGain = (((int32_t)P[kSynLevel] * 328) * 22937) >> 15;
    const int ftype = P[kSynFilterType];
    const float q = 0.55f * powf(20.0f, P[kSynResonance] / 100.0f);
    const float k = 1.0f / q;
    const float baseIdx = P[kSynCutoff] * 10.23f;
    const float envAmt = P[kSynFilterEnv] * 0.01f;
    const int lfoDepth = P[kSynLfoDepth];
    const int lfoDest = P[kSynLfoTarget];
    const uint32_t lfoInc = (uint32_t)((float)params::lfoRateCentiHz(P[kSynLfoRate]) * 0.01f / (float)cfg::kSampleRate * 4294967296.0f);
    const bool modFilter = ftype && (envAmt != 0.0f || (lfoDepth && lfoDest == 1));
    float a1 = 0, a2 = 0, a3 = 0;
    if (ftype && !modFilter) {
        const float g = synth::filterG((int)baseIdx);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    int32_t* outL = acc_[bus][0] + offset;
    int32_t* outR = acc_[bus][1] + offset;
    int32_t peak = 0;
    uint32_t i1 = v.inc1, i2 = v.inc2, isub = v.incSub;
    float ic1 = v.ic1, ic2 = v.ic2;
    uint32_t ph1 = v.ph1, ph2 = v.ph2, phSub = v.phSub, noise = v.noise;

    int i = 0;
    while (i < frames) {
        if (v.releaseGain <= 0) {
            v.active = 0;
            break;
        }
        int blk = frames - i;
        if (blk > 16)
            blk = 16;

        // ---- control rate: envelope, LFO, filter coefficients ----
        const int32_t eqStart = v.envLevel;
        const int32_t eqEnd = envAdvance(v, E, blk);
        int32_t lfo = 0; // triangle, -32768..32767
        if (lfoDepth) {
            v.lfoPh += lfoInc * (uint32_t)blk;
            int32_t t = (int32_t)(v.lfoPh >> 15) - 65536;
            if (t < 0)
                t = -t;
            lfo = t - 32768;
            if (lfo > 32767)
                lfo = 32767;
            if (lfoDest == 0) {
                const float r = (float)lfo * (float)lfoDepth * (0.0595f / (32768.0f * 100.0f)); // +-1 semitone at full depth
                i1 = v.inc1 + (uint32_t)(int32_t)((float)v.inc1 * r);
                i2 = v.inc2 + (uint32_t)(int32_t)((float)v.inc2 * r);
                isub = v.incSub + (uint32_t)(int32_t)((float)v.incSub * r);
            }
        }
        if (modFilter) {
            float idx = baseIdx + envAmt * ((float)eqEnd * (1.0f / 16777216.0f)) * 600.0f;
            if (lfoDepth && lfoDest == 1)
                idx += (float)lfo * (float)lfoDepth * (300.0f / (32768.0f * 100.0f));
            const float g = synth::filterG(idx < 0 ? 0 : (int)idx);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        // amplitude ramp over the block: envelope x output level (x tremolo)
        int32_t gain0 = (int32_t)(((int64_t)(eqStart >> 9) * levelGain) >> 15);
        int32_t gain1 = (int32_t)(((int64_t)(eqEnd >> 9) * levelGain) >> 15);
        if (lfoDepth && lfoDest == 2) {
            const int32_t trem = 32767 - (int32_t)(((int64_t)(lfo + 32768) * lfoDepth) / 200);
            gain0 = (int32_t)(((int64_t)gain0 * trem) >> 15);
            gain1 = (int32_t)(((int64_t)gain1 * trem) >> 15);
        }
        int32_t gcur = gain0 << 8;
        const int32_t gstep = ((gain1 - gain0) << 8) / blk;

        // ---- audio rate ----
        for (int j = 0; j < blk; ++j, ++i) {
            ph1 += i1;
            ph2 += i2;
            phSub += isub;

            int32_t s;
            if (fm) {
                const int32_t mod = synth::osc(w2, ph2, noise);
                const uint32_t pm = (uint32_t)((int64_t)mod * fmAmt * 1704);
                s = (synth::osc(w1, ph1 + pm, noise) * l1) >> 15;
            } else {
                s = (synth::osc(w1, ph1, noise) * l1) >> 15;
                if (l2)
                    s += (synth::osc(w2, ph2, noise) * l2) >> 15;
            }
            if (lsub)
                s += (synth::osc(synth::Sine, phSub, noise) * lsub) >> 15;
            if (lnoise)
                s += (synth::osc(synth::Noise, 0, noise) * lnoise) >> 15;

            if (ftype) {
                const float x = (float)s * (1.0f / 32768.0f);
                const float v3 = x - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                const float y = ftype == 1 ? v2 : (ftype == 2 ? x - k * v1 - v2 : v1);
                s = roundToInt(y * 32768.0f);
                if (s > 98304)
                    s = 98304;
                if (s < -98304)
                    s = -98304;
            }

            s = (s * (gcur >> 8)) >> 15;
            gcur += gstep;
            if (v.releasing) {
                s = (s * v.releaseGain) >> 15;
                v.releaseGain -= v.releaseStep;
            }
            outL[i] += (s * gL) >> 15;
            outR[i] += (s * gR) >> 15;
            const int32_t a = absi(s);
            if (a > peak)
                peak = a;
        }
        if (v.envStage == kEnvDone) {
            v.active = 0;
            break;
        }
    }
    v.ph1 = ph1;
    v.ph2 = ph2;
    v.phSub = phSub;
    v.noise = noise;
    v.ic1 = ic1;
    v.ic2 = ic2;

    const int32_t g = gL > gR ? gL : gR;
    const int32_t level = (peak * g) >> 15;
    if (level > blockPeakCh_[ch])
        blockPeakCh_[ch] = level;
}

bool Mixer::runChain(int bus, int frames)
{
    bool anyFx = false, tail = false;
    for (int s = 0; s < cfg::kFxSlots; ++s) {
        const FxUnit& u = fx_[bus][s];
        if (u.active() && !u.starved()) {
            anyFx = true;
            tail |= u.hasTail();
        }
    }
    trackGr_[bus] = 0;
    if (!anyFx)
        return busDirty_[bus] != 0;

    int32_t* aL = acc_[bus][0];
    int32_t* aR = acc_[bus][1];
    bool nonZero = false;
    for (int i = 0; i < frames; ++i)
        if (aL[i] | aR[i]) {
            nonZero = true;
            break;
        }
    if (nonZero) {
        busTail_[bus] = tail ? kLongTail : kShortTail;
    } else if (busTail_[bus] > (uint32_t)frames) {
        busTail_[bus] -= (uint32_t)frames;
    } else {
        busTail_[bus] = 0;
        return false; // idle: nothing in, nothing left ringing
    }

    float* l = fxBuf_[0];
    float* r = fxBuf_[1];
    for (int i = 0; i < frames; ++i) {
        l[i] = (float)aL[i];
        r[i] = (float)aR[i];
    }
    int gr = 0;
    for (int s = 0; s < cfg::kFxSlots; ++s) {
        FxUnit& u = fx_[bus][s];
        if (!u.active())
            continue;
        u.process(l, r, frames, fxTmp_[0], fxTmp_[1]);
        if (u.type() == FxType::Compressor && u.gainReductionDeci() > gr)
            gr = u.gainReductionDeci();
    }
    trackGr_[bus] = (int16_t)gr;
    for (int i = 0; i < frames; ++i) {
        aL[i] = roundToInt(l[i]);
        aR[i] = roundToInt(r[i]);
    }
    return true;
}

uint32_t Mixer::finishBlock(int16_t* out, int frames)
{
    // Insert tracks: effects, fader, meters, then into the master bus.
    int32_t* mL = acc_[0][0];
    int32_t* mR = acc_[0][1];
    uint64_t fxTicks = 0;
    for (int t = 1; t < cfg::kMixBuses; ++t) {
        const uint64_t f0 = clock_ ? clock_() : 0;
        const bool live = runChain(t, frames);
        if (clock_)
            fxTicks += clock_() - f0;
        int32_t* aL = acc_[t][0];
        int32_t* aR = acc_[t][1];

        int32_t tl = 0, tr = 0;
        if (trackAudible(t)) {
            int32_t pl, pr;
            panToQ15(tracks_[t].pan, pl, pr);
            const int32_t vol = volumeToQ15(tracks_[t].volume);
            tl = (vol * pl) >> 15;
            tr = (vol * pr) >> 15;
        }
        int32_t gl = trackGainL_[t] << 8, gr = trackGainR_[t] << 8; // Q23 for a smooth ramp
        const int32_t sl = ((tl << 8) - gl) / frames, sr = ((tr << 8) - gr) / frames;

        int32_t peakL = 0, peakR = 0;
        // No input and no effect tail: the bus is silent whatever its fader does.
        const bool idle = !live || (trackGainL_[t] == 0 && trackGainR_[t] == 0 && tl == 0 && tr == 0);
        if (!idle) {
            for (int i = 0; i < frames; ++i) {
                gl += sl;
                gr += sr;
                const int32_t l = (int32_t)(((int64_t)aL[i] * (gl >> 8)) >> 15);
                const int32_t r = (int32_t)(((int64_t)aR[i] * (gr >> 8)) >> 15);
                mL[i] += l;
                mR[i] += r;
                if (absi(l) > peakL)
                    peakL = absi(l);
                if (absi(r) > peakR)
                    peakR = absi(r);
            }
        }
        trackGainL_[t] = tl;
        trackGainR_[t] = tr;
        if (peakL > 32767 || peakR > 32767)
            trackClip_[t] = 1;
        trackMeterL_[t] = decayMeter(trackMeterL_[t], peakL);
        trackMeterR_[t] = decayMeter(trackMeterR_[t], peakR);
    }

    // Master chain, gain, saturation.
    const uint64_t m0 = clock_ ? clock_() : 0;
    runChain(0, frames);
    if (clock_)
        fxUs_ = (uint32_t)(fxTicks + (clock_() - m0));
    uint32_t clips = 0;
    int32_t peakL = 0, peakR = 0;
    for (int i = 0; i < frames; ++i) {
        // 32x32 -> 64-bit product: the R5900 MULT gives this in HI/LO cheaply,
        // and the bus can exceed 16 bits before the master gain is applied.
        const int32_t l = (int32_t)(((int64_t)mL[i] * masterGain_) >> 15);
        const int32_t r = (int32_t)(((int64_t)mR[i] * masterGain_) >> 15);
        const int16_t sl = saturate16(l, clips);
        const int16_t sr = saturate16(r, clips);
        out[i * 2] = sl;
        out[i * 2 + 1] = sr;
        if (absi(sl) > peakL)
            peakL = absi(sl);
        if (absi(sr) > peakR)
            peakR = absi(sr);
    }
    if (clips)
        trackClip_[0] = 1;

    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        meterCh_[ch] = decayMeter(meterCh_[ch], blockPeakCh_[ch]);
    meterL_ = decayMeter(meterL_, peakL);
    meterR_ = decayMeter(meterR_, peakR);
    trackMeterL_[0] = meterL_;
    trackMeterR_[0] = meterR_;
    return clips;
}
