#include "project/project.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/drum_synth.hpp"
#include "core/strutil.hpp"

void Project::resetEmpty()
{
    memset(this, 0, sizeof(*this));
    str::copy(name, sizeof(name), "UNTITLED");
    bpmCenti = cfg::kDefaultBpmCenti;
    masterVolume = 80;
    currentPattern = 0;
    channelCount = cfg::kMaxChannels;

    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        ChannelData& c = channels[ch];
        const char* kitName = drumsynth::name((drumsynth::Kind)ch);
        str::copy(c.name, sizeof(c.name), kitName);
        snprintf(c.sampleRef, sizeof(c.sampleRef), "builtin:%s", kitName);
        c.sampleSlot = (int8_t)ch; // built-ins occupy slots 0..7 in Kind order
        c.volume = 78;
        c.pan = 0;
        c.voiceMode = (uint8_t)VoiceMode::Software;
    }
    for (int p = 0; p < cfg::kMaxPatterns; ++p) {
        str::copy(patterns[p].name, sizeof(patterns[p].name), "PATTERN ?");
        patterns[p].name[8] = (char)('1' + p); // kMaxPatterns <= 9
        patterns[p].length = cfg::kDefaultSteps;
    }
}

void Project::resetDemo()
{
    resetEmpty();
    str::copy(name, sizeof(name), "DEMO BEAT");
    PatternData& p = patterns[0];
    const int v = cfg::kDefaultVelocity;
    for (int s = 0; s < 16; ++s) {
        if (s % 4 == 0)
            p.velocity[0][s] = v;          // kick, four on the floor
        if (s == 4 || s == 12)
            p.velocity[1][s] = v;          // snare on 2 and 4
        if (s % 2 == 0)
            p.velocity[2][s] = (s % 4 == 0) ? 70 : v; // closed hats
    }
    p.velocity[3][14] = 90;                // open hat pickup
    p.velocity[7][0] = v;                  // bass
    p.velocity[7][3] = 90;
    p.velocity[7][10] = 90;
    channels[2].volume = 64;
    channels[3].volume = 60;
}
