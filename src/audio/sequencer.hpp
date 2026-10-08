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
    struct Note {
        uint8_t step, pitch, velocity, length;
        uint8_t tick;
        uint8_t reserved;
        uint16_t lenTicks; // 0 = `length` whole steps
        int durTicks() const { return lenTicks ? lenTicks : length * cfg::kTicksPerStep; }
    };

    Sequencer()
    {
        memset(velocity_, 0, sizeof(velocity_));
        memset(notes_, 0, sizeof(notes_));
        memset(noteCount_, 0, sizeof(noteCount_));
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
        if (pattern >= 0 && pattern < cfg::kMaxPatterns) {
            memset(velocity_[pattern], 0, sizeof(velocity_[pattern]));
            memset(noteCount_[pattern], 0, sizeof(noteCount_[pattern]));
        }
    }

    void setNote(int pattern, int channel, int index, const Note& n)
    {
        if (pattern >= 0 && pattern < cfg::kMaxPatterns && channel >= 0 && channel < cfg::kMaxChannels && index >= 0 &&
            index < cfg::kMaxNotes)
            notes_[pattern][channel][index] = n;
    }
    void setNoteCount(int pattern, int channel, int count)
    {
        if (pattern >= 0 && pattern < cfg::kMaxPatterns && channel >= 0 && channel < cfg::kMaxChannels)
            noteCount_[pattern][channel] = (uint8_t)(count < 0 ? 0 : (count > cfg::kMaxNotes ? cfg::kMaxNotes : count));
    }
    int noteCount(int pattern, int channel) const
    {
        return (pattern >= 0 && pattern < cfg::kMaxPatterns && channel >= 0 && channel < cfg::kMaxChannels) ? noteCount_[pattern][channel] : 0;
    }
    const Note& note(int pattern, int channel, int index) const { return notes_[pattern][channel][index]; }

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
    Note notes_[cfg::kMaxPatterns][cfg::kMaxChannels][cfg::kMaxNotes];
    uint8_t noteCount_[cfg::kMaxPatterns][cfg::kMaxChannels];
    uint8_t length_[cfg::kMaxPatterns];
    uint8_t current_;
};
