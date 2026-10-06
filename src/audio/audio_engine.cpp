#include "audio/audio_engine.hpp"

#include <string.h>

AudioEngine::AudioEngine() : bank_(nullptr), hwCount_(0), blockStart_(0)
{
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        channelSample_[ch] = -1;
        channelMode_[ch] = VoiceMode::Software;
    }
    memset(sampleHwReady_, 0, sizeof(sampleHwReady_));
    memset(hw_, 0, sizeof(hw_));
    memset(status_.marks, 0, sizeof(status_.marks));
}

void AudioEngine::applyCommands()
{
    // Bounded per block so a flood of edits (e.g. a project load) can never
    // stall rendering; leftovers are applied on the next block.
    Command c;
    for (int i = 0; i < 256 && queue_.pop(c); ++i) {
        apply(c);
        status_.commands = status_.commands + 1;
    }
}

static bool channelIndexOk(int ch) { return ch >= 0 && ch < cfg::kMaxChannels; }

void AudioEngine::apply(const Command& c)
{
    switch (c.type) {
    case CmdType::Play:
        transport_.play();
        break;
    case CmdType::Pause:
        transport_.pause();
        break;
    case CmdType::Stop:
        transport_.stop();
        mixer_.releaseAll();
        break;
    case CmdType::SetBpm:
        transport_.setBpmCenti((uint32_t)(c.value < 0 ? 0 : c.value));
        break;
    case CmdType::SetStep:
        sequencer_.setStep(c.a, c.b, c.c, c.value);
        break;
    case CmdType::ClearPattern:
        sequencer_.clearPattern(c.a);
        break;
    case CmdType::SetPatternLength:
        sequencer_.setLength(c.a, c.value);
        break;
    case CmdType::SelectPattern:
        sequencer_.select(c.a);
        break;
    case CmdType::SetChannelSample:
        if (channelIndexOk(c.a))
            channelSample_[c.a] = (int8_t)((c.value >= 0 && c.value < cfg::kMaxSamples) ? c.value : -1);
        break;
    case CmdType::SetChannelVolume:
        if (channelIndexOk(c.a))
            mixer_.strip(c.a).volume = (uint8_t)(c.value < 0 ? 0 : (c.value > 100 ? 100 : c.value));
        break;
    case CmdType::SetChannelPan:
        if (channelIndexOk(c.a))
            mixer_.strip(c.a).pan = (int8_t)(c.value < -100 ? -100 : (c.value > 100 ? 100 : c.value));
        break;
    case CmdType::SetChannelMute:
        if (channelIndexOk(c.a))
            mixer_.strip(c.a).mute = c.value ? 1 : 0;
        break;
    case CmdType::SetChannelSolo:
        if (channelIndexOk(c.a))
            mixer_.strip(c.a).solo = c.value ? 1 : 0;
        break;
    case CmdType::SetChannelVoiceMode:
        if (channelIndexOk(c.a))
            channelMode_[c.a] = c.value == (int)VoiceMode::Spu2 ? VoiceMode::Spu2 : VoiceMode::Software;
        break;
    case CmdType::SetMasterVolume:
        mixer_.setMasterVolume(c.value);
        break;
    case CmdType::PreviewChannel:
        if (channelIndexOk(c.a))
            triggerChannel(c.a, cfg::kDefaultVelocity, 0);
        break;
    case CmdType::PreviewSample:
        triggerSample(-1, c.value, cfg::kDefaultVelocity, c.b == (int)VoiceMode::Spu2 ? VoiceMode::Spu2 : VoiceMode::Software, 0);
        break;
    case CmdType::SetSampleHwReady:
        if (c.value >= 0 && c.value < cfg::kMaxSamples)
            sampleHwReady_[c.value] = c.a ? 1 : 0;
        break;
    case CmdType::AllVoicesOff:
        mixer_.releaseAll();
        break;
    }
}

