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
};

// One fired step, for the UI playhead. The UI shows the latest mark whose
// frame has actually been heard, so the cursor matches the sound rather than
// running ahead by the output latency.
struct StepMark {
    uint32_t frame;
    uint8_t step;
    uint8_t pattern;
};

struct EngineStatus {
    static constexpr int kMarks = 16;

    volatile uint32_t renderedFrames = 0;  // absolute engine clock (wraps after ~24 h)
    volatile uint8_t transport = 0;        // Transport::State
    volatile uint8_t pattern = 0;
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
};

class AudioEngine {
public:
    AudioEngine();

    void setSampleBank(const SampleBank* bank) { bank_ = bank; }

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
    void triggerChannel(int ch, int velocity, uint32_t frameInBlock);
    bool triggerSample(int ch, int sampleSlot, int velocity, VoiceMode mode, uint32_t frameInBlock);
    void publish();

    SpscQueue<Command, 1024> queue_;
    Transport transport_;
    Sequencer sequencer_;
    Mixer mixer_;
    const SampleBank* bank_;

    int8_t channelSample_[cfg::kMaxChannels];
    VoiceMode channelMode_[cfg::kMaxChannels];
    uint8_t sampleHwReady_[cfg::kMaxSamples];

    HwTrigger hw_[kMaxHwPerBlock];
    int hwCount_;
    uint32_t blockStart_;

    EngineStatus status_;
};
