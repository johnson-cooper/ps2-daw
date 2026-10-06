#include "audio/sample.hpp"

#include <stdlib.h>

#include "core/strutil.hpp"

SampleBank::SampleBank() : count_(0), bytes_(0)
{
    for (auto& s : slots_) {
        s.name[0] = '\0';
        s.data = nullptr;
        s.frames = 0;
        s.sampleRate = 0;
        s.channels = 0;
        s.builtin = 0;
        s.ready = 0;
    }
}

SampleBank::~SampleBank()
{
    // Only reached at shutdown, after the audio thread has stopped.
    for (int i = 0; i < count_; ++i)
        free(const_cast<int16_t*>(slots_[i].data));
}

int SampleBank::add(const char* name, int16_t* data, uint32_t frames, uint32_t rate, uint8_t channels, bool builtin)
{
    if (count_ >= cfg::kMaxSamples || !data || frames == 0 || (channels != 1 && channels != 2))
        return -1;
    Sample& s = slots_[count_];
    str::copy(s.name, sizeof(s.name), name);
    s.data = data;
    s.frames = frames;
    s.sampleRate = rate;
    s.channels = channels;
    s.builtin = builtin ? 1 : 0;
    // Publish only after every field is written (see header comment).
    __atomic_store_n(&s.ready, (uint8_t)1, __ATOMIC_RELEASE);
    bytes_ += frames * channels * sizeof(int16_t);
    return count_++;
}

const Sample* SampleBank::get(int index) const
{
    if (index < 0 || index >= count_)
        return nullptr;
    const Sample& s = slots_[index];
    return __atomic_load_n(&s.ready, __ATOMIC_ACQUIRE) ? &s : nullptr;
}

int SampleBank::findByName(const char* name) const
{
    for (int i = 0; i < count_; ++i)
        if (str::equalsNoCase(slots_[i].name, name))
            return i;
    return -1;
}
