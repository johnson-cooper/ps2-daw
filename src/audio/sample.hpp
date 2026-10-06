// In-memory sample storage.
//
// Samples are immutable once published to the bank: the audio thread reads
// `data` without locking, so a slot is filled completely and only then marked
// ready. Slots are never freed while the program runs (Milestone 1 policy);
// see docs/ARCHITECTURE.md for the planned deferred-release scheme.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"

struct Sample {
    char name[16];
    const int16_t* data;  // interleaved when channels == 2
    uint32_t frames;
    uint32_t sampleRate;
    uint8_t channels;     // 1 or 2
    uint8_t builtin;      // generated at boot, not loaded from storage
    volatile uint8_t ready;
};

class SampleBank {
public:
    SampleBank();
    ~SampleBank();
    SampleBank(const SampleBank&) = delete;
    SampleBank& operator=(const SampleBank&) = delete;

    // Takes ownership of `data` (allocated with malloc/memalign). Returns the
    // slot index, or -1 when the bank is full.
    int add(const char* name, int16_t* data, uint32_t frames, uint32_t rate, uint8_t channels, bool builtin);

    const Sample* get(int index) const;
    int findByName(const char* name) const;
    int count() const { return count_; }
    uint32_t bytesUsed() const { return bytes_; }

private:
    Sample slots_[cfg::kMaxSamples];
    int count_;
    uint32_t bytes_;
};
