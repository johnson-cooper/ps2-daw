#include "project/session.hpp"

#include <string.h>

#include "core/strutil.hpp"

Session::Session(AudioEngine& engine, const SampleBank& bank, bool (*waitFn)())
    : engine_(engine), bank_(bank), wait_(waitFn), dropped_(0)
{
    project_.resetDemo();
}

void Session::post(CmdType t, int a, int b, int c, int32_t value)
{
    Command cmd;
    cmd.type = t;
    cmd.a = (uint8_t)a;
    cmd.b = (uint8_t)b;
    cmd.c = (uint8_t)c;
    cmd.value = value;
    // The audio thread drains the queue every block (~10 ms), so a full queue
    // clears quickly; give up after a bounded wait rather than hanging the UI.
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (engine_.post(cmd))
            return;
        if (!wait_ || !wait_())
            break;
    }
    ++dropped_;
}

int Session::resolveSample(const ChannelData& c) const
{
    static const char kPrefix[] = "builtin:";
    if (strncmp(c.sampleRef, kPrefix, sizeof(kPrefix) - 1) == 0)
        return bank_.findByName(c.sampleRef + sizeof(kPrefix) - 1);
    // Storage paths are resolved by the sample loader (Milestone 2).
    return -1;
}

void Session::loadProject(const Project& p)
{
    post(CmdType::Stop);
    if (&p != &project_)
        project_ = p;
    for (auto& c : project_.channels)
        if (c.sampleSlot < 0 || !bank_.get(c.sampleSlot))
            c.sampleSlot = (int8_t)resolveSample(c);
    syncAll();
}

void Session::syncAll()
{
    const Project& p = project_;
    post(CmdType::SetBpm, 0, 0, 0, (int32_t)p.bpmCenti);
    post(CmdType::SetMasterVolume, 0, 0, 0, p.masterVolume);
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        const ChannelData& c = p.channels[ch];
        post(CmdType::SetChannelSample, ch, 0, 0, c.sampleSlot);
        post(CmdType::SetChannelVolume, ch, 0, 0, c.volume);
        post(CmdType::SetChannelPan, ch, 0, 0, c.pan);
        post(CmdType::SetChannelMute, ch, 0, 0, c.mute);
        post(CmdType::SetChannelSolo, ch, 0, 0, c.solo);
        post(CmdType::SetChannelVoiceMode, ch, 0, 0, c.voiceMode);
    }
    for (int pi = 0; pi < cfg::kMaxPatterns; ++pi) {
        const PatternData& pd = p.patterns[pi];
        post(CmdType::ClearPattern, pi);
        post(CmdType::SetPatternLength, pi, 0, 0, pd.length);
        for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
            for (int s = 0; s < cfg::kMaxSteps; ++s)
                if (pd.velocity[ch][s])
                    post(CmdType::SetStep, pi, ch, s, pd.velocity[ch][s]);
    }
    post(CmdType::SelectPattern, p.currentPattern);
}

void Session::play() { post(CmdType::Play); }
void Session::pause() { post(CmdType::Pause); }
void Session::stop() { post(CmdType::Stop); }

void Session::togglePlayPause()
{
    if (engine_.status().transport == (uint8_t)Transport::State::Playing)
        pause();
    else
        play();
}

void Session::setBpmCenti(int bpmCenti)
{
    if (bpmCenti < (int)cfg::kMinBpmCenti)
        bpmCenti = (int)cfg::kMinBpmCenti;
    if (bpmCenti > (int)cfg::kMaxBpmCenti)
        bpmCenti = (int)cfg::kMaxBpmCenti;
    project_.bpmCenti = (uint32_t)bpmCenti;
    post(CmdType::SetBpm, 0, 0, 0, bpmCenti);
}

static bool inRange(int v, int n) { return v >= 0 && v < n; }

void Session::setStep(int pattern, int channel, int step, int velocity)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels) || !inRange(step, cfg::kMaxSteps))
        return;
    velocity = velocity < 0 ? 0 : (velocity > 127 ? 127 : velocity);
    project_.patterns[pattern].velocity[channel][step] = (uint8_t)velocity;
    post(CmdType::SetStep, pattern, channel, step, velocity);
}

