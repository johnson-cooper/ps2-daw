// Platform-independent audio engine: transport + sequencer + mixer.
//
// Threading contract (see docs/ARCHITECTURE.md):
//  * post() is called only by the UI thread.
//  * render() and hardware-trigger access are called only by the audio thread.
//  * status() fields are written by the audio thread and may be read by any
//    thread for display; each field is a naturally aligned 32-bit (or smaller)
//    value, so individual reads are never torn.
//
// render() performs no allocation, no I/O and takes no locks.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "audio/engine_command.hpp"
#include "audio/mixer.hpp"
#include "audio/sample.hpp"
#include "audio/sequencer.hpp"
#include "audio/transport.hpp"
#include "core/spsc_queue.hpp"

// A note the platform should play on an SPU2 hardware voice. `frame` is the
// absolute engine frame at which it should be heard.
struct HwTrigger {
    uint32_t frame;
    uint8_t channel;    // rack channel 0..7, or 0xff for a preview
    uint8_t sample;     // sample bank slot
    uint8_t volume;     // 0..100 (already includes strip volume and velocity)
    int8_t pan;         // -100..100
    int8_t semis;       // transpose from the sample's root, semitones
};

// One fired step, for the UI playhead. The UI shows the latest mark whose
// frame has actually been heard, so the cursor matches the sound rather than
// running ahead by the output latency.
struct StepMark {
    uint32_t frame;
    uint32_t songStep;  // absolute step in the song (playlist mode); equals `step` otherwise
    uint8_t step;       // step within the pattern that fired
    uint8_t pattern;    // pattern that fired (the first active clip's in song mode)
};

struct EngineStatus {
    static constexpr int kMarks = 16;

    volatile uint32_t renderedFrames = 0;  // absolute engine clock (wraps after ~24 h)
    volatile uint8_t transport = 0;        // Transport::State
    volatile uint8_t pattern = 0;
    volatile uint8_t queuedPattern = 0xff; // pending switch target, 0xff = none
    volatile uint8_t songMode = 0;         // 1 while the playlist drives playback
    volatile uint16_t songBars = 0;        // length of the song being played
    volatile uint32_t bpmCenti = cfg::kDefaultBpmCenti;
    volatile uint32_t songFrames = 0;

    StepMark marks[kMarks];
    volatile uint32_t markSerial = 0;      // count of marks ever written

    volatile uint32_t lastTriggerFrame[cfg::kMaxChannels] = {};
    volatile uint32_t triggerCount[cfg::kMaxChannels] = {};
    volatile uint16_t meter[cfg::kMaxChannels] = {};
    volatile uint16_t meterL = 0, meterR = 0;

    volatile uint32_t clipSamples = 0;
    volatile uint32_t voicesActive = 0;
    volatile uint32_t voiceSteals = 0;
    volatile uint32_t commands = 0;
    volatile uint32_t hwTriggers = 0;
    volatile uint32_t hwDropped = 0;

    // Mixer buses: index 0 is the master, 1..8 the inserts. Post-fader peaks.
    volatile uint16_t busMeterL[cfg::kMixBuses] = {};
    volatile uint16_t busMeterR[cfg::kMixBuses] = {};
    volatile uint8_t busClip[cfg::kMixBuses] = {};
    volatile int16_t busGr[cfg::kMixBuses] = {};   // compressor gain reduction, tenths of a dB
    volatile uint8_t fxStarved = 0;                // bit n set: a delay/reverb found no memory (any bus)
    volatile uint8_t exportDone = 0;               // the one-shot pass finished (export mode)
    volatile uint32_t exportSkipped = 0;           // notes that could not be rendered (SPU2-only samples)
    volatile uint32_t audioClipsPlayed = 0;

    // Smoothed per-block timings in microseconds (only when a profile clock is set).
    volatile uint32_t profCommandsUs = 0, profVoicesUs = 0, profFxUs = 0, profFinishUs = 0;
};

class AudioEngine {
public:
    AudioEngine();

    void setSampleBank(const SampleBank* bank) { bank_ = bank; }
    // Optional profiling clock in microseconds (see EngineStatus::prof*).
    void setProfileClock(uint64_t (*clock)())
    {
        clock_ = clock;
        mixer_.setClock(clock);
    }

