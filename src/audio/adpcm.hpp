// PS-ADPCM (SPU2 "VAG" block format) encoder.
//
// Each 16-byte block holds 28 samples: byte 0 = (filter << 4) | shift,
// byte 1 = loop flags (left 0 here; the audio library rewrites them from
// audio_sound_desc_t::loops), then 14 bytes of 4-bit codes, low nibble first.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace adpcm {

constexpr int kSamplesPerBlock = 28;
constexpr int kBlockBytes = 16;

inline size_t encodedSize(uint32_t frames)
{
    return (size_t)((frames + kSamplesPerBlock - 1) / kSamplesPerBlock) * kBlockBytes;
}

// Encodes mono int16 PCM into `out` (encodedSize(frames) bytes).
void encodeMono(const int16_t* pcm, uint32_t frames, uint8_t* out);

// Reference decoder (used by host tests to verify encoder quality).
void decodeMono(const uint8_t* in, size_t bytes, int16_t* out);

} // namespace adpcm
