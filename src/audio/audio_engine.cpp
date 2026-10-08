#include "audio/audio_engine.hpp"

#include <string.h>

#include "audio/params.hpp"
#include "audio/pitch.hpp"

AudioEngine::AudioEngine() : bank_(nullptr), hwCount_(0), blockStart_(0), queuedPattern_(-1), queuedQuantum_(16), clipCount_(0), aclipCount_(0), justStarted_(false), exportPasses_(0), songSteps_(0), songMode_(false)
{
    memset(aclips_, 0, sizeof(aclips_));
    for (auto& a : aclips_)
        a.slot = 0xff;
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
    for (int i = 0; i < aclipCount_; ++i)
        if (aclips_[i].slot != 0xff && aclips_[i].startBar + aclips_[i].lengthBars > end)
            end = aclips_[i].startBar + aclips_[i].lengthBars;
    songSteps_ = end * (cfg::kStepsPerBeat * cfg::kBeatsPerBar);
}

void AudioEngine::applyCommands()
{
    // Bounded per block (the queue capacity) so nothing can stall rendering.
    // A full project load posts a few hundred commands (patterns, instruments,
    // mixer, effects) and must land before the first step it precedes.
    Command c;
    for (int i = 0; i < 1024 && queue_.pop(c); ++i) {
        apply(c);
        status_.commands = status_.commands + 1;
    }
}

static bool channelIndexOk(int ch) { return ch >= 0 && ch < cfg::kMaxChannels; }

