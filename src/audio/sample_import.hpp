// Turns a WAV file image into a SampleBank slot, with hard size limits.
// Never trusts file contents; every failure returns a readable reason.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/sample.hpp"

namespace sampleimport {

constexpr uint32_t kMaxFileBytes = 3u * 1024u * 1024u;  // WAV / ADP file on disk
constexpr uint32_t kMaxSampleBytes = 3u * 1024u * 1024u; // converted int16 PCM
constexpr uint32_t kMinFileBytes = 44;

enum class Result : uint8_t {
    Ok,
    BadFormat,   // not a usable WAV
    TooLarge,    // exceeds kMax*Bytes
    BudgetFull,  // SampleBank external PCM budget exhausted
    SlotsFull,   // no free bank slot
    NoMemory,
};

// Parses and converts `file` into the bank under `ref`/`name`. On Ok, *slot
// is the new slot. `err` always receives a short message on failure.
Result importWav(SampleBank& bank, const char* name, const char* ref, const uint8_t* file, size_t size, int* slot,
                 char* err, size_t errCap);

// Name shown in the UI for a path: the file name without directories,
// truncated to fit Sample::name.
void displayName(char* out, size_t cap, const char* rel);

} // namespace sampleimport
