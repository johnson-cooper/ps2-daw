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
constexpr int kMaxChannels = 16;   // rack rows available (a new project starts with kDefaultChannels)
constexpr int kDefaultChannels = 8;
constexpr int kMaxPatterns = 8; // <= 9: default names use one digit
constexpr int kMaxSteps = 64;
constexpr int kDefaultSteps = 16;
constexpr int kMaxSamples = 32;

// Playlist: tracks are rows, a bar is 16 steps. A clip plays one pattern
// (looped if the clip is longer than the pattern) for lengthBars bars.
// Piano roll: extra polyphonic notes per pattern and channel, on top of the step grid.
constexpr int kMaxNotes = 64;

constexpr int kPlaylistTracks = 8;
constexpr int kMaxSongBars = 128;
constexpr int kMaxClips = 64;

// Software voice pool shared by all channels.
constexpr int kMaxVoices = 24;
// A synth channel may hold at most this many voices, so one pad cannot starve the kit.
constexpr int kMaxSynthVoicesPerChannel = 8;

// Mixer: tracks 1..kMixTracks are inserts, track 0 is the master bus. A rack
// channel's `route` names its insert (0 = straight to the master).
constexpr int kMixTracks = 8;
constexpr int kMixBuses = kMixTracks + 1;
constexpr int kFxSlots = 4;      // effect chain length per track
constexpr int kFxParams = 6;     // parameters per effect
constexpr int kEnvParams = 6;    // AHDSR + enable
constexpr int kSynthParams = 24; // native synthesizer parameters

// Audio clips in the playlist and the sample references they use.
constexpr int kMaxAudioClips = 16;
constexpr int kMaxAudioSources = 8;

// Tempo range in hundredths of a BPM.
constexpr uint32_t kMinBpmCenti = 4000;   // 40.00
constexpr uint32_t kMaxBpmCenti = 30000;  // 300.00
constexpr uint32_t kDefaultBpmCenti = 12000;

constexpr int kDefaultVelocity = 100;

// Hardware SPU2 voices used for channels in SPU2 mode: rack channel n plays
// on SPU2 channel kSpuChannelBase + n.
constexpr int kSpuChannelBase = 0;

} // namespace cfg
