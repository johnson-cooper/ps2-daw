#include "audio/audio_engine.hpp"

#include <string.h>

#include "audio/pitch.hpp"

AudioEngine::AudioEngine() : bank_(nullptr), hwCount_(0), blockStart_(0), queuedPattern_(-1), queuedQuantum_(16), clipCount_(0), songSteps_(0), songMode_(false)
{
    memset(channelGate_, 0, sizeof(channelGate_));
    memset(chokeDone_, 0, sizeof(chokeDone_));
    trackMute_ = trackSolo_ = 0;
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        channelSample_[ch] = -1;
        channelMode_[ch] = VoiceMode::Software;
    }
    memset(sampleHwReady_, 0, sizeof(sampleHwReady_));
    memset(hw_, 0, sizeof(hw_));
    memset(status_.marks, 0, sizeof(status_.marks));
    memset(clips_, 0, sizeof(clips_));
}

void AudioEngine::recomputeSong()
{
    int end = 0;
    for (int i = 0; i < clipCount_; ++i)
        if (clips_[i].startBar + clips_[i].lengthBars > end)
            end = clips_[i].startBar + clips_[i].lengthBars;
    songSteps_ = end * (cfg::kStepsPerBeat * cfg::kBeatsPerBar);
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
        if (queuedPattern_ >= 0) { // nothing to wait for any more
            sequencer_.select(queuedPattern_);
            queuedPattern_ = -1;
        }
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
        queuedPattern_ = -1;
        break;
    case CmdType::QueuePattern:
        if (c.a >= cfg::kMaxPatterns)
            break;
        if (!transport_.playing() || c.b == (uint8_t)SwitchMode::Immediate) {
            sequencer_.select(c.a);
            queuedPattern_ = -1;
        } else {
            queuedPattern_ = c.a;
            queuedQuantum_ = c.b == (uint8_t)SwitchMode::NextBeat ? cfg::kStepsPerBeat : cfg::kStepsPerBeat * cfg::kBeatsPerBar;
        }
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
        if (channelIndexOk(c.a)) {
            memset(chokeDone_, 0, sizeof(chokeDone_));
            triggerChannel(c.a, cfg::kDefaultVelocity, 0, c.value, 2);
        }
        break;
    case CmdType::PreviewSample:
        triggerSample(-1, c.value, cfg::kDefaultVelocity, c.b == (int)VoiceMode::Spu2 ? VoiceMode::Spu2 : VoiceMode::Software, 0);
        break;
    case CmdType::SetSampleHwReady:
        if (c.value >= 0 && c.value < cfg::kMaxSamples)
            sampleHwReady_[c.value] = c.a ? 1 : 0;
        break;
    case CmdType::SetClip:
        if (c.a < cfg::kMaxClips) {
            Clip& k = clips_[c.a];
            k.track = c.b;
            k.pattern = c.c < cfg::kMaxPatterns ? c.c : 0;
            k.startBar = (uint16_t)(c.value & 0xffff);
            k.lengthBars = (uint16_t)((uint32_t)c.value >> 16);
            recomputeSong();
        }
        break;
    case CmdType::SetClipCount:
        clipCount_ = c.value < 0 ? 0 : (c.value > cfg::kMaxClips ? cfg::kMaxClips : c.value);
        recomputeSong();
        break;
    case CmdType::SetNote: {
        Sequencer::Note n;
        const uint32_t v = (uint32_t)c.value;
        n.step = (uint8_t)v;
        n.pitch = (uint8_t)(v >> 8) & 0x7f;
        n.velocity = (uint8_t)(v >> 16) & 0x7f;
        n.length = (uint8_t)(v >> 24);
        if (n.length == 0)
            n.length = 1;
        sequencer_.setNote(c.a, c.b, c.c, n);
        break;
    }
    case CmdType::SetNoteCount:
        sequencer_.setNoteCount(c.a, c.b, c.value);
        break;
    case CmdType::SetChannelGate:
        if (channelIndexOk(c.a))
            channelGate_[c.a] = c.value ? 1 : 0;
        break;
    case CmdType::SetTrackMask:
        trackMute_ = (uint8_t)(c.value & 0xff);
        trackSolo_ = (uint8_t)((c.value >> 8) & 0xff);
        break;
    case CmdType::PlayFromBar:
        if (songActive()) {
            int bar = c.value < 0 ? 0 : c.value;
            if (bar * (cfg::kStepsPerBeat * cfg::kBeatsPerBar) >= songSteps_)
                bar = 0;
            mixer_.releaseAll();
            transport_.startAtStep(bar * (cfg::kStepsPerBeat * cfg::kBeatsPerBar));
        } else {
            transport_.play();
        }
        break;
    case CmdType::SetSongMode:
        songMode_ = c.value != 0;
        break;
    case CmdType::ReleaseSample:
        // The UI already made the slot unreadable for new voices (get() is
        // null). Drop the voices still reading it, forget it everywhere, and
        // only then acknowledge: after this the UI may free the PCM.
        if (c.value >= 0 && c.value < cfg::kMaxSamples && bank_) {
            mixer_.stopSample(bank_->peek(c.value));
            for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
                if (channelSample_[ch] == c.value)
                    channelSample_[ch] = -1;
            sampleHwReady_[c.value] = 0;
            bank_->acknowledgeRelease(c.value);
        }
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
            if (songActive()) {
                // The playlist is a linear timeline: the transport wraps at the
                // end of the last clip.
                const int songStep = transport_.consumeStep(songSteps_);
                fireSongStep(songStep, (uint32_t)pos);
                continue;
            }
            const int step = transport_.consumeStep(sequencer_.length(sequencer_.current()));
            // A queued pattern starts on a beat/bar boundary (a loop point
            // always qualifies). The new pattern must have a step here,
            // otherwise wait for the next loop.
            if (queuedPattern_ >= 0 && step % queuedQuantum_ == 0 && step < sequencer_.length(queuedPattern_)) {
                sequencer_.select(queuedPattern_);
                queuedPattern_ = -1;
            }
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

bool AudioEngine::trackAudible(int track) const
{
    if (trackSolo_)
        return (trackSolo_ >> track) & 1;
    return !((trackMute_ >> track) & 1);
}

void AudioEngine::fireNotes(int pattern, int localStep, uint32_t frameInBlock)
{
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        const int n = sequencer_.noteCount(pattern, ch);
        if (n == 0 || !mixer_.audible(ch))
            continue;
        for (int i = 0; i < n; ++i) {
            const Sequencer::Note& note = sequencer_.note(pattern, ch, i);
            if (note.step == localStep && note.velocity > 0)
                triggerChannel(ch, note.velocity, frameInBlock, (int)note.pitch - pitch::kRootNote, note.length);
        }
    }
}

