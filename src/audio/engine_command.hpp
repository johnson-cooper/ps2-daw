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
    SelectPattern,       // a = pattern (immediate; cancels a queued switch)
    QueuePattern,        // a = pattern, b = SwitchMode: switch on the next beat/bar boundary
    SetChannelSample,    // a = channel, value = sample slot (-1 = none)
    SetChannelVolume,    // a = channel, value = 0..100
    SetChannelPan,       // a = channel, value = -100..100
    SetChannelMute,      // a = channel, value = 0/1
    SetChannelSolo,      // a = channel, value = 0/1
    SetChannelVoiceMode, // a = channel, value = VoiceMode
    SetMasterVolume,     // value = 0..100
    PreviewChannel,      // a = channel, value = semitones from the root note
    PreviewSample,       // value = sample slot, b = VoiceMode
    SetSampleHwReady,    // value = sample slot, a = 0/1
    SetClip,             // a = index, b = track, c = pattern, value = startBar | (lengthBars << 16)
    SetClipCount,        // value = number of valid clips
    SetClipMask,         // a = index, value = channel bit mask (which instruments the clip plays)
    SetSongMode,         // value = 0 pattern loop, 1 play the playlist
    SetNote,             // a = pattern, b = channel, c = index, value = step | pitch << 8 | velocity << 16 | length << 24
    SetNoteCount,        // a = pattern, b = channel, value = number of valid notes
    SetNoteFine,         // a = pattern, b = channel, c = index, value = tick | lenTicks << 8 (sub-step timing)
    SetChannelGate,      // a = channel, value = 0 one-shot, 1 note length cuts the sample
    SetTrackMask,        // value = playlist track mute bits | solo bits << 8
    PlayFromBar,         // value = bar: start (or restart) the song from that bar
    ReleaseSample,       // value = sample slot: stop its voices, then acknowledge
    AllVoicesOff,
    // ---- mixer / instruments / effects (Milestones 5, 6) ----
    SetChannelRoute,     // a = channel, value = 0 master, 1..8 insert track
    SetChannelKind,      // a = channel, value = InstrKind
    SetEnvParam,         // a = channel, b = EnvParam, value
    SetSynthParam,       // a = channel, b = SynthParam, value
    SetTrackParam,       // a = track 1..8, b = 0 volume | 1 pan | 2 mute | 3 solo, value
    SetFxType,           // a = track 0..8 (0 = master), b = slot, value = FxType (loads defaults)
    SetFxParam,          // a = track, b = slot, c = parameter index, value
    SetFxBypass,         // a = track, b = slot, value = 0/1
    ClearClipLatch,      // reset the clip indicators
    // ---- playlist audio clips ----
    SetAudioClip,        // a = index, b = track, c = sample slot (0xff none), value = startBar | lengthBars << 16
    SetAudioClipMix,     // a = index, b = volume 0..127, c = flags (bit0 loop | route << 1)
    SetAudioClipTrim,    // a = index, b = 0 start / 1 end (frames; end 0 = whole sample), value = frames
    SetAudioClipCount,   // value = number of valid audio clips
    // ---- offline rendering ----
    SetSwing,            // value = delay of odd 16th steps in ticks (0..12)
    SetMetronome,        // value = 0/1
    SetExportMode,       // value = 0 off; n > 0: software voices only, stop after n passes of the song/pattern
};

// When a pattern change takes effect while the transport is playing.
enum class SwitchMode : uint8_t {
    Immediate = 0, // next step, mid-bar
    NextBeat = 1,  // next beat boundary (every 4 steps from the pattern start)
    NextBar = 2,   // next bar boundary (every 16 steps), or when the pattern loops
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