void AudioEngine::apply(const Command& c)
{
    switch (c.type) {
    case CmdType::Play:
        if (transport_.state() == Transport::State::Stopped)
            justStarted_ = true;
        transport_.play();
        break;
    case CmdType::Pause:
        pendingCount_ = 0;
        transport_.pause();
        break;
    case CmdType::Stop:
        pendingCount_ = 0;
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
            triggerChannel(c.a, cfg::kDefaultVelocity, 0, c.value, 2 * cfg::kTicksPerStep);
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
            k.mask = 0xffff; // SetClipMask narrows it
            recomputeSong();
        }
        break;
    case CmdType::SetClipMask:
        if (c.a < cfg::kMaxClips)
            clips_[c.a].mask = (uint16_t)((uint32_t)c.value & 0xffff);
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
        n.tick = 0; // sub-step timing arrives in SetNoteFine
        n.reserved = 0;
        n.lenTicks = 0;
        sequencer_.setNote(c.a, c.b, c.c, n);
        break;
    }
    case CmdType::SetNoteFine:
        if (c.a < cfg::kMaxPatterns && c.b < cfg::kMaxChannels && c.c < cfg::kMaxNotes) {
            Sequencer::Note n = sequencer_.note(c.a, c.b, c.c);
            n.tick = (uint8_t)((uint32_t)c.value & 0xff);
            if (n.tick >= cfg::kTicksPerStep)
                n.tick = 0;
            n.lenTicks = (uint16_t)(((uint32_t)c.value >> 8) & 0xffff);
            sequencer_.setNote(c.a, c.b, c.c, n);
        }
        break;
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
            justStarted_ = true;
            transport_.startAtStep(bar * (cfg::kStepsPerBeat * cfg::kBeatsPerBar));
        } else {
            if (transport_.state() == Transport::State::Stopped)
                justStarted_ = true;
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
            for (int i = 0; i < cfg::kMaxAudioClips; ++i)
                if (aclips_[i].slot == c.value)
                    aclips_[i].slot = 0xff;
            bank_->acknowledgeRelease(c.value);
        }
        break;
    case CmdType::AllVoicesOff:
        mixer_.releaseAll();
        break;
    case CmdType::SetChannelRoute:
        if (channelIndexOk(c.a))
            mixer_.strip(c.a).route = (uint8_t)(c.value < 0 || c.value > cfg::kMixTracks ? 0 : c.value);
        break;
    case CmdType::SetChannelKind:
        if (channelIndexOk(c.a))
            mixer_.instrument(c.a).kind = c.value == (int)InstrKind::Synth ? (uint8_t)InstrKind::Synth : (uint8_t)InstrKind::Sampler;
        break;
    case CmdType::SetEnvParam:
        if (channelIndexOk(c.a) && c.b < cfg::kEnvParams)
            mixer_.instrument(c.a).env[c.b] = (int16_t)params::clampTo(instr::envParam(c.b), c.value);
        break;
    case CmdType::SetSynthParam:
        if (channelIndexOk(c.a) && c.b < cfg::kSynthParams)
            mixer_.instrument(c.a).synth[c.b] = c.b < instr::synthParamCount() ? (int16_t)params::clampTo(instr::synthParam(c.b), c.value) : 0;
        break;
    case CmdType::SetTrackParam:
        if (c.a >= 1 && c.a <= cfg::kMixTracks) {
            MixTrackStrip& t = mixer_.track(c.a);
            switch (c.b) {
            case 0: t.volume = (uint8_t)(c.value < 0 ? 0 : (c.value > 100 ? 100 : c.value)); break;
            case 1: t.pan = (int8_t)(c.value < -100 ? -100 : (c.value > 100 ? 100 : c.value)); break;
            case 2: t.mute = c.value ? 1 : 0; break;
            case 3: t.solo = c.value ? 1 : 0; mixer_.refreshSolo(); break;
            }
        }
        break;
    case CmdType::SetFxType:
        if (c.a < cfg::kMixBuses && c.b < cfg::kFxSlots)
            mixer_.fx(c.a, c.b).setType((FxType)(c.value >= 0 && c.value < kFxTypeCount ? c.value : 0), mixer_.fxMemory());
        break;
    case CmdType::SetFxParam:
        if (c.a < cfg::kMixBuses && c.b < cfg::kFxSlots)
            mixer_.fx(c.a, c.b).setParam(c.c, c.value);
        break;
    case CmdType::SetFxBypass:
        if (c.a < cfg::kMixBuses && c.b < cfg::kFxSlots)
            mixer_.fx(c.a, c.b).setBypass(c.value != 0);
        break;
    case CmdType::ClearClipLatch:
        mixer_.clearClips();
        break;
    case CmdType::SetAudioClip:
        if (c.a < cfg::kMaxAudioClips) {
            AudioClip& k = aclips_[c.a];
            k.track = c.b;
            k.slot = (c.c < cfg::kMaxSamples) ? c.c : 0xff;
            k.startBar = (uint16_t)(c.value & 0xffff);
            k.lengthBars = (uint16_t)((uint32_t)c.value >> 16);
            recomputeSong();
        }
        break;
    case CmdType::SetAudioClipMix:
        if (c.a < cfg::kMaxAudioClips) {
            aclips_[c.a].volume = (uint8_t)(c.b > 127 ? 127 : c.b);
            aclips_[c.a].flags = c.c;
        }
        break;
    case CmdType::SetAudioClipTrim:
        if (c.a < cfg::kMaxAudioClips) {
            if (c.b == 0)
                aclips_[c.a].trimStart = (uint32_t)c.value;
            else
                aclips_[c.a].trimEnd = (uint32_t)c.value;
        }
        break;
    case CmdType::SetAudioClipCount:
        aclipCount_ = c.value < 0 ? 0 : (c.value > cfg::kMaxAudioClips ? cfg::kMaxAudioClips : c.value);
        recomputeSong();
        break;
    case CmdType::SetSwing:
        transport_.setSwingTicks(c.value);
        break;
    case CmdType::SetMetronome:
        metronome_ = c.value != 0;
        break;
    case CmdType::SetExportMode:
        exportPasses_ = c.value < 0 ? 0 : c.value;
        status_.exportDone = 0;
        status_.exportSkipped = 0;
        break;
    }
}

