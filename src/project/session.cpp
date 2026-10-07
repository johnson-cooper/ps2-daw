#include "project/session.hpp"

#include <string.h>

#include "audio/sample_ref.hpp"
#include "core/strutil.hpp"

Session::Session(AudioEngine& engine, SampleBank& bank, bool (*waitFn)())
    : engine_(engine), bank_(bank), wait_(waitFn), dropped_(0), loadSerial_(0), releasePending_(0)
{
    project_.resetDemo();
}

bool Session::post(CmdType t, int a, int b, int c, int32_t value)
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
            return true;
        if (!wait_ || !wait_())
            break;
    }
    ++dropped_;
    return false;
}

int Session::resolveSample(const ChannelData& c) const
{
    return bank_.findByRef(c.sampleRef);
}

void Session::loadProject(const Project& p)
{
    post(CmdType::Stop);
    if (&p != &project_)
        project_ = p;
    // The slot is runtime state (files store only the reference and load with
    // -1). Keep a slot that is still valid, otherwise resolve by reference.
    for (auto& c : project_.channels)
        if (c.sampleSlot < 0 || !bank_.get(c.sampleSlot))
            c.sampleSlot = (int8_t)resolveSample(c);
    ++loadSerial_;
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
    post(CmdType::SelectPattern, p.currentPattern); // immediate: the whole song was just replaced
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
    if (switchMode_ == SwitchMode::Immediate)
        post(CmdType::SelectPattern, pattern);
    else
        post(CmdType::QueuePattern, pattern, (int)switchMode_);
}

bool Session::patternIsEmpty(int pattern) const
{
    if (!inRange(pattern, cfg::kMaxPatterns))
        return false;
    const PatternData& pd = project_.patterns[pattern];
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        for (int s = 0; s < cfg::kMaxSteps; ++s)
            if (pd.velocity[ch][s])
                return false;
    return true;
}

bool Session::copyPattern(int src, int dst)
{
    if (!inRange(src, cfg::kMaxPatterns) || !inRange(dst, cfg::kMaxPatterns))
        return false;
    if (src == dst)
        return true;
    PatternData& to = project_.patterns[dst];
    const PatternData& from = project_.patterns[src];
    to.length = from.length;
    memcpy(to.velocity, from.velocity, sizeof(to.velocity));
    post(CmdType::ClearPattern, dst);
    post(CmdType::SetPatternLength, dst, 0, 0, to.length);
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        for (int s = 0; s < cfg::kMaxSteps; ++s)
            if (to.velocity[ch][s])
                post(CmdType::SetStep, dst, ch, s, to.velocity[ch][s]);
    return true;
}

int Session::duplicatePattern(int src)
{
    if (!inRange(src, cfg::kMaxPatterns))
        return -1;
    for (int i = 1; i <= cfg::kMaxPatterns; ++i) {
        const int dst = (src + i) % cfg::kMaxPatterns;
        if (dst != src && patternIsEmpty(dst)) {
            copyPattern(src, dst);
            // Same name with a copy marker; the user can rename it.
            char name[sizeof(project_.patterns[0].name)];
            str::copy(name, sizeof(name), project_.patterns[src].name);
            const size_t n = strlen(name);
            if (n + 2 < sizeof(name))
                str::append(name, sizeof(name), "+");
            setPatternName(dst, name);
            selectPattern(dst);
            return dst;
        }
    }
    return -1;
}

void Session::setPatternName(int pattern, const char* name)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !name)
        return;
    str::copy(project_.patterns[pattern].name, sizeof(project_.patterns[pattern].name), name);
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
        str::copy(c.sampleRef, sizeof(c.sampleRef), s->ref);
    }
    post(CmdType::SetChannelSample, channel, 0, 0, c.sampleSlot);
    // SPU2-only (.adp) samples have no software path.
    if (s && s->hwOnly)
        setVoiceMode(channel, VoiceMode::Spu2);
}

int Session::bindSamples()
{
    int unresolved = 0;
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        ChannelData& c = project_.channels[ch];
        const int slot = resolveSample(c);
        if (slot != c.sampleSlot) {
            c.sampleSlot = (int8_t)slot;
            post(CmdType::SetChannelSample, ch, 0, 0, slot);
        }
        if (slot < 0 && sampleref::classify(c.sampleRef) != sampleref::Kind::None &&
            sampleref::classify(c.sampleRef) != sampleref::Kind::Builtin)
            ++unresolved;
        const Sample* s = bank_.get(slot);
        if (s && s->hwOnly && c.voiceMode != (uint8_t)VoiceMode::Spu2)
            setVoiceMode(ch, VoiceMode::Spu2);
    }
    return unresolved;
}

bool Session::slotInUse(int slot) const
{
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        if (project_.channels[ch].sampleSlot == slot)
            return true;
    return false;
}

bool Session::releaseSample(int slot)
{
    if (slotInUse(slot) || !bank_.requestRelease(slot))
        return false;
    // The state change above comes first: from here the audio thread can no
    // longer start voices on this slot. The command makes it drop the ones it
    // already has and acknowledge, after which reap() may free the memory.
    if (!post(CmdType::ReleaseSample, 0, 0, 0, slot)) {
        if (wait_ && wait_())
            releasePending_ |= 1u << slot; // audio is alive; retry from pumpReleases()
        else
            bank_.acknowledgeRelease(slot); // no audio thread exists to read it
    }
    return true;
}

void Session::pumpReleases()
{
    if (!releasePending_)
        return;
    for (int slot = 0; slot < cfg::kMaxSamples; ++slot) {
        if (!(releasePending_ & (1u << slot)))
            continue;
        if (!bank_.isReleasing(slot)) {
            releasePending_ &= ~(1u << slot);
            continue;
        }
        Command cmd;
        cmd.type = CmdType::ReleaseSample;
        cmd.a = cmd.b = cmd.c = 0;
        cmd.value = slot;
        if (engine_.post(cmd))
            releasePending_ &= ~(1u << slot);
    }
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
