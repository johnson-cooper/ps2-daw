#include "audio/adpcm.hpp"

#include <string.h>

namespace adpcm {
namespace {

const int kF0[5] = {0, 60, 115, 98, 122};
const int kF1[5] = {0, 0, -52, -55, -60};

inline int clamp16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

// Quantises one block with a given filter/shift. Returns squared error and
// writes codes plus the resulting decoder history.
uint32_t tryBlock(const int16_t* x, int filter, int shift, int h1, int h2, int8_t* codes, int& outH1, int& outH2)
{
    uint32_t err = 0;
    for (int i = 0; i < kSamplesPerBlock; ++i) {
        const int predicted = (h1 * kF0[filter] + h2 * kF1[filter] + 32) >> 6;
        const int residual = x[i] - predicted;
        // q = round(residual * 2^shift / 4096)
        int scaled = residual * (1 << shift);
        int q = (scaled + (scaled >= 0 ? 2048 : -2048)) / 4096;
        if (q > 7)
            q = 7;
        if (q < -8)
            q = -8;
        const int decoded = clamp16(((q * 4096) >> shift) + predicted);
        const int e = x[i] - decoded;
        const uint32_t e2 = (uint32_t)(e * e);
        err = (err > 0xffffffffu - e2) ? 0xffffffffu : err + e2;
        codes[i] = (int8_t)q;
        h2 = h1;
        h1 = decoded;
    }
    outH1 = h1;
    outH2 = h2;
    return err;
}

} // namespace

void encodeMono(const int16_t* pcm, uint32_t frames, uint8_t* out)
{
    int h1 = 0, h2 = 0;
    int16_t block[kSamplesPerBlock];
    const uint32_t blocks = (frames + kSamplesPerBlock - 1) / kSamplesPerBlock;

    for (uint32_t b = 0; b < blocks; ++b) {
        const uint32_t base = b * kSamplesPerBlock;
        for (int i = 0; i < kSamplesPerBlock; ++i)
            block[i] = (base + (uint32_t)i < frames) ? pcm[base + i] : 0;

        uint32_t bestErr = 0xffffffffu;
        int bestFilter = 0, bestShift = 0, bestH1 = 0, bestH2 = 0;
        int8_t bestCodes[kSamplesPerBlock] = {};

        for (int f = 0; f < 5; ++f) {
            // Estimate the shift from the peak open-loop residual, then also
            // try one step finer/coarser and keep the lowest real error.
            int maxRes = 0, p1 = h1, p2 = h2;
            for (int i = 0; i < kSamplesPerBlock; ++i) {
                const int predicted = (p1 * kF0[f] + p2 * kF1[f] + 32) >> 6;
                int r = block[i] - predicted;
                if (r < 0)
                    r = -r;
                if (r > maxRes)
                    maxRes = r;
                p2 = p1;
                p1 = block[i];
            }
            int shift = 12;
            while (shift > 0 && (maxRes >> (12 - shift)) > 7)
                --shift;
            for (int s = shift - 1; s <= shift + 1; ++s) {
                if (s < 0 || s > 12)
                    continue;
                int8_t codes[kSamplesPerBlock];
                int nh1, nh2;
                const uint32_t e = tryBlock(block, f, s, h1, h2, codes, nh1, nh2);
                if (e < bestErr) {
                    bestErr = e;
                    bestFilter = f;
                    bestShift = s;
                    bestH1 = nh1;
                    bestH2 = nh2;
                    memcpy(bestCodes, codes, sizeof(codes));
                }
            }
        }

        uint8_t* o = out + b * kBlockBytes;
        o[0] = (uint8_t)((bestFilter << 4) | bestShift);
        o[1] = 0;
        for (int i = 0; i < kSamplesPerBlock; i += 2)
            o[2 + i / 2] = (uint8_t)((bestCodes[i] & 0x0f) | ((bestCodes[i + 1] & 0x0f) << 4));
        h1 = bestH1;
        h2 = bestH2;
    }
}

void decodeMono(const uint8_t* in, size_t bytes, int16_t* out)
{
    int h1 = 0, h2 = 0;
    for (size_t b = 0; b + kBlockBytes <= bytes; b += kBlockBytes) {
        const int filter = (in[b] >> 4) & 0x0f;
        const int shift = in[b] & 0x0f;
        const int f = filter > 4 ? 0 : filter;
        for (int i = 0; i < kSamplesPerBlock; ++i) {
            const uint8_t byte = in[b + 2 + i / 2];
            int nib = (i & 1) ? (byte >> 4) : (byte & 0x0f);
            if (nib & 8)
                nib -= 16;
            const int predicted = (h1 * kF0[f] + h2 * kF1[f] + 32) >> 6;
            const int s = clamp16(((nib * 4096) >> shift) + predicted);
            *out++ = (int16_t)s;
            h2 = h1;
            h1 = s;
        }
    }
}

} // namespace adpcm
