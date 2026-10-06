#include "audio/mixer.hpp"

#include <string.h>

namespace {

constexpr int32_t kUnity = 32767;
constexpr int kReleaseFrames = 256; // ~5 ms declick

inline int32_t absi(int32_t v) { return v < 0 ? -v : v; }

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

} // namespace

Mixer::Mixer() : meterL_(0), meterR_(0), masterPercent_(0), masterGain_(0), serial_(0), steals_(0)
{
    memset(voices_, 0, sizeof(voices_));
    memset(accL_, 0, sizeof(accL_));
    memset(accR_, 0, sizeof(accR_));
    memset(blockPeakCh_, 0, sizeof(blockPeakCh_));
    memset(meterCh_, 0, sizeof(meterCh_));
    setMasterVolume(80);
}

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

Mixer::Voice* Mixer::allocVoice()
{
    Voice* oldest = nullptr;
    for (auto& v : voices_) {
        if (!v.active)
            return &v;
        if (!oldest || (int32_t)(v.serial - oldest->serial) < 0)
            oldest = &v;
    }
    ++steals_;
    return oldest;
}

bool Mixer::trigger(int channel, const Sample* sample, int velocity)
{
    if (!sample || !sample->data || sample->frames == 0)
        return false;

    // Choke: fade any voice already sounding on this channel.
    if (channel >= 0) {
        for (auto& v : voices_) {
            if (v.active && v.channel == channel && !v.releasing) {
                v.releasing = 1;
                v.releaseGain = kUnity;
                v.releaseStep = kUnity / kReleaseFrames;
            }
        }
    }

    Voice* v = allocVoice();
    if (velocity < 0)
        velocity = 0;
    if (velocity > 127)
        velocity = 127;

    v->sample = sample;
    v->pos = 0;
    v->frac = 0;
    // 16.16 rate step = sampleRate / outputRate (pitch control arrives with
    // the piano roll; built-ins are 48 kHz so this is exactly 1.0 today).
    const uint32_t step = (uint32_t)(((uint64_t)sample->sampleRate << 16) / (uint32_t)cfg::kSampleRate);
    v->incInt = step >> 16;
    v->incFrac = step & 0xffff;
    v->velGain = velocity * kUnity / 127;
    v->releaseGain = kUnity;
    v->releaseStep = 0;
    v->serial = ++serial_;
    v->channel = (int8_t)channel;
    v->releasing = 0;
    v->active = 1;
    return true;
}

void Mixer::releaseAll()
{
    for (auto& v : voices_) {
        if (v.active && !v.releasing) {
            v.releasing = 1;
            v.releaseGain = kUnity;
            v.releaseStep = kUnity / kReleaseFrames;
        }
    }
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
    memset(accL_, 0, sizeof(int32_t) * (size_t)frames);
    memset(accR_, 0, sizeof(int32_t) * (size_t)frames);
    memset(blockPeakCh_, 0, sizeof(blockPeakCh_));
}

void Mixer::mixSegment(int offset, int frames)
{
    if (frames <= 0)
        return;
    for (auto& v : voices_)
        if (v.active)
            mixVoice(v, offset, frames);
}

void Mixer::mixVoice(Voice& v, int offset, int frames)
{
    const Sample* s = v.sample;
    const int ch = v.channel;

    int32_t gL = v.velGain, gR = v.velGain;
    if (ch >= 0) {
        if (!audible(ch)) {
            gL = gR = 0;
        } else {
            int32_t pl, pr;
            panToQ15(strips_[ch].pan, pl, pr);
            const int32_t vol = (v.velGain * volumeToQ15(strips_[ch].volume)) >> 15;
            gL = (vol * pl) >> 15;
            gR = (vol * pr) >> 15;
        }
    }

    const int16_t* data = s->data;
    const uint32_t len = s->frames;
    const bool stereo = s->channels == 2;
    int32_t* outL = accL_ + offset;
    int32_t* outR = accR_ + offset;
    int32_t peak = 0;

    if (!v.releasing && v.incInt == 1 && v.incFrac == 0) {
        // Fast path: native-rate playback, no interpolation, no ramp.
        uint32_t n = len - v.pos;
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
        if (v.pos >= len)
            v.active = 0;
    } else {
        // General path: linear interpolation and/or release ramp.
        for (int i = 0; i < frames; ++i) {
            if (v.pos >= len || v.releaseGain <= 0) {
                v.active = 0;
                break;
            }
            const uint32_t nextPos = (v.pos + 1 < len) ? v.pos + 1 : v.pos;
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
        if (v.pos >= len)
            v.active = 0;
    }

    if (ch >= 0) {
        const int32_t g = gL > gR ? gL : gR;
        const int32_t level = (peak * g) >> 15;
        if (level > blockPeakCh_[ch])
            blockPeakCh_[ch] = level;
    }
}

uint32_t Mixer::finishBlock(int16_t* out, int frames)
{
    uint32_t clips = 0;
    int32_t peakL = 0, peakR = 0;
    for (int i = 0; i < frames; ++i) {
        // 32x32 -> 64-bit product: the R5900 MULT gives this in HI/LO cheaply,
        // and the bus can exceed 16 bits before the master gain is applied.
        const int32_t l = (int32_t)(((int64_t)accL_[i] * masterGain_) >> 15);
        const int32_t r = (int32_t)(((int64_t)accR_[i] * masterGain_) >> 15);
        const int16_t sl = saturate16(l, clips);
        const int16_t sr = saturate16(r, clips);
        out[i * 2] = sl;
        out[i * 2 + 1] = sr;
        if (absi(sl) > peakL)
            peakL = absi(sl);
        if (absi(sr) > peakR)
            peakR = absi(sr);
    }

    // Meters: instant attack, ~-0.56 dB per block release.
    auto decay = [](uint16_t m, int32_t p) -> uint16_t {
        int32_t d = m - (m >> 4);
        if (p > d)
            d = p;
        return (uint16_t)(d > 32767 ? 32767 : d);
    };
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        meterCh_[ch] = decay(meterCh_[ch], blockPeakCh_[ch]);
    meterL_ = decay(meterL_, peakL);
    meterR_ = decay(meterR_, peakR);
    return clips;
}
