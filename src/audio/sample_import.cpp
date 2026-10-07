#include "audio/sample_import.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/sample_ref.hpp"
#include "audio/wav.hpp"
#include "core/strutil.hpp"

namespace sampleimport {

static void setErr(char* err, size_t cap, const char* msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
}

void displayName(char* out, size_t cap, const char* rel)
{
    str::copy(out, cap, sampleref::baseName(rel));
}

Result importWav(SampleBank& bank, const char* name, const char* ref, const uint8_t* file, size_t size, int* slot,
                 char* err, size_t errCap)
{
    if (slot)
        *slot = -1;
    if (size > kMaxFileBytes) {
        setErr(err, errCap, "file too large (max 3 MiB)");
        return Result::TooLarge;
    }
    wav::Info info;
    char why[40];
    if (!wav::parse(file, size, info, why, sizeof(why))) {
        setErr(err, errCap, why);
        return Result::BadFormat;
    }
    // Size of the converted 16-bit PCM, computed in 64 bits before any allocation.
    const uint64_t pcm = (uint64_t)info.frames * info.channels * sizeof(int16_t);
    if (pcm > kMaxSampleBytes) {
        setErr(err, errCap, "sample too long (max 3 MiB PCM)");
        return Result::TooLarge;
    }
    if (!bank.hasFreeSlot()) {
        setErr(err, errCap, "no free sample slots");
        return Result::SlotsFull;
    }
    if (!bank.canFit((uint32_t)pcm)) {
        setErr(err, errCap, "out of sample memory");
        return Result::BudgetFull;
    }
    int16_t* data = wav::toInt16(info);
    if (!data) {
        setErr(err, errCap, "out of memory");
        return Result::NoMemory;
    }
    const int idx = bank.add(name, data, info.frames, info.sampleRate, (uint8_t)info.channels, false, ref, (uint32_t)size);
    if (idx < 0) {
        free(data);
        const bool budget = bank.lastFailure() == SampleBank::Failure::BudgetFull;
        setErr(err, errCap, budget ? "out of sample memory" : "no free sample slots");
        return budget ? Result::BudgetFull : Result::SlotsFull;
    }
    if (slot)
        *slot = idx;
    return Result::Ok;
}

} // namespace sampleimport