    // UI thread. Returns false if the queue is full (caller may retry).
    bool post(const Command& cmd) { return queue_.push(cmd); }
    uint32_t queuedCommands() const { return queue_.size(); }

    // Audio thread. Renders `frames` (<= cfg::kMaxBlockFrames) interleaved
    // stereo frames into `out`.
    void render(int16_t* out, int frames);

    // Audio thread, valid after render() until the next render().
    int hwTriggerCount() const { return hwCount_; }
    const HwTrigger& hwTrigger(int i) const { return hw_[i]; }

    const EngineStatus& status() const { return status_; }

private:
    static constexpr int kMaxHwPerBlock = 32;

    void applyCommands();
    void apply(const Command& c);
    void fireStep(int step, uint32_t frameInBlock);
    void fireSongStep(int songStep, uint32_t frameInBlock);
    bool songActive() const { return songMode_ && (clipCount_ > 0 || aclipCount_ > 0) && songSteps_ > 0; }
    void recomputeSong();
    void triggerChannel(int ch, int velocity, uint32_t frameInBlock, int semis = 0, int gateTicks = 0);
    void fireNotes(int pattern, int localStep, uint32_t frameInBlock, uint16_t mask = 0xffff);
    bool trackAudible(int track) const;
    bool triggerSample(int ch, int sampleSlot, int velocity, VoiceMode mode, uint32_t frameInBlock, int semis = 0, int gateTicks = 0);
    uint32_t framesForSteps(int steps) const;
    uint32_t framesForTicks(int ticks) const;
    void firePending(uint32_t absFrame, uint32_t frameInBlock);
    void queueNote(int ch, int velocity, uint32_t dueFrame, int semis, int gateTicks);
    void triggerAudioClips(int songStep, uint32_t frameInBlock);
    void playAudioClip(int index, int elapsedSteps, uint32_t frameInBlock);
    void publish();

    SpscQueue<Command, 1024> queue_;
    Transport transport_;
    Sequencer sequencer_;
    Mixer mixer_;
    const SampleBank* bank_;

    int8_t channelSample_[cfg::kMaxChannels];
    uint8_t channelGate_[cfg::kMaxChannels];
    bool chokeDone_[cfg::kMaxChannels]; // a voice on this channel was already (re)triggered this step
    uint8_t trackMute_, trackSolo_;
    VoiceMode channelMode_[cfg::kMaxChannels];
    uint8_t sampleHwReady_[cfg::kMaxSamples];

    HwTrigger hw_[kMaxHwPerBlock];
    int hwCount_;
    uint64_t (*clock_)() = nullptr;
    uint32_t blockStart_;
    int queuedPattern_;     // -1 = none
    int queuedQuantum_;     // steps between allowed switch points

    struct Clip {
        uint8_t track, pattern;
        uint16_t startBar, lengthBars;
        uint16_t mask;          // channels this clip plays
    };
    Clip clips_[cfg::kMaxClips];
    int clipCount_;

    struct AudioClip {
        uint8_t track, slot;     // slot 0xff = none
        uint16_t startBar, lengthBars;
        uint8_t volume, flags;   // flags: bit0 loop, bits1..4 mixer route
        uint32_t trimStart, trimEnd;
    };
    AudioClip aclips_[cfg::kMaxAudioClips];
    int aclipCount_;
    bool justStarted_;          // the next song step begins a (re)start: join clips already under way
    int exportPasses_;          // > 0 in export mode
    bool metronome_ = false;

    // Notes that start part-way through a step wait here until their frame comes.
    struct PendingNote {
        uint32_t frame;      // absolute engine frame
        uint16_t gateTicks;
        uint8_t channel, velocity;
        int8_t semis;
    };
    static constexpr int kMaxPending = 64;
    PendingNote pending_[kMaxPending];
    int pendingCount_ = 0;
    bool exporting() const { return exportPasses_ > 0; }
    int songSteps_;         // length of the song in steps (0 = no clips)
    bool songMode_;

    EngineStatus status_;
};
