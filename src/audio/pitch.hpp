// Semitone -> playback-rate ratio, shared by the software mixer and the SPU2
// path so both transpose identically.
#pragma once

#include <stdint.h>

namespace pitch {

constexpr int kRootNote = 60;   // MIDI note at which a sample plays at its native speed
constexpr int kMaxSemis = 60;   // +-5 octaves from the root

// 2^(semis/12) in 16.16 fixed point; semis is clamped to +-kMaxSemis.
uint32_t ratioQ16(int semis);

} // namespace pitch
