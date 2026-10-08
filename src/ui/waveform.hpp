// Waveform summaries for the playlist's audio clips.
//
// Each bank sample gets 128 peak columns (128 bytes), built a little at a time
// on the UI thread so a 3 MiB sample never costs more than ~32 K frames of
// scanning per UI frame. Drawing a clip then only reads the summary. The cache
// notices a reused slot through the sample generation counter.
#pragma once

#include <stdint.h>

#include "audio/sample.hpp"

class WaveformCache {
public:
    static constexpr int kColumns = 128;
    static constexpr uint32_t kBudgetFrames = 32768;

    WaveformCache();

    // Call once per UI frame: scans a bounded amount of PCM.
    void tick(const SampleBank& bank);
    // Peaks (0..255, abs amplitude / 128) once the summary is complete, else null.
    const uint8_t* peaks(int slot) const;
    bool ready(int slot) const { return peaks(slot) != nullptr; }

private:
    struct Entry {
        uint8_t peaks[kColumns];
        uint16_t generation;
        uint16_t nextColumn;  // columns finished so far
        uint8_t state;        // 0 empty, 1 building, 2 ready
    };
    Entry entries_[cfg::kMaxSamples];
    int cursor_;
};
