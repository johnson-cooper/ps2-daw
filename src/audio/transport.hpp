// Sample-clocked musical transport.
//
// Musical time advances only when audio frames are rendered, so tempo is
// exactly as stable as the audio clock and completely independent of the UI
// frame rate. Position is kept as an exact rational number of ticks:
//
//   acc = ticks * D,   D = 6000 * sampleRate,   acc += bpmCenti * PPQ per frame
//
// (6000 = 60 s/min * 100 because tempo is stored in hundredths of a BPM.)
// Integer arithmetic means no drift, and a tempo change only changes the
// per-frame increment, so it takes effect without a position jump.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"

class Transport {
public:
    enum class State : uint8_t { Stopped, Playing, Paused };

    Transport();

    void setBpmCenti(uint32_t bpmCenti);
    uint32_t bpmCenti() const { return bpmCenti_; }

    void play();  // from Stopped: restart at step 0; from Paused: resume
    void pause();
    void stop();  // rewind to the start
    State state() const { return state_; }
    bool playing() const { return state_ == State::Playing; }

    // Frames until the next step boundary is due (0 = due now). Never returns
    // more than `limit`.
    uint32_t framesUntilNextStep(uint32_t limit) const;

    // Advances musical time by `frames` rendered frames (while playing).
    void advance(uint32_t frames);

    // Consumes the due step boundary and returns the pattern step index that
    // fires. Wraps to step 0 at `patternSteps` (also when the length shrank
    // underneath the playhead).
    int consumeStep(int patternSteps);

    // Frames rendered since play started from Stopped.
    uint32_t songFrames() const { return songFrames_; }
    uint32_t loopCount() const { return loops_; }

    static constexpr uint64_t kDenominator = 6000ull * (uint64_t)cfg::kSampleRate;

private:
    State state_;
    uint32_t bpmCenti_;
    uint32_t increment_;   // bpmCenti * PPQ, added per frame
    uint64_t acc_;         // ticks * kDenominator (+ fractional remainder)
    uint32_t nextStepTick_;
    uint32_t songFrames_;
    uint32_t loops_;
};
