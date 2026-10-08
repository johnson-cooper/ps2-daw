#include "ui/waveform.hpp"

#include <string.h>

WaveformCache::WaveformCache() : cursor_(0)
{
    memset(entries_, 0, sizeof(entries_));
}

const uint8_t* WaveformCache::peaks(int slot) const
{
    if (slot < 0 || slot >= cfg::kMaxSamples || entries_[slot].state != 2)
        return nullptr;
    return entries_[slot].peaks;
}

void WaveformCache::tick(const SampleBank& bank)
{
    uint32_t budget = kBudgetFrames;
    for (int n = 0; n < cfg::kMaxSamples && budget > 0; ++n) {
        const int slot = cursor_;
        Entry& e = entries_[slot];
        const Sample* s = bank.get(slot);
        if (!s) {
            e.state = 0; // slot freed: forget it
            cursor_ = (cursor_ + 1) % cfg::kMaxSamples;
            continue;
        }
        if (e.state != 0 && e.generation != s->generation)
            e.state = 0; // slot reused for another sample
        if (e.state == 2) {
            cursor_ = (cursor_ + 1) % cfg::kMaxSamples;
            continue;
        }
        if (e.state == 0) {
            memset(e.peaks, 0, sizeof(e.peaks));
            e.generation = s->generation;
            e.nextColumn = 0;
            e.state = 1;
        }
        if (!s->data || s->frames == 0) { // SPU2-only: nothing to show
            e.state = 2;
            cursor_ = (cursor_ + 1) % cfg::kMaxSamples;
            continue;
        }
        const int ch = s->channels == 2 ? 2 : 1;
        while (e.nextColumn < kColumns && budget > 0) {
            const uint32_t a = (uint32_t)((uint64_t)s->frames * e.nextColumn / kColumns);
            uint32_t b = (uint32_t)((uint64_t)s->frames * (e.nextColumn + 1) / kColumns);
            if (b <= a)
                b = a + 1;
            if (b > s->frames)
                b = s->frames;
            int peak = 0;
            for (uint32_t f = a; f < b; ++f)
                for (int c = 0; c < ch; ++c) {
                    const int v = s->data[f * ch + c];
                    const int m = v < 0 ? -v : v;
                    if (m > peak)
                        peak = m;
                }
            const uint32_t scanned = b - a;
            budget = scanned >= budget ? 0 : budget - scanned;
            const int p = peak >> 7;
            e.peaks[e.nextColumn++] = (uint8_t)(p > 255 ? 255 : p);
        }
        if (e.nextColumn >= kColumns)
            e.state = 2;
        // keep working on this slot next frame if the budget ran out inside it
        if (e.state == 2)
            cursor_ = (cursor_ + 1) % cfg::kMaxSamples;
        else
            break;
    }
}
