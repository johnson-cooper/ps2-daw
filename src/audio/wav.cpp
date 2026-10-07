#include "audio/wav.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace wav {
namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool fail(char* err, size_t cap, const char* msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
    return false;
}

} // namespace

bool parse(const uint8_t* data, size_t size, Info& out, char* err, size_t errCap)
{
    memset(&out, 0, sizeof(out));
    if (!data || size < 12)
        return fail(err, errCap, "file too small");
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return fail(err, errCap, "not a RIFF/WAVE file");

    // Never trust the RIFF size; bound everything by the real buffer.
    size_t end = size;
    const uint32_t riffSize = rd32(data + 4);
    if ((size_t)riffSize + 8 < end)
        end = (size_t)riffSize + 8;

    bool haveFmt = false;
    uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    uint32_t pcmBytes = 0;

    size_t pos = 12;
    while (pos + 8 <= end) {
        const uint8_t* ck = data + pos;
        const uint32_t ckSize = rd32(ck + 4);
        const size_t bodyStart = pos + 8;
        const size_t avail = end - bodyStart;
        const size_t body = ckSize > avail ? avail : ckSize;

        if (memcmp(ck, "fmt ", 4) == 0) {
            if (body < 16)
                return fail(err, errCap, "fmt chunk too short");
            const uint8_t* f = data + bodyStart;
            format = rd16(f);
            channels = rd16(f + 2);
            rate = rd32(f + 4);
            blockAlign = rd16(f + 12);
            bits = rd16(f + 14);
            if (format == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE
                if (body < 40)
                    return fail(err, errCap, "extensible fmt too short");
                format = rd16(f + 24); // first two bytes of the subformat GUID
            }
            haveFmt = true;
        } else if (memcmp(ck, "data", 4) == 0) {
            pcm = data + bodyStart;
            pcmBytes = (uint32_t)body; // truncated files keep what is present
        }

        // Chunks are word aligned; guard against size overflow near the end.
        const size_t advance = 8 + (size_t)ckSize + (ckSize & 1u);
        if (advance > end - pos)
            break;
        pos += advance;
    }

    if (!haveFmt)
        return fail(err, errCap, "missing fmt chunk");
    if (format != 1 && format != 3) {
        static char msg[40];
        snprintf(msg, sizeof(msg), "unsupported WAV encoding (tag %u)", (unsigned)format);
        return fail(err, errCap, msg);
    }
    if (channels != 1 && channels != 2)
        return fail(err, errCap, "only mono/stereo supported");
    if (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32)
        return fail(err, errCap, "PCM must be 8/16/24/32-bit");
    if (format == 3 && bits != 32 && bits != 64)
        return fail(err, errCap, "float WAV must be 32/64-bit");
    if (rate < 4000 || rate > 96000)
        return fail(err, errCap, "sample rate out of range");
    const uint16_t expectAlign = (uint16_t)(channels * (bits / 8));
    if (blockAlign != expectAlign)
        return fail(err, errCap, "inconsistent block align");
    if (!pcm)
        return fail(err, errCap, "missing data chunk");

    const uint32_t frames = pcmBytes / expectAlign;
    if (frames == 0)
        return fail(err, errCap, "no audio frames");

    out.sampleRate = rate;
    out.channels = channels;
    out.bitsPerSample = bits;
    out.isFloat = format == 3;
    out.frames = frames;
    out.pcm = pcm;
    out.pcmBytes = frames * expectAlign;
    return true;
}

static int16_t clip16(float v)
{
    if (!(v == v)) // NaN
        return 0;
    v *= 32768.0f;
    if (v >= 32767.0f)
        return 32767;
    if (v <= -32768.0f)
        return -32768;
    return (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

int16_t* toInt16(const Info& info)
{
    const size_t count = (size_t)info.frames * info.channels;
    int16_t* out = (int16_t*)malloc(count * sizeof(int16_t));
    if (!out)
        return nullptr;
    const uint8_t* p = info.pcm;
    // Byte-wise reads throughout: the data chunk may start at an odd address.
    if (info.isFloat && info.bitsPerSample == 32) {
        for (size_t i = 0; i < count; ++i) {
            const uint32_t u = rd32(p + i * 4);
            float f;
            memcpy(&f, &u, 4);
            out[i] = clip16(f);
        }
    } else if (info.isFloat) { // 64-bit
        for (size_t i = 0; i < count; ++i) {
            const uint64_t u = (uint64_t)rd32(p + i * 8) | ((uint64_t)rd32(p + i * 8 + 4) << 32);
            double d;
            memcpy(&d, &u, 8);
            out[i] = clip16((float)d);
        }
    } else if (info.bitsPerSample == 16) {
        for (size_t i = 0; i < count; ++i)
            out[i] = (int16_t)rd16(p + i * 2);
    } else if (info.bitsPerSample == 24) {
        for (size_t i = 0; i < count; ++i) {
            // Top 16 bits, rounded (saturating at the positive end).
            int32_t v = (int32_t)(((uint32_t)p[i * 3] << 8) | ((uint32_t)p[i * 3 + 1] << 16) | ((uint32_t)p[i * 3 + 2] << 24)) >> 8;
            v = (v + 128) >> 8;
            out[i] = (int16_t)(v > 32767 ? 32767 : v);
        }
    } else if (info.bitsPerSample == 32) {
        for (size_t i = 0; i < count; ++i) {
            int32_t v = (int32_t)rd32(p + i * 4);
            v = (int32_t)(((int64_t)v + 32768) >> 16);
            out[i] = (int16_t)(v > 32767 ? 32767 : v);
        }
    } else { // 8-bit unsigned
        for (size_t i = 0; i < count; ++i)
            out[i] = (int16_t)(((int)p[i] - 128) << 8);
    }
    return out;
}

} // namespace wav
