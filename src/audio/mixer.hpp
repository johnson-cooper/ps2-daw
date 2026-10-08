// Software mixer: a fixed voice pool feeding routed buses and a master bus.
//
//   voices ──► channel strip (volume, pan, mute/solo)
//          ──► route ──► insert track 1..8 (effect chain, fader, pan, mute/solo)
//          ──► master (effect chain, master gain, saturation) ──► PCM stream
//
// Voices are sampler voices (one-shots, note-length gated notes), native
// synthesizer voices, or audio-clip voices started by the playlist. Accumulation
// is done in 32-bit integers (Q15 gains); each bus has its own accumulator, so a
// route change also redirects voices that are already ringing. The master bus
// is saturated to 16-bit with a clip counter. Nothing here allocates while
// rendering; every buffer is a member sized for cfg::kMaxBlockFrames and the
// effect memory pool is created once in the constructor.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "audio/fx.hpp"
#include "audio/instrument.hpp"
#include "audio/sample.hpp"

struct ChannelStrip {
    uint8_t volume = 78;  // percent, squared-law curve
    int8_t pan = 0;       // -100 .. 100, balance law
    uint8_t mute = 0;
    uint8_t solo = 0;
    uint8_t route = 0;    // 0 = master, 1..cfg::kMixTracks = insert track
};

struct MixTrackStrip {
    uint8_t volume = 100;
    int8_t pan = 0;
    uint8_t mute = 0;
    uint8_t solo = 0;
};

class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    ChannelStrip& strip(int ch) { return strips_[ch]; }
    const ChannelStrip& strip(int ch) const { return strips_[ch]; }
    InstrumentData& instrument(int ch) { return inst_[ch]; }
    const InstrumentData& instrument(int ch) const { return inst_[ch]; }
    // Insert track 1..kMixTracks (index 0 is the master bus; its fader is the master volume).
    MixTrackStrip& track(int t) { return tracks_[t]; }
    const MixTrackStrip& track(int t) const { return tracks_[t]; }
    FxUnit& fx(int t, int slot) { return fx_[t][slot]; }
    const FxUnit& fx(int t, int slot) const { return fx_[t][slot]; }
    FxMemory& fxMemory() { return *fxMem_; }
    void setMasterVolume(int percent);
    int masterVolume() const { return masterPercent_; }

    // True if the channel should currently be heard (channel mute + solo logic).
    bool audible(int ch) const;
    // Track-level mute/solo (an insert; 0 = master is audible unless an insert is soloed).
    bool trackAudible(int t) const;
    bool anyTrackSolo() const { return anyTrackSolo_; }
    // Recomputes cached solo state after editing track()/strip() directly.
    void refreshSolo();

    // Starts a sampler voice for `channel` (or -1 for a channel-less preview).
    // Any voice already playing on the same channel is faded out quickly
    // (a declicked choke). Returns false if `sample` is not ready.
    // `semis` transposes by resampling (0 = native speed). A nonzero
    // `gateFrames` ends the note (envelope release, or a short fade) after that
    // many output frames. With `choke` false, voices already sounding on the
    // channel keep playing (chords, sustained notes).
    bool trigger(int channel, const Sample* sample, int velocity, int semis = 0, uint32_t gateFrames = 0, bool choke = true);
    // Metronome click (accent = bar start). Plays straight into the master bus.
    void triggerClick(bool accent);

    // Starts a native synthesizer voice (MIDI `note`, 60 = middle C).
    bool triggerSynth(int channel, int note, int velocity, uint32_t gateFrames);
    // Starts a playlist audio clip: plays `sample` from `startFrame` up to
    // `endFrame` (a looping clip wraps back to `loopStart`), straight into
    // insert `route`, for at most `gateFrames` output frames (0 = until the
    // region ends). `volume` is 0..127.
    bool triggerClip(const Sample* sample, int route, int volume, uint32_t startFrame, uint32_t loopStart, uint32_t endFrame,
                     bool loop, uint32_t gateFrames);

    // Fades every voice out over a few milliseconds.
    void releaseAll();
    // Immediately silences every voice reading `sample` (its memory is about
    // to be freed, so no fade). Returns how many were stopped.
    int stopSample(const Sample* sample);

    // Clears the accumulators for a new block.
    void beginBlock(int frames);
    // Mixes all active voices into accumulator frames [offset, offset+frames).
    void mixSegment(int offset, int frames);
    // Runs the insert/master chains, applies master gain, saturates into `out`
    // (interleaved stereo), updates meters. Returns the number of clipped
    // samples in this block.
    uint32_t finishBlock(int16_t* out, int frames);

    // Optional profiling clock (microseconds). When set, finishBlock() measures the
    // time spent in effect chains (read with fxMicros()). Costs two clock reads per block.
    void setClock(uint64_t (*clock)()) { clock_ = clock; }
    uint32_t fxMicros() const { return fxUs_; }

    int activeVoices() const;
    uint32_t voiceSteals() const { return steals_; }

    // Decaying peak meters, 0..32767, written once per block.
    uint16_t channelMeter(int ch) const { return meterCh_[ch]; }
    uint16_t masterMeterL() const { return meterL_; }
    uint16_t masterMeterR() const { return meterR_; }
    // Per bus (0 = master): post-fader peaks and a latched "exceeded full scale" flag.
    uint16_t trackMeterL(int t) const { return trackMeterL_[t]; }
    uint16_t trackMeterR(int t) const { return trackMeterR_[t]; }
    bool trackClipped(int t) const { return trackClip_[t] != 0; }
    void clearClips();
    // Compressor gain reduction (tenths of a dB) of the worst compressor on a bus.
    int trackGainReductionDeci(int t) const { return trackGr_[t]; }

    // Q15 gain curves shared with the SPU2 path so both sound alike.
    static int32_t volumeToQ15(int percent);
    static void panToQ15(int pan, int32_t& left, int32_t& right);