void AudioEngine::fireStep(int step, uint32_t frameInBlock)
{
    const int pattern = sequencer_.current();

    const uint32_t serial = status_.markSerial;
    StepMark& m = status_.marks[serial % EngineStatus::kMarks];
    m.frame = blockStart_ + frameInBlock;
    m.songStep = (uint32_t)step;
    m.step = (uint8_t)step;
    m.pattern = (uint8_t)pattern;
    __atomic_store_n(&status_.markSerial, serial + 1, __ATOMIC_RELEASE);

    memset(chokeDone_, 0, sizeof(chokeDone_));
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        const int vel = sequencer_.velocity(pattern, ch, step);
        if (vel > 0 && mixer_.audible(ch))
            triggerChannel(ch, vel, frameInBlock);
    }
    fireNotes(pattern, step, frameInBlock);
}

void AudioEngine::fireSongStep(int songStep, uint32_t frameInBlock)
{
    const int bar = songStep / (cfg::kStepsPerBeat * cfg::kBeatsPerBar);
    uint8_t vel[cfg::kMaxChannels];
    memset(vel, 0, sizeof(vel));
    int firstPattern = -1, firstStep = 0;
    int active[cfg::kMaxClips], activeLocal[cfg::kMaxClips];
    int activeCount = 0;
    for (int i = 0; i < clipCount_; ++i) {
        const Clip& k = clips_[i];
        if (bar < k.startBar || bar >= k.startBar + k.lengthBars || !trackAudible(k.track))
            continue;
        // The pattern restarts at the clip start and loops while the clip lasts.
        const int local = (songStep - k.startBar * (cfg::kStepsPerBeat * cfg::kBeatsPerBar)) % sequencer_.length(k.pattern);
        if (firstPattern < 0 || k.track < clips_[firstPattern].track) {
            firstPattern = i;
            firstStep = local;
        }
        active[activeCount] = i;
        activeLocal[activeCount++] = local;
        for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
            const int v = sequencer_.velocity(k.pattern, ch, local);
            if (v > vel[ch])
                vel[ch] = (uint8_t)v; // two clips hitting one channel at once: the louder wins
        }
    }
    if (firstPattern >= 0)
        sequencer_.select(clips_[firstPattern].pattern); // status/UI show what is sounding

    const uint32_t serial = status_.markSerial;
    StepMark& m = status_.marks[serial % EngineStatus::kMarks];
    m.frame = blockStart_ + frameInBlock;
    m.songStep = (uint32_t)songStep;
    m.step = (uint8_t)firstStep;
    m.pattern = (uint8_t)sequencer_.current();
    __atomic_store_n(&status_.markSerial, serial + 1, __ATOMIC_RELEASE);

    memset(chokeDone_, 0, sizeof(chokeDone_));
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        if (vel[ch] > 0 && mixer_.audible(ch))
            triggerChannel(ch, vel[ch], frameInBlock);
    for (int i = 0; i < activeCount; ++i)
        fireNotes(clips_[active[i]].pattern, activeLocal[i], frameInBlock);
}

