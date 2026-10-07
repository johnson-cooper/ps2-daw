// Defensive RIFF/WAVE parser for user-supplied files.
//
// Supported: integer PCM (format 1) at 8-bit unsigned or 16/24/32-bit signed,
// and IEEE float (format 3) at 32 or 64 bits, either directly or inside
// WAVE_FORMAT_EXTENSIBLE; mono or stereo, 4000..96000 Hz. Everything is
// converted to signed 16-bit by toInt16().
// Anything else is rejected with a readable reason; malformed lengths are
// never trusted beyond the bytes actually present.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wav {

struct Info {
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint8_t isFloat;      // IEEE float samples
    uint32_t frames;
    const uint8_t* pcm;   // points into the caller's buffer
    uint32_t pcmBytes;
};

// Parses `size` bytes. On failure returns false and writes a short reason.
bool parse(const uint8_t* data, size_t size, Info& out, char* err, size_t errCap);

// Converts parsed PCM to newly malloc'd interleaved int16 (caller frees).
// Returns nullptr on allocation failure.
int16_t* toInt16(const Info& info);

} // namespace wav
