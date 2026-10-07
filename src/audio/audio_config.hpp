// Engine-wide constants. The internal format is fixed at 48 kHz stereo
// signed 16-bit: it is the SPU2's native output layout, so the audio library
// can pass our stream through without resampling (see docs/ARCHITECTURE.md).
#pragma once

#include <stdint.h>

namespace cfg {

constexpr int kSampleRate = 48000;
constexpr int kOutputChannels = 2;

// Largest block the engine renders in one call. The platform may ask for
// fewer frames; it must never ask for more.
constexpr int kMaxBlockFrames = 512;

// Musical clock. 96 PPQ divides evenly into 16ths (24 ticks) and triplets.
constexpr int kPpq = 96;
constexpr int kStepsPerBeat = 4;
constexpr int kTicksPerStep = kPpq / kStepsPerBeat;
constexpr int kBeatsPerBar = 4;

// Project limits. Sized for EE RAM, not for desktop expectations.
constexpr int kMaxChannels = 8;
constexpr int kMaxPatterns = 8; // <= 9: default names use one digit
constexpr int kMaxSteps = 64;
constexpr int kDefaultSteps = 16;
constexpr int kMaxSamples = 32;

// Playlist: tracks are rows, a bar is 16 steps. A clip plays one pattern
// (looped if the clip is longer than the pattern) for lengthBars bars.
constexpr int kPlaylistTracks = 6;
constexpr int kMaxSongBars = 128;
constexpr int kMaxClips = 64;

// Software voice pool shared by all channels.
constexpr int kMaxVoices = 24;

// Tempo range in hundredths of a BPM.
constexpr uint32_t kMinBpmCenti = 4000;   // 40.00
constexpr uint32_t kMaxBpmCenti = 30000;  // 300.00
constexpr uint32_t kDefaultBpmCenti = 12000;

constexpr int kDefaultVelocity = 100;

// Hardware SPU2 voices used for channels in SPU2 mode: rack channel n plays
// on SPU2 channel kSpuChannelBase + n.
constexpr int kSpuChannelBase = 0;

} // namespace cfg
