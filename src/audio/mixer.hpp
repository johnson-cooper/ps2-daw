// Software mixer: a fixed voice pool feeding eight channel strips and a
// master bus.
//
// Accumulation is done in 32-bit integers (Q15 gains), then the master bus is
// saturated to 16-bit with a clip counter. Nothing here allocates; every
// buffer is a member sized for cfg::kMaxBlockFrames.
//
// Gains are evaluated from the live strip state on every render segment, so
// volume, pan and mute changes also affect voices that are already ringing.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "audio/sample.hpp"

struct ChannelStrip {
    uint8_t volume = 78;  // percent, squared-law curve
    int8_t pan = 0;       // -100 .. 100, balance law
    uint8_t mute = 0;
    uint8_t solo = 0;
};

class Mixer {
public:
    Mixer();

    ChannelStrip& strip(int ch) { return strips_[ch]; }
    const ChannelStrip& strip(int ch) const { return strips_[ch]; }
    void setMasterVolume(int percent);
    int masterVolume() const { return masterPercent_; }

    // True if the channel should currently be heard (mute + solo logic).
    bool audible(int ch) const;

    // Starts a voice for `channel` (or -1 for a channel-less preview).
    // Any voice already playing on the same channel is faded out quickly
    // (a declicked choke). Returns false if `sample` is not ready.
    // `semis` transposes by resampling (0 = native speed). A nonzero
    // `gateFrames` fades the voice out after that many output frames (note
    // length). With `choke` false, voices already sounding on the channel
    // keep playing (chords, sustained notes).
    bool trigger(int channel, const Sample* sample, int velocity, int semis = 0, uint32_t gateFrames = 0, bool choke = true);

    // Fades every voice out over a few milliseconds.
    void releaseAll();
    // Immediately silences every voice reading `sample` (its memory is about
    // to be freed, so no fade). Returns how many were stopped.
    int stopSample(const Sample* sample);

    // Clears the accumulators for a new block.
    void beginBlock(int frames);
    // Mixes all active voices into accumulator frames [offset, offset+frames).
    void mixSegment(int offset, int frames);
    // Applies master gain, saturates into `out` (interleaved stereo), updates
    // meters. Returns the number of clipped samples in this block.
    uint32_t finishBlock(int16_t* out, int frames);

    int activeVoices() const;
    uint32_t voiceSteals() const { return steals_; }

    // Decaying peak meters, 0..32767, written once per block.
    uint16_t channelMeter(int ch) const { return meterCh_[ch]; }
    uint16_t masterMeterL() const { return meterL_; }
    uint16_t masterMeterR() const { return meterR_; }

    // Q15 gain curves shared with the SPU2 path so both sound alike.
    static int32_t volumeToQ15(int percent);
    static void panToQ15(int pan, int32_t& left, int32_t& right);

private:
    struct Voice {
        const Sample* sample;
        uint32_t pos;       // integer frame position
        uint32_t frac;      // 16-bit fractional position
        uint32_t incInt;    // integer part of the rate step
        uint32_t incFrac;   // 16-bit fractional part of the rate step
        int32_t velGain;    // Q15
        int32_t releaseGain;// Q15, decremented per frame while releasing
        int32_t releaseStep;
        uint32_t gateLeft;  // frames until the note ends (0 = plays to the end)
        uint32_t serial;    // age, for stealing
        int8_t channel;
        uint8_t active;
        uint8_t releasing;
    };

    void mixVoice(Voice& v, int offset, int frames);
    Voice* allocVoice();

    ChannelStrip strips_[cfg::kMaxChannels];
    Voice voices_[cfg::kMaxVoices];
    int32_t accL_[cfg::kMaxBlockFrames];
    int32_t accR_[cfg::kMaxBlockFrames];
    int32_t blockPeakCh_[cfg::kMaxChannels];
    uint16_t meterCh_[cfg::kMaxChannels];
    uint16_t meterL_, meterR_;
    int masterPercent_;
    int32_t masterGain_;
    uint32_t serial_;
    uint32_t steals_;
};