void AudioEngine::render(int16_t* out, int frames)
{
    if (frames > cfg::kMaxBlockFrames)
        frames = cfg::kMaxBlockFrames;
    if (frames <= 0)
        return;

    const uint64_t t0 = clock_ ? clock_() : 0;
    applyCommands();
    const uint64_t t1 = clock_ ? clock_() : 0;
    uint64_t voicesUs = 0;
    hwCount_ = 0;
    blockStart_ = status_.renderedFrames;
    mixer_.beginBlock(frames);

    int pos = 0;
    while (pos < frames) {
        if (pendingCount_ && transport_.playing())
            firePending(blockStart_ + (uint32_t)pos, (uint32_t)pos);
        if (!transport_.playing()) {
            mixer_.mixSegment(pos, frames - pos);
            break;
        }
        uint32_t n = transport_.framesUntilNextStep((uint32_t)(frames - pos));
        if (n == 0) {
            if (songActive()) {
                // The playlist is a linear timeline: the transport wraps at the
                // end of the last clip.
                const int songStep = transport_.consumeStep(songSteps_);
                if (exporting() && (int)transport_.loopCount() >= exportPasses_) {
                    transport_.pause(); // the song ended: let the voices and effect tails ring out
                    status_.exportDone = 1;
                    continue;
                }
                fireSongStep(songStep, (uint32_t)pos);
                continue;
            }
            const int step = transport_.consumeStep(sequencer_.length(sequencer_.current()));
            if (exporting() && (int)transport_.loopCount() >= exportPasses_) {
                transport_.pause();
                status_.exportDone = 1;
                continue;
            }
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
        // end the segment where a queued sub-step note is due
        for (int i = 0; i < pendingCount_; ++i) {
            const int32_t d = (int32_t)(pending_[i].frame - (blockStart_ + (uint32_t)pos));
            if (d > 0 && (uint32_t)d < n)
                n = (uint32_t)d;
        }
        if (clock_) {
            const uint64_t a = clock_();
            mixer_.mixSegment(pos, (int)n);
            voicesUs += clock_() - a;
        } else {
            mixer_.mixSegment(pos, (int)n);
        }
        transport_.advance(n);
        pos += (int)n;
    }

    const uint64_t t2 = clock_ ? clock_() : 0;
    const uint32_t clips = mixer_.finishBlock(out, frames);
    if (clock_) {
        const uint64_t t3 = clock_();
        auto smooth = [](volatile uint32_t& v, uint32_t x) { v = (v * 15 + x) / 16; };
        smooth(status_.profCommandsUs, (uint32_t)(t1 - t0));
        smooth(status_.profVoicesUs, (uint32_t)voicesUs);
        smooth(status_.profFxUs, mixer_.fxMicros());
        const uint32_t fin = (uint32_t)(t3 - t2);
        smooth(status_.profFinishUs, fin > mixer_.fxMicros() ? fin - mixer_.fxMicros() : 0);
    }
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

void AudioEngine::fireNotes(int pattern, int localStep, uint32_t frameInBlock, uint16_t mask)
{
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        const int n = sequencer_.noteCount(pattern, ch);
        if (n == 0 || !mixer_.audible(ch) || !((mask >> ch) & 1))
            continue;
        for (int i = 0; i < n; ++i) {
            const Sequencer::Note& note = sequencer_.note(pattern, ch, i);
            if (note.step != localStep || note.velocity == 0)
                continue;
            const int semis = (int)note.pitch - pitch::kRootNote;
            if (note.tick == 0)
                triggerChannel(ch, note.velocity, frameInBlock, semis, note.durTicks());
            else
                queueNote(ch, note.velocity, blockStart_ + frameInBlock + framesForTicks(note.tick), semis, note.durTicks());
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

    if (metronome_ && !exporting() && step % cfg::kStepsPerBeat == 0)
        mixer_.triggerClick(step % (cfg::kStepsPerBeat * cfg::kBeatsPerBar) == 0);
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
            if (!((k.mask >> ch) & 1))
                continue; // an ungrouped clip plays only its own instruments
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

    if (metronome_ && !exporting() && songStep % cfg::kStepsPerBeat == 0)
        mixer_.triggerClick(songStep % (cfg::kStepsPerBeat * cfg::kBeatsPerBar) == 0);
    memset(chokeDone_, 0, sizeof(chokeDone_));
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        if (vel[ch] > 0 && mixer_.audible(ch))
            triggerChannel(ch, vel[ch], frameInBlock);
    for (int i = 0; i < activeCount; ++i)
        fireNotes(clips_[active[i]].pattern, activeLocal[i], frameInBlock, clips_[active[i]].mask);
    triggerAudioClips(songStep, frameInBlock);
    justStarted_ = false;
}

uint32_t AudioEngine::framesForTicks(int ticks) const
{
    // frames per tick = 72,000,000 / (24 * bpmCenti) = 3,000,000 / bpmCenti
    const uint32_t bpm = transport_.bpmCenti() ? transport_.bpmCenti() : 1;
    return (uint32_t)((uint64_t)(ticks < 0 ? 0 : ticks) * 3000000ull / bpm);
}

void AudioEngine::queueNote(int ch, int velocity, uint32_t dueFrame, int semis, int gateTicks)
{
    if (pendingCount_ >= kMaxPending)
        return; // a pathological pattern: drop rather than allocate
    PendingNote& p = pending_[pendingCount_++];
    p.frame = dueFrame;
    p.channel = (uint8_t)ch;
    p.velocity = (uint8_t)velocity;
    p.semis = (int8_t)semis;
    p.gateTicks = (uint16_t)(gateTicks > 0xffff ? 0xffff : gateTicks);
}

// Starts every queued note whose frame has arrived. The render loop splits its
// segments at those frames, so each begins sample-accurately. Notes due at the
// same frame (a chord) do not choke each other.
void AudioEngine::firePending(uint32_t absFrame, uint32_t frameInBlock)
{
    bool first = true;
    for (int i = 0; i < pendingCount_;) {
        const PendingNote p = pending_[i];
        if ((int32_t)(absFrame - p.frame) >= 0) {
            if (first) {
                memset(chokeDone_, 0, sizeof(chokeDone_));
                first = false;
            }
            triggerChannel(p.channel, p.velocity, frameInBlock, p.semis, p.gateTicks);
            pending_[i] = pending_[--pendingCount_];
        } else {
            ++i;
        }
    }
}

uint32_t AudioEngine::framesForSteps(int steps) const
{
    // frames per step = 48000 * 60 / (4 * bpm) = 72,000,000 / bpmCenti
    const uint32_t bpm = transport_.bpmCenti() ? transport_.bpmCenti() : 1;
    return (uint32_t)((uint64_t)(steps < 0 ? 0 : steps) * 72000000ull / bpm);
}

void AudioEngine::triggerAudioClips(int songStep, uint32_t frameInBlock)
{
    constexpr int kStepsPerBar = cfg::kStepsPerBeat * cfg::kBeatsPerBar;
    for (int i = 0; i < aclipCount_; ++i) {
        const AudioClip& k = aclips_[i];
        if (k.slot == 0xff || k.lengthBars == 0 || !trackAudible(k.track))
            continue;
        const int startStep = k.startBar * kStepsPerBar;
        const int endStep = (k.startBar + k.lengthBars) * kStepsPerBar;
        if (songStep == startStep || (justStarted_ && songStep > startStep && songStep < endStep))
            playAudioClip(i, songStep - startStep, frameInBlock);
    }
}

void AudioEngine::playAudioClip(int index, int elapsedSteps, uint32_t frameInBlock)
{
    constexpr int kStepsPerBar = cfg::kStepsPerBeat * cfg::kBeatsPerBar;
    const AudioClip& k = aclips_[index];
    const Sample* s = bank_ ? bank_->get(k.slot) : nullptr;
    if (!s)
        return;
    if (!s->data) { // SPU2-only (.adp) sample: no PCM for the software path
        if (exporting())
            status_.exportSkipped = status_.exportSkipped + 1;
        return;
    }
    const uint32_t endFrame = (k.trimEnd && k.trimEnd < s->frames) ? k.trimEnd : s->frames;
    const uint32_t trimStart = k.trimStart < endFrame ? k.trimStart : 0;
    const bool loop = (k.flags & 1) != 0;
    const int route = (k.flags >> 1) & 0xf;

    // Join a clip that is already under way (play-from-bar).
    uint32_t start = trimStart;
    if (elapsedSteps > 0) {
        const uint64_t elapsedOut = framesForSteps(elapsedSteps);
        uint64_t off = elapsedOut * s->sampleRate / (uint64_t)cfg::kSampleRate;
        const uint64_t region = endFrame - trimStart;
        if (loop)
            off %= region;
        else if (off >= region)
            return;
        start = trimStart + (uint32_t)off;
    }
    const int remainingSteps = (k.lengthBars * kStepsPerBar) - elapsedSteps;
    const uint32_t gate = framesForSteps(remainingSteps);
    if (mixer_.triggerClip(s, route, k.volume, start, trimStart, endFrame, loop, gate))
        status_.audioClipsPlayed = status_.audioClipsPlayed + 1;
    (void)frameInBlock; // clip voices start at the current segment boundary, like pattern notes
}

void AudioEngine::triggerChannel(int ch, int velocity, uint32_t frameInBlock, int semis, int gateTicks)
{
    if (mixer_.instrument(ch).kind == (uint8_t)InstrKind::Synth) {
        // Synth notes always have a length: step hits sound for two steps.
        const uint32_t gate = framesForTicks(gateTicks > 0 ? gateTicks : 2 * cfg::kTicksPerStep);
        if (mixer_.triggerSynth(ch, pitch::kRootNote + semis, velocity, gate)) {
            status_.lastTriggerFrame[ch] = blockStart_ + frameInBlock;
            status_.triggerCount[ch] = status_.triggerCount[ch] + 1;
        }
        return;
    }
    if (triggerSample(ch, channelSample_[ch], velocity, channelMode_[ch], frameInBlock, semis, gateTicks)) {
        status_.lastTriggerFrame[ch] = blockStart_ + frameInBlock;
        status_.triggerCount[ch] = status_.triggerCount[ch] + 1;
    }
}

bool AudioEngine::triggerSample(int ch, int slot, int velocity, VoiceMode mode, uint32_t frameInBlock, int semis, int gateTicks)
{
    if (!bank_ || slot < 0 || slot >= cfg::kMaxSamples)
        return false;
    const Sample* s = bank_->get(slot);
    if (!s)
        return false;

    if (exporting() && !s->data)
        status_.exportSkipped = status_.exportSkipped + 1; // SPU2-only sample: cannot be rendered offline

    if (mode == VoiceMode::Spu2 && sampleHwReady_[slot] && !exporting()) {
        // Hardware voices bypass the software mixer, so inserts and effects do
        // not apply; the insert's fader, mute/solo are folded into the level.
        if (ch >= 0 && !mixer_.trackAudible(mixer_.strip(ch).route))
            return false;
        if (hwCount_ >= kMaxHwPerBlock) {
            status_.hwDropped = status_.hwDropped + 1;
            return false;
        }
        int32_t q15 = (int32_t)velocity * 32767 / 127;
        int pan = 0;
        if (ch >= 0) {
            q15 = (q15 * Mixer::volumeToQ15(mixer_.strip(ch).volume)) >> 15;
            pan = mixer_.strip(ch).pan;
            const int route = mixer_.strip(ch).route;
            if (route > 0) {
                q15 = (q15 * Mixer::volumeToQ15(mixer_.track(route).volume)) >> 15;
                pan += mixer_.track(route).pan;
                pan = pan < -100 ? -100 : (pan > 100 ? 100 : pan);
            }
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
    if (gated && gateTicks > 0) {
        gateFrames = framesForTicks(gateTicks);
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
    for (int t = 0; t < cfg::kMixBuses; ++t) {
        status_.busMeterL[t] = mixer_.trackMeterL(t);
        status_.busMeterR[t] = mixer_.trackMeterR(t);
        status_.busClip[t] = mixer_.trackClipped(t) ? 1 : 0;
        status_.busGr[t] = (int16_t)mixer_.trackGainReductionDeci(t);
    }
    uint8_t starved = 0;
    for (int t = 0; t < cfg::kMixBuses; ++t)
        for (int s = 0; s < cfg::kFxSlots; ++s)
            if (mixer_.fx(t, s).starved())
                starved |= 1;
    status_.fxStarved = starved;
    status_.voicesActive = (uint32_t)mixer_.activeVoices();
    status_.voiceSteals = mixer_.voiceSteals();
}
