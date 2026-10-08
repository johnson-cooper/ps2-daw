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
#include "audio/fx.hpp"
#include "audio/instrument.hpp"

struct ChannelData {
    char name[12];
    char sampleRef[64];   // "builtin:KICK" or "samples:DIR/NAME.WAV"
    int8_t sampleSlot;    // runtime-resolved bank slot, -1 = none (not saved)
    uint8_t volume;       // 0..100
    int8_t pan;           // -100..100
    uint8_t mute;
    uint8_t solo;
    uint8_t voiceMode;    // VoiceMode
    uint8_t route;        // mixer insert target; 0 = master, 1..cfg::kMixTracks = insert
    uint8_t gate;         // 1 = piano-roll note length cuts the sample (sustained instruments)
    InstrumentData inst;  // sampler/synth kind, AHDSR envelope, synth patch
};

// A mixer insert: fader, pan, mute/solo and an effect chain. Index 0 of
// Project::tracks is the master bus (only its effect chain is used; the master
// fader is Project::masterVolume).
struct MixerTrackData {
    char name[10];
    uint8_t volume;       // 0..100
    int8_t pan;           // -100..100
    uint8_t mute;
    uint8_t solo;
    FxData fx[cfg::kFxSlots];
};

// An audio sample placed in the playlist. The sample is named by an entry of
// Project::audioRefs (a stable reference, like a channel's sampleRef), never by
// a runtime slot.
struct AudioClipData {
    uint8_t track;        // playlist track
    uint8_t source;       // index into Project::audioRefs
    uint16_t startBar;
    uint16_t lengthBars;
    uint8_t volume;       // 0..100
    uint8_t loop;         // 1 = repeat the trimmed region for the whole clip
    uint8_t route;        // mixer insert, 0 = master
    uint32_t trimStart;   // frames cut from the start of the sample
    uint32_t trimEnd;     // frame the region ends at (0 = to the end of the sample)
};

// A piano-roll note. The step grid stays the quick way to place root-pitch hits;
// notes add pitch, length and chords on top of it.
//
// Timing is in ticks: cfg::kTicksPerStep (24) per 16th step, so a note can start
// part-way through a step and be shorter than one step (chopped notes, rolls,
// flams). `step` + `tick` is the start; the duration is `lenTicks` when set, else
// `length` whole steps. Files and code that only know whole steps keep working:
// tick 0 and lenTicks 0 is exactly the old note.
struct PianoNote {
    uint8_t step;         // 0..kMaxSteps-1
    uint8_t pitch;        // MIDI note 0..127; 60 plays the sample at its native speed
    uint8_t velocity;     // 1..127
    uint8_t length;       // whole steps, 1..kMaxSteps (rounded up when lenTicks is set)
    uint8_t tick;         // 0..23 ticks after the start of `step`
    uint8_t reserved;
    uint16_t lenTicks;    // 0 = `length` whole steps, else the exact duration in ticks

    int startTicks() const { return step * cfg::kTicksPerStep + tick; }
    int durTicks() const { return lenTicks ? lenTicks : length * cfg::kTicksPerStep; }
};

struct PatternData {
    char name[12];
    uint8_t length;       // steps, 1..cfg::kMaxSteps
    uint8_t velocity[cfg::kMaxChannels][cfg::kMaxSteps]; // 0 = step off
    uint8_t noteCount[cfg::kMaxChannels];
    PianoNote notes[cfg::kMaxChannels][cfg::kMaxNotes];
};

// Reserved for Milestone 4: a playlist clip references a pattern by index.
constexpr uint16_t kAllChannels = 0xffff;

// `chanMask` says which rack channels of the pattern this clip plays (bit n = channel n).
// kAllChannels is the whole pattern; splitting a clip ("ungroup") moves instruments into
// clips of their own, on other tracks, that play the same pattern with a narrower mask.
struct PlaylistClip {
    uint8_t track;
    uint8_t pattern;
    uint16_t startBar;
    uint16_t lengthBars;
    uint16_t chanMask;
};

struct Project {
    static constexpr int kMaxClips = cfg::kMaxClips;

    char name[24];
    uint32_t bpmCenti;
    uint8_t masterVolume;
    uint8_t currentPattern;
    uint8_t channelCount;     // active rack rows, 1..cfg::kMaxChannels
    ChannelData channels[cfg::kMaxChannels];
    PatternData patterns[cfg::kMaxPatterns];
    uint16_t clipCount;
    PlaylistClip clips[kMaxClips];
    uint8_t songMode;         // 1 = play the playlist, 0 = loop the current pattern
    uint8_t trackMute;        // playlist track bits
    uint8_t trackSolo;
    uint8_t swing;            // 0..50: percent of a 16th step by which odd steps are delayed

    MixerTrackData tracks[cfg::kMixBuses];             // [0] = master chain, [1..8] = inserts
    char playlistTrackName[cfg::kPlaylistTracks][10];
    char audioRefs[cfg::kMaxAudioSources][64];         // sample references used by audio clips
    int8_t audioSlot[cfg::kMaxAudioSources];           // runtime-resolved bank slot, -1 = unresolved (not saved)
    uint8_t audioClipCount;
    AudioClipData audioClips[cfg::kMaxAudioClips];

    // Resets to an empty project with the built-in kit on channels 0..7.
    void resetEmpty();
    // Empty project plus a short demo beat in pattern 1 so that pressing
    // START on first boot is immediately audible.
    void resetDemo();

    // Drops invalid clips (bad track/pattern/length), clamps lengths to the
    // song limit, removes clips that overlap an earlier clip on the same
    // track, and orders the rest by start bar. Run after loading untrusted data.
    void sanitizeClips();
    // Index of the clip covering (track, bar), or -1.
    int clipAt(int track, int bar) const;
    // Bars until the last clip (pattern or audio) ends (0 for an empty playlist).
    int songBars() const;
    // Clamps/validates audio clips and effect data after loading untrusted data.
    void sanitizeAudio();
    // True when [startBar, startBar+len) on `track` is free of pattern clips and
    // audio clips (except the audio clip `ignoreAudio`, or pattern clip `ignorePattern`).
    bool trackFree(int track, int startBar, int len, int ignorePattern = -1, int ignoreAudio = -1) const;
    // Index of the audio clip covering (track, bar), or -1.
    int audioClipAt(int track, int bar) const;

    PatternData& pattern() { return patterns[currentPattern]; }
    const PatternData& pattern() const { return patterns[currentPattern]; }
};
