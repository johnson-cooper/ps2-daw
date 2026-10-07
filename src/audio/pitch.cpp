#include "audio/pitch.hpp"

#include <math.h>

namespace pitch {

uint32_t ratioQ16(int semis)
{
    static uint32_t table[2 * kMaxSemis + 1];
    static bool ready = false;
    if (!ready) {
        for (int i = -kMaxSemis; i <= kMaxSemis; ++i)
            table[i + kMaxSemis] = (uint32_t)(65536.0 * pow(2.0, i / 12.0) + 0.5);
        ready = true; // idempotent: a racing second init writes the same values
    }
    if (semis < -kMaxSemis)
        semis = -kMaxSemis;
    if (semis > kMaxSemis)
        semis = kMaxSemis;
    return table[semis + kMaxSemis];
}

} // namespace pitch
