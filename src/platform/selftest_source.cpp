#include "platform/selftest_source.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform/debug_mailbox.hpp"

DebugMailbox g_debugMailbox = {DebugMailbox::kMagic, 0, 0, 0, {0}};

namespace {

void put16(uint8_t*& p, uint16_t v)
{
    *p++ = (uint8_t)v;
    *p++ = (uint8_t)(v >> 8);
}
void put32(uint8_t*& p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p, (uint16_t)(v >> 16));
}

// A decaying sine "blip" as a canonical PCM WAV file image.
uint8_t* makeWav(uint32_t frames, uint32_t rate, uint16_t channels, double hz, uint32_t* outSize)
{
    const uint32_t dataBytes = frames * channels * 2;
    const uint32_t total = 44 + dataBytes;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf)
        return nullptr;
    uint8_t* p = buf;
    memcpy(p, "RIFF", 4); p += 4;
    put32(p, total - 8);
    memcpy(p, "WAVEfmt ", 8); p += 8;
    put32(p, 16);
    put16(p, 1);
    put16(p, channels);
    put32(p, rate);
    put32(p, rate * channels * 2);
    put16(p, (uint16_t)(channels * 2));
    put16(p, 16);
    memcpy(p, "data", 4); p += 4;
    put32(p, dataBytes);
    for (uint32_t i = 0; i < frames; ++i) {
        const double env = exp(-6.0 * i / frames);
        const int16_t v = (int16_t)(12000.0 * env * sin(6.283185307 * hz * i / rate));
        for (int c = 0; c < channels; ++c)
            put16(p, (uint16_t)v);
    }
    *outSize = total;
    return buf;
}

} // namespace

int SelfTestSource::openSample(const char* rel, uint32_t* size)
{
    if (strncmp(rel, kPrefix, strlen(kPrefix)) != 0)
        return real_.openSample(rel, size);
    const char* name = rel + strlen(kPrefix);
    for (int i = 0; i < kMaxHandles; ++i) {
        if (items_[i].data)
            continue;
        Item& it = items_[i];
        if (strcmp(name, "SINE.WAV") == 0) {
            it.data = makeWav(70000, 44100, 1, 440.0, &it.size); // 140 KB: several 32 KB chunks
        } else if (strcmp(name, "STEREO.WAV") == 0) {
            it.data = makeWav(30000, 48000, 2, 660.0, &it.size);
        } else if (strcmp(name, "GARBAGE.WAV") == 0) {
            it.size = 4096;
            it.data = (uint8_t*)malloc(it.size);
            if (it.data)
                memset(it.data, 0x5a, it.size);
        } else {
            return -1; // "missing file"
        }
        if (!it.data)
            return -1;
        it.pos = 0;
        *size = it.size;
        return kBase + i;
    }
    return -1;
}

int SelfTestSource::read(int handle, uint8_t* buf, uint32_t n)
{
    if (handle < kBase)
        return real_.read(handle, buf, n);
    Item& it = items_[handle - kBase];
    if (!it.data)
        return -1;
    const uint32_t left = it.size - it.pos;
    const uint32_t c = left < n ? left : n;
    memcpy(buf, it.data + it.pos, c);
    it.pos += c;
    return (int)c;
}

void SelfTestSource::close(int handle)
{
    if (handle < kBase) {
        real_.close(handle);
        return;
    }
    Item& it = items_[handle - kBase];
    free(it.data);
    it = Item();
}