void AudioEngine::render(int16_t* out, int frames)
{
    if (frames > cfg::kMaxBlockFrames)
        frames = cfg::kMaxBlockFrames;
    if (frames <= 0)
        return;

    applyCommands();
    hwCount_ = 0;
    blockStart_ = status_.renderedFrames;
    mixer_.beginBlock(frames);

    int pos = 0;
    while (pos < frames) {
        if (!transport_.playing()) {
            mixer_.mixSegment(pos, frames - pos);
            break;
        }
        const uint32_t n = transport_.framesUntilNextStep((uint32_t)(frames - pos));
        if (n == 0) {
            const int step = transport_.consumeStep(sequencer_.length(sequencer_.current()));
            fireStep(step, (uint32_t)pos);
            continue;
        }
        mixer_.mixSegment(pos, (int)n);
        transport_.advance(n);
        pos += (int)n;
    }

    const uint32_t clips = mixer_.finishBlock(out, frames);
    status_.clipSamples = status_.clipSamples + clips;
    status_.renderedFrames = blockStart_ + (uint32_t)frames;
    publish();
}

void AudioEngine::fireStep(int step, uint32_t frameInBlock)
{
    const int pattern = sequencer_.current();

    const uint32_t serial = status_.markSerial;
    StepMark& m = status_.marks[serial % EngineStatus::kMarks];
    m.frame = blockStart_ + frameInBlock;
    m.step = (uint8_t)step;
    m.pattern = (uint8_t)pattern;
    __atomic_store_n(&status_.markSerial, serial + 1, __ATOMIC_RELEASE);

    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        const int vel = sequencer_.velocity(pattern, ch, step);
        if (vel > 0 && mixer_.audible(ch))
            triggerChannel(ch, vel, frameInBlock);
    }
}

void AudioEngine::triggerChannel(int ch, int velocity, uint32_t frameInBlock)
{
    if (triggerSample(ch, channelSample_[ch], velocity, channelMode_[ch], frameInBlock)) {
        status_.lastTriggerFrame[ch] = blockStart_ + frameInBlock;
        status_.triggerCount[ch] = status_.triggerCount[ch] + 1;
    }
}

bool AudioEngine::triggerSample(int ch, int slot, int velocity, VoiceMode mode, uint32_t frameInBlock)
{
    if (!bank_ || slot < 0 || slot >= cfg::kMaxSamples)
        return false;
    const Sample* s = bank_->get(slot);
    if (!s)
        return false;

    if (mode == VoiceMode::Spu2 && sampleHwReady_[slot]) {
        if (hwCount_ >= kMaxHwPerBlock) {
            status_.hwDropped = status_.hwDropped + 1;
            return false;
        }
        int32_t q15 = (int32_t)velocity * 32767 / 127;
        int pan = 0;
        if (ch >= 0) {
            q15 = (q15 * Mixer::volumeToQ15(mixer_.strip(ch).volume)) >> 15;
            pan = mixer_.strip(ch).pan;
        }
        HwTrigger& t = hw_[hwCount_++];
        t.frame = blockStart_ + frameInBlock;
        t.channel = ch >= 0 ? (uint8_t)ch : 0xff;
        t.sample = (uint8_t)slot;
        t.volume = (uint8_t)((q15 * 100 + 16383) / 32767);
        t.pan = (int8_t)pan;
        status_.hwTriggers = status_.hwTriggers + 1;
        return true;
    }

    // Software voices start exactly at their segment boundary: render() mixes
    // in segments split at every step, so the voice's first frame lands on
    // frameInBlock with sample accuracy.
    return mixer_.trigger(ch, s, velocity);
}

void AudioEngine::publish()
{
    status_.transport = (uint8_t)transport_.state();
    status_.pattern = (uint8_t)sequencer_.current();
    status_.bpmCenti = transport_.bpmCenti();
    status_.songFrames = transport_.songFrames();
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        status_.meter[ch] = mixer_.channelMeter(ch);
    status_.meterL = mixer_.masterMeterL();
    status_.meterR = mixer_.masterMeterR();
    status_.voicesActive = (uint32_t)mixer_.activeVoices();
    status_.voiceSteals = mixer_.voiceSteals();
}
