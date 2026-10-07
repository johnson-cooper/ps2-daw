#include "audio/sample.hpp"

#include <stdlib.h>
#include <string.h>

#include "core/strutil.hpp"

namespace {
uint8_t loadState(const Sample& s) { return __atomic_load_n(&s.state, __ATOMIC_ACQUIRE); }
void storeState(Sample& s, SlotState v) { __atomic_store_n(&s.state, (uint8_t)v, __ATOMIC_RELEASE); }
} // namespace

SampleBank::SampleBank() : highWater_(0), totalBytes_(0), externalBytes_(0), nextGeneration_(1), failure_(Failure::None)
{
    memset(slots_, 0, sizeof(slots_));
}

SampleBank::~SampleBank()
{
    // Only reached at shutdown, after the audio thread has stopped.
    for (auto& s : slots_)
        free(const_cast<int16_t*>(s.data));
}

bool SampleBank::hasFreeSlot() const
{
    for (const auto& s : slots_)
        if (loadState(s) == (uint8_t)SlotState::Empty)
            return true;
    return false;
}

static void fillCommon(Sample& s, const char* name, const char* ref, uint16_t gen)
{
    memset(&s, 0, sizeof(s));
    str::copy(s.name, sizeof(s.name), name);
    if (ref && *ref) {
        str::copy(s.ref, sizeof(s.ref), ref);
    } else {
        str::copy(s.ref, sizeof(s.ref), "builtin:");
        str::append(s.ref, sizeof(s.ref), name);
    }
    s.generation = gen;
}

int SampleBank::add(const char* name, int16_t* data, uint32_t frames, uint32_t rate, uint8_t channels, bool builtin,
                    const char* ref, uint32_t fileBytes)
{
    failure_ = Failure::None;
    if (!data || frames == 0 || rate == 0 || (channels != 1 && channels != 2)) {
        failure_ = Failure::Invalid;
        return -1;
    }
    const uint64_t bytes64 = (uint64_t)frames * channels * sizeof(int16_t);
    if (bytes64 > 0x7fffffffu) {
        failure_ = Failure::Invalid;
        return -1;
    }
    const uint32_t bytes = (uint32_t)bytes64;
    if (!builtin && !canFit(bytes)) {
        failure_ = Failure::BudgetFull;
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < cfg::kMaxSamples; ++i)
        if (loadState(slots_[i]) == (uint8_t)SlotState::Empty) {
            idx = i;
            break;
        }
    if (idx < 0) {
        failure_ = Failure::SlotsFull;
        return -1;
    }
    Sample& s = slots_[idx];
    fillCommon(s, name, ref, nextGeneration_++);
    s.data = data;
    s.frames = frames;
    s.sampleRate = rate;
    s.pcmBytes = bytes;
    s.fileBytes = fileBytes;
    s.channels = channels;
    s.builtin = builtin ? 1 : 0;
    totalBytes_ += bytes;
    if (!builtin)
        externalBytes_ += bytes;
    if (idx >= highWater_)
        highWater_ = idx + 1;
    // Publish only after every field is written.
    storeState(s, SlotState::Ready);
    return idx;
}

int SampleBank::addHwOnly(const char* name, const char* ref, uint32_t fileBytes)
{
    failure_ = Failure::None;
    int idx = -1;
    for (int i = 0; i < cfg::kMaxSamples; ++i)
        if (loadState(slots_[i]) == (uint8_t)SlotState::Empty) {
            idx = i;
            break;
        }
    if (idx < 0) {
        failure_ = Failure::SlotsFull;
        return -1;
    }
    Sample& s = slots_[idx];
    fillCommon(s, name, ref, nextGeneration_++);
    s.fileBytes = fileBytes;
    s.hwOnly = 1;
    if (idx >= highWater_)
        highWater_ = idx + 1;
    storeState(s, SlotState::Ready);
    return idx;
}

const Sample* SampleBank::get(int index) const
{
    if (index < 0 || index >= cfg::kMaxSamples)
        return nullptr;
    const Sample& s = slots_[index];
    return loadState(s) == (uint8_t)SlotState::Ready ? &s : nullptr;
}

const Sample* SampleBank::peek(int index) const
{
    if (index < 0 || index >= cfg::kMaxSamples)
        return nullptr;
    return &slots_[index];
}

int SampleBank::findByName(const char* name) const
{
    for (int i = 0; i < highWater_; ++i)
        if (get(i) && str::equalsNoCase(slots_[i].name, name))
            return i;
    return -1;
}

int SampleBank::findByRef(const char* ref) const
{
    if (!ref || !*ref)
        return -1;
    for (int i = 0; i < highWater_; ++i)
        if (get(i) && str::equalsNoCase(slots_[i].ref, ref))
            return i;
    return -1;
}

int SampleBank::liveCount() const
{
    int n = 0;
    for (int i = 0; i < highWater_; ++i)
        if (get(i))
            ++n;
    return n;
}

bool SampleBank::requestRelease(int index)
{
    if (index < 0 || index >= cfg::kMaxSamples)
        return false;
    Sample& s = slots_[index];
    if (loadState(s) != (uint8_t)SlotState::Ready || s.builtin)
        return false;
    storeState(s, SlotState::Releasing);
    return true;
}

void SampleBank::acknowledgeRelease(int index) const
{
    if (index < 0 || index >= cfg::kMaxSamples)
        return;
    const Sample& s = slots_[index];
    if (loadState(s) == (uint8_t)SlotState::Releasing)
        __atomic_store_n(const_cast<volatile uint8_t*>(&s.state), (uint8_t)SlotState::Acked, __ATOMIC_RELEASE);
}

bool SampleBank::isReleasing(int index) const
{
    return index >= 0 && index < cfg::kMaxSamples && loadState(slots_[index]) == (uint8_t)SlotState::Releasing;
}

void SampleBank::freeSlot(int index)
{
    Sample& s = slots_[index];
    totalBytes_ -= s.pcmBytes;
    if (!s.builtin)
        externalBytes_ -= s.pcmBytes;
    free(const_cast<int16_t*>(s.data));
    const uint16_t gen = s.generation;
    memset(&s, 0, sizeof(s));
    s.generation = gen; // slot reuse assigns a fresh generation in add()
    while (highWater_ > 0 && loadState(slots_[highWater_ - 1]) == (uint8_t)SlotState::Empty)
        --highWater_;
}

int SampleBank::reap()
{
    int n = 0;
    for (int i = 0; i < cfg::kMaxSamples; ++i)
        if (loadState(slots_[i]) == (uint8_t)SlotState::Acked) {
            freeSlot(i);
            ++n;
        }
    return n;
}

void SampleBank::discardNew(int index)
{
    if (index >= 0 && index < cfg::kMaxSamples && loadState(slots_[index]) == (uint8_t)SlotState::Ready && !slots_[index].builtin)
        freeSlot(index);
}