void Session::toggleStep(int channel, int step)
{
    const int p = project_.currentPattern;
    if (!inRange(channel, cfg::kMaxChannels) || !inRange(step, cfg::kMaxSteps))
        return;
    const bool on = project_.patterns[p].velocity[channel][step] != 0;
    setStep(p, channel, step, on ? 0 : cfg::kDefaultVelocity);
}

void Session::clearChannelSteps(int channel)
{
    const int p = project_.currentPattern;
    for (int s = 0; s < cfg::kMaxSteps; ++s)
        if (project_.patterns[p].velocity[channel][s])
            setStep(p, channel, s, 0);
}

void Session::fillChannelEvery(int channel, int interval)
{
    if (interval < 1)
        return;
    const int p = project_.currentPattern;
    const int len = project_.patterns[p].length;
    for (int s = 0; s < len; ++s)
        setStep(p, channel, s, (s % interval == 0) ? cfg::kDefaultVelocity : 0);
}

void Session::clearPattern(int pattern)
{
    if (!inRange(pattern, cfg::kMaxPatterns))
        return;
    memset(project_.patterns[pattern].velocity, 0, sizeof(project_.patterns[pattern].velocity));
    post(CmdType::ClearPattern, pattern);
}

void Session::setPatternLength(int pattern, int steps)
{
    if (!inRange(pattern, cfg::kMaxPatterns))
        return;
    steps = steps < 1 ? 1 : (steps > cfg::kMaxSteps ? cfg::kMaxSteps : steps);
    project_.patterns[pattern].length = (uint8_t)steps;
    post(CmdType::SetPatternLength, pattern, 0, 0, steps);
}

void Session::selectPattern(int pattern)
{
    if (!inRange(pattern, cfg::kMaxPatterns))
        return;
    project_.currentPattern = (uint8_t)pattern;
    post(CmdType::SelectPattern, pattern);
}

void Session::setVolume(int channel, int volume)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    project_.channels[channel].volume = (uint8_t)volume;
    post(CmdType::SetChannelVolume, channel, 0, 0, volume);
}

void Session::setPan(int channel, int pan)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    pan = pan < -100 ? -100 : (pan > 100 ? 100 : pan);
    project_.channels[channel].pan = (int8_t)pan;
    post(CmdType::SetChannelPan, channel, 0, 0, pan);
}

void Session::setMute(int channel, bool mute)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    project_.channels[channel].mute = mute ? 1 : 0;
    post(CmdType::SetChannelMute, channel, 0, 0, mute ? 1 : 0);
}

void Session::setSolo(int channel, bool solo)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    project_.channels[channel].solo = solo ? 1 : 0;
    post(CmdType::SetChannelSolo, channel, 0, 0, solo ? 1 : 0);
}

void Session::setSample(int channel, int slot)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    const Sample* s = bank_.get(slot);
    ChannelData& c = project_.channels[channel];
    c.sampleSlot = (int8_t)(s ? slot : -1);
    if (s) {
        str::copy(c.name, sizeof(c.name), s->name);
        str::copy(c.sampleRef, sizeof(c.sampleRef), s->builtin ? "builtin:" : "");
        str::append(c.sampleRef, sizeof(c.sampleRef), s->name);
    }
    post(CmdType::SetChannelSample, channel, 0, 0, c.sampleSlot);
}

void Session::setVoiceMode(int channel, VoiceMode mode)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    project_.channels[channel].voiceMode = (uint8_t)mode;
    post(CmdType::SetChannelVoiceMode, channel, 0, 0, (int)mode);
}

void Session::setMasterVolume(int volume)
{
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    project_.masterVolume = (uint8_t)volume;
    post(CmdType::SetMasterVolume, 0, 0, 0, volume);
}

void Session::previewChannel(int channel)
{
    if (inRange(channel, cfg::kMaxChannels))
        post(CmdType::PreviewChannel, channel);
}

void Session::previewSample(int slot, VoiceMode mode)
{
    post(CmdType::PreviewSample, 0, (int)mode, 0, slot);
}

void Session::setSampleHwReady(int slot, bool ready)
{
    post(CmdType::SetSampleHwReady, ready ? 1 : 0, 0, 0, slot);
}