private:
    enum EnvStage : uint8_t { kEnvDone = 0, kEnvA, kEnvH, kEnvD, kEnvS, kEnvR };

    struct Voice {
        const Sample* sample;   // null for synth voices
        uint32_t pos;           // integer frame position
        uint32_t frac;          // 16-bit fractional position
        uint32_t incInt;        // integer part of the rate step
        uint32_t incFrac;       // 16-bit fractional part of the rate step
        uint32_t endPos;        // one past the last frame to play
        uint32_t loopStart;
        int32_t velGain;        // Q15
        int32_t releaseGain;    // Q15, decremented per frame while releasing
        int32_t releaseStep;
        uint32_t gateLeft;      // frames until the note ends (0 = plays to the end)
        uint32_t serial;        // age, for stealing
        // envelope (Q24 level)
        int32_t envLevel;
        int32_t envInc;         // attack step per frame
        int32_t envCoef;        // decay/release multiplier per frame (Q16)
        int32_t envCoef16;      // the same over a 16-frame control block
        uint32_t envCount;      // hold frames left
        // synth
        uint32_t ph1, ph2, phSub, lfoPh, noise;
        uint32_t inc1, inc2, incSub;
        float ic1, ic2;
        int8_t channel;
        int8_t route;           // -1 = follow the channel's route
        uint8_t active;
        uint8_t releasing;      // declick fade in progress
        uint8_t loop;
        uint8_t isSynth;
        uint8_t envOn;
        uint8_t envStage;
    };

    void mixVoice(Voice& v, int offset, int frames);
    void mixSynth(Voice& v, int offset, int frames);
    void voiceGains(const Voice& v, int32_t& gL, int32_t& gR, int& bus) const;
    Voice* allocVoice(int synthChannel);
    void declick(Voice& v);
    void endNote(Voice& v);
    void startEnvelope(Voice& v);
    int32_t envAdvance(Voice& v, const int16_t* e, int n);
    void beginDecay(Voice& v, const int16_t* e);
    bool runChain(int bus, int frames); // true when the bus carries signal (input or an effect tail)

    ChannelStrip strips_[cfg::kMaxChannels];
    InstrumentData inst_[cfg::kMaxChannels];
    MixTrackStrip tracks_[cfg::kMixBuses];
    FxUnit fx_[cfg::kMixBuses][cfg::kFxSlots];
    FxMemory* fxMem_;
    Voice voices_[cfg::kMaxVoices];
    int32_t acc_[cfg::kMixBuses][2][cfg::kMaxBlockFrames];
    float fxBuf_[2][cfg::kMaxBlockFrames];
    float fxTmp_[2][cfg::kMaxBlockFrames];
    int32_t blockPeakCh_[cfg::kMaxChannels];
    uint16_t meterCh_[cfg::kMaxChannels];
    uint16_t trackMeterL_[cfg::kMixBuses], trackMeterR_[cfg::kMixBuses];
    uint8_t trackClip_[cfg::kMixBuses];
    int16_t trackGr_[cfg::kMixBuses];
    int32_t trackGainL_[cfg::kMixBuses], trackGainR_[cfg::kMixBuses]; // Q15, ramped per block
    uint8_t busDirty_[cfg::kMixBuses]; // a voice mixed into this bus during the current block
    uint32_t busTail_[cfg::kMixBuses]; // frames left before an idle bus with a tail effect is skipped
    uint16_t meterL_, meterR_;
    int masterPercent_;
    int32_t masterGain_;
    bool anyTrackSolo_;
    uint32_t serial_;
    uint32_t steals_;
    int16_t clickPcm_[2][2400];
    Sample click_[2];
    uint64_t (*clock_)() = nullptr;
    uint32_t fxUs_ = 0;
};