void AudioEngine::triggerChannel(int ch, int velocity, uint32_t frameInBlock, int semis, int gateSteps)
{
    if (triggerSample(ch, channelSample_[ch], velocity, channelMode_[ch], frameInBlock, semis, gateSteps)) {
        status_.lastTriggerFrame[ch] = blockStart_ + frameInBlock;
        status_.triggerCount[ch] = status_.triggerCount[ch] + 1;
    }
}

bool AudioEngine::triggerSample(int ch, int slot, int velocity, VoiceMode mode, uint32_t frameInBlock, int semis, int gateSteps)
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
        t.semis = (int8_t)(semis < -pitch::kMaxSemis ? -pitch::kMaxSemis : (semis > pitch::kMaxSemis ? pitch::kMaxSemis : semis));
        status_.hwTriggers = status_.hwTriggers + 1;
        return true;
    }

    // Software voices start exactly at their segment boundary: render() mixes
    // in segments split at every step, so the voice's first frame lands on
    // frameInBlock with sample accuracy.
    const bool gated = ch >= 0 && channelGate_[ch];
    uint32_t gateFrames = 0;
    if (gated && gateSteps > 0) {
        // frames per step = 48000 * 60 / (4 * bpm) = 72,000,000 / bpmCenti
        const uint32_t bpm = transport_.bpmCenti() ? transport_.bpmCenti() : 1;
        gateFrames = (uint32_t)((uint64_t)gateSteps * 72000000ull / bpm);
    }
    // Sustained (gated) channels layer notes; one-shot channels retrigger like a drum pad
    // (but several notes landing on one step, a chord, do not cut each other).
    bool choke = !gated;
    if (ch >= 0) {
        if (chokeDone_[ch])
            choke = false;
        chokeDone_[ch] = true;
    }
    return mixer_.trigger(ch, s, velocity, semis, gateFrames, choke);
}

void AudioEngine::publish()
{
    status_.transport = (uint8_t)transport_.state();
    status_.pattern = (uint8_t)sequencer_.current();
    status_.songMode = songActive() ? 1 : 0;
    status_.songBars = (uint16_t)(songSteps_ / (cfg::kStepsPerBeat * cfg::kBeatsPerBar));
    status_.queuedPattern = (uint8_t)(queuedPattern_ < 0 ? 0xff : queuedPattern_);
    status_.bpmCenti = transport_.bpmCenti();
    status_.songFrames = transport_.songFrames();
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        status_.meter[ch] = mixer_.channelMeter(ch);
    status_.meterL = mixer_.masterMeterL();
    status_.meterR = mixer_.masterMeterR();
    status_.voicesActive = (uint32_t)mixer_.activeVoices();
    status_.voiceSteals = mixer_.voiceSteals();
}
