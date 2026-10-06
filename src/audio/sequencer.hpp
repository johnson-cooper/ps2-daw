// The audio thread's copy of the pattern grid.
//
// Pattern data is mirrored here (written only by the audio thread while
// applying commands) so that rendering reads a consistent grid. 8 patterns x
// 8 channels x 64 steps of velocity bytes = 4 KiB.
#pragma once

#include <stdint.h>
#include <string.h>

#include "audio/audio_config.hpp"

class Sequencer {
public:
    Sequencer()
    {
        memset(velocity_, 0, sizeof(velocity_));
        for (auto& l : length_)
            l = cfg::kDefaultSteps;
        current_ = 0;
    }

    void setStep(int pattern, int channel, int step, int velocity)
    {
        if (!valid(pattern, channel, step))
            return;
        if (velocity < 0)
            velocity = 0;
        if (velocity > 127)
            velocity = 127;
        velocity_[pattern][channel][step] = (uint8_t)velocity;
    }

    int velocity(int pattern, int channel, int step) const
    {
        return valid(pattern, channel, step) ? velocity_[pattern][channel][step] : 0;
    }

    void clearPattern(int pattern)
    {
        if (pattern >= 0 && pattern < cfg::kMaxPatterns)
            memset(velocity_[pattern], 0, sizeof(velocity_[pattern]));
    }

    void setLength(int pattern, int steps)
    {
        if (pattern < 0 || pattern >= cfg::kMaxPatterns)
            return;
        if (steps < 1)
            steps = 1;
        if (steps > cfg::kMaxSteps)
            steps = cfg::kMaxSteps;
        length_[pattern] = (uint8_t)steps;
    }

    int length(int pattern) const
    {
        return (pattern >= 0 && pattern < cfg::kMaxPatterns) ? length_[pattern] : cfg::kDefaultSteps;
    }

    void select(int pattern)
    {
        if (pattern >= 0 && pattern < cfg::kMaxPatterns)
            current_ = (uint8_t)pattern;
    }
    int current() const { return current_; }

private:
    static bool valid(int p, int c, int s)
    {
        return p >= 0 && p < cfg::kMaxPatterns && c >= 0 && c < cfg::kMaxChannels && s >= 0 && s < cfg::kMaxSteps;
    }

    uint8_t velocity_[cfg::kMaxPatterns][cfg::kMaxChannels][cfg::kMaxSteps];
    uint8_t length_[cfg::kMaxPatterns];
    uint8_t current_;
};
