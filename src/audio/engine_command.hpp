// Messages from the UI thread to the audio thread.
//
// The audio thread owns its own copy of everything it plays (pattern grid,
// channel strip settings, transport). The UI edits the Project and mirrors
// each edit as one of these small fixed-size commands, so the realtime path
// never takes a lock and never sees a half-edited structure.
#pragma once

#include <stdint.h>

enum class CmdType : uint8_t {
    Play,
    Pause,
    Stop,
    SetBpm,              // value = bpm * 100
    SetStep,             // a = pattern, b = channel, c = step, value = velocity (0 = off)
    ClearPattern,        // a = pattern
    SetPatternLength,    // a = pattern, value = steps
    SelectPattern,       // a = pattern
    SetChannelSample,    // a = channel, value = sample slot (-1 = none)
    SetChannelVolume,    // a = channel, value = 0..100
    SetChannelPan,       // a = channel, value = -100..100
    SetChannelMute,      // a = channel, value = 0/1
    SetChannelSolo,      // a = channel, value = 0/1
    SetChannelVoiceMode, // a = channel, value = VoiceMode
    SetMasterVolume,     // value = 0..100
    PreviewChannel,      // a = channel
    PreviewSample,       // value = sample slot, b = VoiceMode
    SetSampleHwReady,    // value = sample slot, a = 0/1
    AllVoicesOff,
};

// How a channel's sound is produced.
enum class VoiceMode : uint8_t {
    Software = 0, // EE software mixer -> PCM stream (sample-accurate, metered)
    Spu2 = 1,     // SPU2 hardware voice (offloads the EE; ~block-accurate)
};

struct Command {
    CmdType type;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    int32_t value;
};
