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

void Project::sanitizeClips()
{
    int kept = 0;
    PlaylistClip out[kMaxClips];
    const int n = clipCount > kMaxClips ? kMaxClips : clipCount;
    for (int i = 0; i < n; ++i) {
        PlaylistClip c = clips[i];
        if (c.track >= cfg::kPlaylistTracks || c.pattern >= cfg::kMaxPatterns || c.lengthBars == 0 ||
            c.startBar >= cfg::kMaxSongBars)
            continue;
        if ((int)c.startBar + c.lengthBars > cfg::kMaxSongBars)
            c.lengthBars = (uint16_t)(cfg::kMaxSongBars - c.startBar);
        bool overlap = false;
        for (int k = 0; k < kept && !overlap; ++k)
            overlap = out[k].track == c.track && c.startBar < out[k].startBar + out[k].lengthBars &&
                      out[k].startBar < c.startBar + c.lengthBars;
        if (!overlap)
            out[kept++] = c;
    }
    // Insertion sort by (startBar, track): stable and tiny.
    for (int i = 1; i < kept; ++i) {
        PlaylistClip t = out[i];
        int j = i - 1;
        while (j >= 0 && (out[j].startBar > t.startBar || (out[j].startBar == t.startBar && out[j].track > t.track))) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = t;
    }
    memset(clips, 0, sizeof(clips));
    for (int i = 0; i < kept; ++i)
        clips[i] = out[i];
    clipCount = (uint16_t)kept;
}

int Project::clipAt(int track, int bar) const
{
    for (int i = 0; i < clipCount && i < kMaxClips; ++i)
        if (clips[i].track == track && bar >= clips[i].startBar && bar < clips[i].startBar + clips[i].lengthBars)
            return i;
    return -1;
}

int Project::songBars() const
{
    int end = 0;
    for (int i = 0; i < clipCount && i < kMaxClips; ++i)
        if (clips[i].startBar + clips[i].lengthBars > end)
            end = clips[i].startBar + clips[i].lengthBars;
    return end;
}
