// The editable song document, owned by the UI thread.
//
// This is the source of truth for editing and saving. The audio thread never
// reads it; Session mirrors every change into the engine as a Command.
// Plain fixed-size arrays only: no pointers, so it can be validated and
// serialised field by field (see project_io).
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"
#include "audio/engine_command.hpp"

struct ChannelData {
    char name[12];
    char sampleRef[48];   // "builtin:KICK" or a storage path (Milestone 2)
    int8_t sampleSlot;    // runtime-resolved bank slot, -1 = none (not saved)
    uint8_t volume;       // 0..100
    int8_t pan;           // -100..100
    uint8_t mute;
    uint8_t solo;
    uint8_t voiceMode;    // VoiceMode
    uint8_t route;        // mixer insert target; 0 = master (routing UI later)
};

struct PatternData {
    char name[12];
    uint8_t length;       // steps, 1..cfg::kMaxSteps
    uint8_t velocity[cfg::kMaxChannels][cfg::kMaxSteps]; // 0 = step off
};

// Reserved for Milestone 4: a playlist clip references a pattern by index.
struct PlaylistClip {
    uint8_t track;
    uint8_t pattern;
    uint16_t startBar;
    uint16_t lengthBars;
};

struct Project {
    static constexpr int kMaxClips = 64;

    char name[24];
    uint32_t bpmCenti;
    uint8_t masterVolume;
    uint8_t currentPattern;
    uint8_t channelCount;     // active rack rows, 1..cfg::kMaxChannels
    ChannelData channels[cfg::kMaxChannels];
    PatternData patterns[cfg::kMaxPatterns];
    uint16_t clipCount;
    PlaylistClip clips[kMaxClips];

    // Resets to an empty project with the built-in kit on channels 0..7.
    void resetEmpty();
    // Empty project plus a short demo beat in pattern 1 so that pressing
    // START on first boot is immediately audible.
    void resetDemo();

    PatternData& pattern() { return patterns[currentPattern]; }
    const PatternData& pattern() const { return patterns[currentPattern]; }
};
