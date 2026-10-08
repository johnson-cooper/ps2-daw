#include "audio/params.hpp"

#include <math.h>
#include <stdio.h>

namespace params {

int cutoffHz(int idx)
{
    if (idx < 0)
        idx = 0;
    if (idx > 100)
        idx = 100;
    // 20 Hz * 900^(idx/100) = 20 Hz .. 18 kHz
    return (int)(20.0f * powf(900.0f, (float)idx / 100.0f) + 0.5f);
}

int lfoRateCentiHz(int idx)
{
    if (idx < 0)
        idx = 0;
    if (idx > 100)
        idx = 100;
    // 0.1 Hz * 200^(idx/100) = 0.1 .. 20 Hz
    return (int)(10.0f * powf(200.0f, (float)idx / 100.0f) + 0.5f);
}

void format(const ParamDesc& d, int v, char* out, size_t cap)
{
    switch (d.fmt) {
    case Fmt::Int:
        snprintf(out, cap, "%d", v);
        break;
    case Fmt::Percent:
        snprintf(out, cap, "%d%%", v);
        break;
    case Fmt::Db:
        if (v <= d.min && d.min <= -60)
            snprintf(out, cap, "-inf");
        else
            snprintf(out, cap, "%+d dB", v);
        break;
    case Fmt::Ms:
        if (v >= 1000)
            snprintf(out, cap, "%d.%02d s", v / 1000, (v % 1000) / 10);
        else
            snprintf(out, cap, "%d ms", v);
        break;
    case Fmt::Cutoff: {
        const int hz = cutoffHz(v);
        if (hz >= 1000)
            snprintf(out, cap, "%d.%d kHz", hz / 1000, (hz % 1000) / 100);
        else
            snprintf(out, cap, "%d Hz", hz);
        break;
    }
    case Fmt::Cents:
        snprintf(out, cap, "%+d ct", v);
        break;
    case Fmt::Semis:
        snprintf(out, cap, "%+d st", v);
        break;
    case Fmt::Enum:
        if (d.names && v >= d.min && v <= d.max)
            snprintf(out, cap, "%s", d.names[v - d.min]);
        else
            snprintf(out, cap, "%d", v);
        break;
    case Fmt::OnOff:
        snprintf(out, cap, "%s", v ? "ON" : "OFF");
        break;
    case Fmt::Ratio:
        snprintf(out, cap, "%d:1", v);
        break;
    case Fmt::LfoRate: {
        const int c = lfoRateCentiHz(v);
        snprintf(out, cap, "%d.%02d Hz", c / 100, c % 100);
        break;
    }
    case Fmt::Pan:
        if (v == 0)
            snprintf(out, cap, "C");
        else
            snprintf(out, cap, "%c%d", v < 0 ? 'L' : 'R', v < 0 ? -v : v);
        break;
    }
}

} // namespace params
