#include "project/session.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/sample_ref.hpp"
#include "core/strutil.hpp"

static bool inRange(int v, int n) { return v >= 0 && v < n; }

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
    for (int i = 0; i < cfg::kMaxAudioSources; ++i)
        project_.audioSlot[i] = (int8_t)(project_.audioRefs[i][0] ? bank_.findByRef(project_.audioRefs[i]) : -1);
    ++loadSerial_;
    syncAll();
}

void Session::syncAll()
{
    const Project& p = project_;
    post(CmdType::SetBpm, 0, 0, 0, (int32_t)p.bpmCenti);
    post(CmdType::SetMasterVolume, 0, 0, 0, p.masterVolume);
    post(CmdType::SetSwing, 0, 0, 0, p.swing * cfg::kTicksPerStep / 100);
    post(CmdType::SetMetronome, 0, 0, 0, metronome_ ? 1 : 0);
    // Only active rack rows are synced (spare rows are re-synced when added), which keeps a
    // project load well inside the command queue.
    for (int ch = 0; ch < p.channelCount; ++ch) {
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
    syncClips();
    syncTrackMask();
    syncMixer();
    for (int ch = 0; ch < p.channelCount; ++ch) {
        syncInstrument(ch);
        post(CmdType::SetChannelRoute, ch, 0, 0, p.channels[ch].route);
    }
    syncAudioClips();
    for (int ch = 0; ch < p.channelCount; ++ch)
        post(CmdType::SetChannelGate, ch, 0, 0, p.channels[ch].gate);
    for (int pi = 0; pi < cfg::kMaxPatterns; ++pi)
        for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
            if (p.patterns[pi].noteCount[ch])
                syncNotes(pi, ch);
    post(CmdType::SetSongMode, 0, 0, 0, p.songMode);
    post(CmdType::SelectPattern, p.currentPattern); // immediate: the whole song was just replaced
}

static int32_t packNote(const PianoNote& n)
{
    return (int32_t)((uint32_t)n.step | ((uint32_t)n.pitch << 8) | ((uint32_t)n.velocity << 16) | ((uint32_t)n.length << 24));
}

void Session::syncNotes(int pattern, int channel)
{
    const PatternData& pd = project_.patterns[pattern];
    const int n = pd.noteCount[channel] > cfg::kMaxNotes ? cfg::kMaxNotes : pd.noteCount[channel];
    for (int i = 0; i < n; ++i) {
        const PianoNote& nt = pd.notes[channel][i];
        post(CmdType::SetNote, pattern, channel, i, packNote(nt));
        if (nt.tick || nt.lenTicks) // sub-step timing (SetNote resets it to whole steps)
            post(CmdType::SetNoteFine, pattern, channel, i, (int32_t)((uint32_t)nt.tick | ((uint32_t)nt.lenTicks << 8)));
    }
    post(CmdType::SetNoteCount, pattern, channel, 0, n);
}

void Session::syncTrackMask()
{
    post(CmdType::SetTrackMask, 0, 0, 0, (int32_t)(project_.trackMute | ((uint32_t)project_.trackSolo << 8)));
}

int Session::noteIndexAt(int pattern, int channel, int step, int pitch) const
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels))
        return -1;
    const PatternData& pd = project_.patterns[pattern];
    for (int i = 0; i < pd.noteCount[channel]; ++i)
        if (pd.notes[channel][i].step == step && pd.notes[channel][i].pitch == pitch)
            return i;
    return -1;
}

bool Session::addNote(int pattern, int channel, int step, int pitch, int length, int velocity)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels) || !inRange(step, cfg::kMaxSteps) ||
        !inRange(pitch, 128))
        return false;
    length = length < 1 ? 1 : (length > cfg::kMaxSteps ? cfg::kMaxSteps : length);
    velocity = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
    PatternData& pd = project_.patterns[pattern];
    int idx = noteIndexAt(pattern, channel, step, pitch);
    if (idx < 0) {
        if (pd.noteCount[channel] >= cfg::kMaxNotes)
            return false;
        idx = pd.noteCount[channel]++;
    }
    pd.notes[channel][idx].step = (uint8_t)step;
    pd.notes[channel][idx].pitch = (uint8_t)pitch;
    pd.notes[channel][idx].velocity = (uint8_t)velocity;
    pd.notes[channel][idx].length = (uint8_t)length;
    syncNotes(pattern, channel);
    return true;
}

bool Session::setNotes(int pattern, int channel, const PianoNote* notes, int count)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels) || count < 0 || count > cfg::kMaxNotes ||
        (count > 0 && !notes))
        return false;
    PatternData& pd = project_.patterns[pattern];
    int kept = 0;
    PianoNote out[cfg::kMaxNotes];
    for (int i = 0; i < count; ++i) {
        PianoNote n = notes[i];
        if (n.step >= cfg::kMaxSteps || n.pitch > 127)
            continue;
        n.velocity = (uint8_t)(n.velocity < 1 ? 1 : (n.velocity > 127 ? 127 : n.velocity));
        n.length = (uint8_t)(n.length < 1 ? 1 : (n.length > cfg::kMaxSteps ? cfg::kMaxSteps : n.length));
        if (n.tick >= cfg::kTicksPerStep)
            n.tick = 0;
        n.reserved = 0;
        if (n.lenTicks) { // keep `length` the whole-step ceiling of the exact duration
            if (n.lenTicks > cfg::kMaxSteps * cfg::kTicksPerStep)
                n.lenTicks = (uint16_t)(cfg::kMaxSteps * cfg::kTicksPerStep);
            n.length = (uint8_t)((n.lenTicks + cfg::kTicksPerStep - 1) / cfg::kTicksPerStep);
        }
        out[kept++] = n;
    }
    memset(pd.notes[channel], 0, sizeof(pd.notes[channel]));
    for (int i = 0; i < kept; ++i)
        pd.notes[channel][i] = out[i];
    pd.noteCount[channel] = (uint8_t)kept;
    syncNotes(pattern, channel);
    return true;
}

bool Session::removeNote(int pattern, int channel, int index)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels))
        return false;
    PatternData& pd = project_.patterns[pattern];
    if (!inRange(index, pd.noteCount[channel]))
        return false;
    for (int i = index; i + 1 < pd.noteCount[channel]; ++i)
        pd.notes[channel][i] = pd.notes[channel][i + 1];
    --pd.noteCount[channel];
    memset(&pd.notes[channel][pd.noteCount[channel]], 0, sizeof(PianoNote));
    syncNotes(pattern, channel);
    return true;
}

int Session::setNoteLength(int pattern, int channel, int index, int length)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels))
        return 0;
    PatternData& pd = project_.patterns[pattern];
    if (!inRange(index, pd.noteCount[channel]))
        return 0;
    PianoNote& n = pd.notes[channel][index];
    length = length < 1 ? 1 : (length > cfg::kMaxSteps ? cfg::kMaxSteps : length);
    n.length = (uint8_t)length;
    syncNotes(pattern, channel);
    return length;
}

void Session::clearNotes(int pattern, int channel)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels))
        return;
    memset(project_.patterns[pattern].notes[channel], 0, sizeof(project_.patterns[pattern].notes[channel]));
    project_.patterns[pattern].noteCount[channel] = 0;
    syncNotes(pattern, channel);
}

bool Session::transposeNotes(int pattern, int channel, int semitones)
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels))
        return false;
    PatternData& pd = project_.patterns[pattern];
    for (int i = 0; i < pd.noteCount[channel]; ++i) {
        const int np = pd.notes[channel][i].pitch + semitones;
        if (np < 0 || np > 127)
            return false;
    }
    for (int i = 0; i < pd.noteCount[channel]; ++i)
        pd.notes[channel][i].pitch = (uint8_t)(pd.notes[channel][i].pitch + semitones);
    syncNotes(pattern, channel);
    return true;
}

void Session::setChannelGate(int channel, bool gate)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    project_.channels[channel].gate = gate ? 1 : 0;
    post(CmdType::SetChannelGate, channel, 0, 0, gate ? 1 : 0);
}

void Session::setTrackMute(int track, bool mute)
{
    if (!inRange(track, cfg::kPlaylistTracks))
        return;
    project_.trackMute = (uint8_t)(mute ? (project_.trackMute | (1u << track)) : (project_.trackMute & ~(1u << track)));
    syncTrackMask();
}

void Session::setTrackSolo(int track, bool solo)
{
    if (!inRange(track, cfg::kPlaylistTracks))
        return;
    project_.trackSolo = (uint8_t)(solo ? (project_.trackSolo | (1u << track)) : (project_.trackSolo & ~(1u << track)));
    syncTrackMask();
}

void Session::playFromBar(int bar)
{
    post(CmdType::PlayFromBar, 0, 0, 0, bar < 0 ? 0 : bar);
}

void Session::syncClips()
{
    const Project& p = project_;
    for (int i = 0; i < p.clipCount; ++i) {
        const PlaylistClip& k = p.clips[i];
        post(CmdType::SetClip, i, k.track, k.pattern, (int32_t)(k.startBar | ((uint32_t)k.lengthBars << 16)));
        if (k.chanMask != kAllChannels && k.chanMask != 0)
            post(CmdType::SetClipMask, i, 0, 0, k.chanMask);
    }
    post(CmdType::SetClipCount, 0, 0, 0, p.clipCount);
}

int Session::patternBars(int pattern) const
{
    if (!inRange(pattern, cfg::kMaxPatterns))
        return 1;
    const int steps = project_.patterns[pattern].length;
    const int perBar = cfg::kStepsPerBeat * cfg::kBeatsPerBar;
    return (steps + perBar - 1) / perBar;
}

bool Session::placeClip(int track, int startBar, int pattern, int lengthBars, uint16_t chanMask)
{
    if (!inRange(track, cfg::kPlaylistTracks) || !inRange(startBar, cfg::kMaxSongBars) || !inRange(pattern, cfg::kMaxPatterns) ||
        lengthBars < 1)
        return false;
    if (startBar + lengthBars > cfg::kMaxSongBars)
        lengthBars = cfg::kMaxSongBars - startBar;
    Project& p = project_;
    // Make room: drop every clip on this track that overlaps the new one.
    PlaylistClip kept[Project::kMaxClips];
    int n = 0;
    for (int i = 0; i < p.clipCount; ++i) {
        const PlaylistClip& k = p.clips[i];
        const bool overlap = k.track == track && startBar < k.startBar + k.lengthBars && k.startBar < startBar + lengthBars;
        if (!overlap)
            kept[n++] = k;
    }
    if (n >= Project::kMaxClips)
        return false; // table full (checked before touching the project)
    // Audio clips under the new clip are replaced as well.
    bool audioRemoved = false;
    for (int i = p.audioClipCount - 1; i >= 0; --i) {
        const AudioClipData& k = p.audioClips[i];
        if (k.track == track && startBar < k.startBar + k.lengthBars && k.startBar < startBar + lengthBars) {
            for (int j = i; j + 1 < p.audioClipCount; ++j)
                p.audioClips[j] = p.audioClips[j + 1];
            --p.audioClipCount;
            memset(&p.audioClips[p.audioClipCount], 0, sizeof(AudioClipData));
            audioRemoved = true;
        }
    }
    if (audioRemoved)
        syncAudioClips();
    PlaylistClip nc;
    nc.track = (uint8_t)track;
    nc.pattern = (uint8_t)pattern;
    nc.startBar = (uint16_t)startBar;
    nc.lengthBars = (uint16_t)lengthBars;
    nc.chanMask = chanMask ? chanMask : kAllChannels;
    kept[n++] = nc;
    memcpy(p.clips, kept, sizeof(PlaylistClip) * (size_t)n);
    p.clipCount = (uint16_t)n;
    p.sanitizeClips(); // sorted order
    syncClips();
    return true;
}

bool Session::removeClip(int index)
{
    Project& p = project_;
    if (!inRange(index, p.clipCount))
        return false;
    for (int i = index; i + 1 < p.clipCount; ++i)
        p.clips[i] = p.clips[i + 1];
    --p.clipCount;
    memset(&p.clips[p.clipCount], 0, sizeof(PlaylistClip));
    syncClips();
    return true;
}

int Session::setClipLength(int index, int lengthBars)
{
    Project& p = project_;
    if (!inRange(index, p.clipCount))
        return 0;
    PlaylistClip& k = p.clips[index];
    int limit = cfg::kMaxSongBars - k.startBar;
    for (int i = 0; i < p.clipCount; ++i) {
        const PlaylistClip& o = p.clips[i];
        if (i != index && o.track == k.track && o.startBar >= k.startBar + 1 && o.startBar - k.startBar < limit)
            limit = o.startBar - k.startBar;
    }
    lengthBars = lengthBars < 1 ? 1 : (lengthBars > limit ? limit : lengthBars);
    k.lengthBars = (uint16_t)lengthBars;
    syncClips();
    return lengthBars;
}

void Session::clearTrack(int track)
{
    Project& p = project_;
    int n = 0;
    for (int i = 0; i < p.clipCount; ++i)
        if (p.clips[i].track != track)
            p.clips[n++] = p.clips[i];
    for (int i = n; i < p.clipCount; ++i)
        memset(&p.clips[i], 0, sizeof(PlaylistClip));
    p.clipCount = (uint16_t)n;
    syncClips();
}

void Session::clearPlaylist()
{
    memset(project_.clips, 0, sizeof(project_.clips));
    project_.clipCount = 0;
    syncClips();
}

void Session::setSongMode(bool on)
{
    project_.songMode = on ? 1 : 0;
    post(CmdType::SetSongMode, 0, 0, 0, on ? 1 : 0);
    // Leaving song mode: the engine goes back to looping the edited pattern.
    if (!on)
        post(CmdType::SelectPattern, project_.currentPattern);
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

void Session::setSwing(int percent)
{
    percent = percent < 0 ? 0 : (percent > 50 ? 50 : percent);
    project_.swing = (uint8_t)percent;
    post(CmdType::SetSwing, 0, 0, 0, percent * cfg::kTicksPerStep / 100);
}

void Session::setMetronome(bool on)
{
    metronome_ = on;
    post(CmdType::SetMetronome, 0, 0, 0, on ? 1 : 0);
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
    if (!on) {
        // A cell that only shows piano-roll notes starting here: clicking it removes them.
        PatternData& pd = project_.patterns[p];
        bool removed = false;
        for (int i = pd.noteCount[channel] - 1; i >= 0; --i)
            if (pd.notes[channel][i].step == step) {
                for (int k = i; k + 1 < pd.noteCount[channel]; ++k)
                    pd.notes[channel][k] = pd.notes[channel][k + 1];
                --pd.noteCount[channel];
                removed = true;
            }
        if (removed) {
            syncNotes(p, channel);
            return;
        }
    }
    setStep(p, channel, step, on ? 0 : cfg::kDefaultVelocity);
}

void Session::clearChannelSteps(int channel)
{
    const int p = project_.currentPattern;
    for (int s = 0; s < cfg::kMaxSteps; ++s)
        if (project_.patterns[p].velocity[channel][s])
            setStep(p, channel, s, 0);
    clearNotes(p, channel);
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
    memset(project_.patterns[pattern].noteCount, 0, sizeof(project_.patterns[pattern].noteCount));
    post(CmdType::ClearPattern, pattern); // the engine clears steps and notes together
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
    {
        if (pd.noteCount[ch])
            return false;
        for (int s = 0; s < cfg::kMaxSteps; ++s)
            if (pd.velocity[ch][s])
                return false;
    }
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
    memcpy(to.noteCount, from.noteCount, sizeof(to.noteCount));
    memcpy(to.notes, from.notes, sizeof(to.notes));
    post(CmdType::ClearPattern, dst);
    post(CmdType::SetPatternLength, dst, 0, 0, to.length);
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        for (int s = 0; s < cfg::kMaxSteps; ++s)
            if (to.velocity[ch][s])
                post(CmdType::SetStep, dst, ch, s, to.velocity[ch][s]);
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        syncNotes(dst, ch);
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
    bool audioChanged = false;
    for (int i = 0; i < cfg::kMaxAudioSources; ++i) {
        if (!project_.audioRefs[i][0])
            continue;
        const int slot = bank_.findByRef(project_.audioRefs[i]);
        if (slot != project_.audioSlot[i]) {
            project_.audioSlot[i] = (int8_t)slot;
            audioChanged = true;
        }
        if (slot < 0)
            ++unresolved;
    }
    if (audioChanged)
        syncAudioClips();
    return unresolved;
}

bool Session::slotInUse(int slot) const
{
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        if (project_.channels[ch].sampleSlot == slot)
            return true;
    for (int i = 0; i < project_.audioClipCount; ++i)
        if (project_.audioSlot[project_.audioClips[i].source] == slot)
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

void Session::previewChannel(int channel, int semis)
{
    if (inRange(channel, cfg::kMaxChannels))
        post(CmdType::PreviewChannel, channel, 0, 0, semis);
}

void Session::previewSample(int slot, VoiceMode mode)
{
    post(CmdType::PreviewSample, 0, (int)mode, 0, slot);
}

void Session::setSampleHwReady(int slot, bool ready)
{
    post(CmdType::SetSampleHwReady, ready ? 1 : 0, 0, 0, slot);
}

// ---------------------------------------------------------------------------
// Mixer routing, effects, instruments, audio clips
// ---------------------------------------------------------------------------

void Session::syncInstrument(int channel)
{
    const InstrumentData& in = project_.channels[channel].inst;
    post(CmdType::SetChannelKind, channel, 0, 0, in.kind);
    for (int i = 0; i < cfg::kEnvParams; ++i)
        post(CmdType::SetEnvParam, channel, i, 0, in.env[i]);
    for (int i = 0; i < instr::synthParamCount(); ++i)
        post(CmdType::SetSynthParam, channel, i, 0, in.synth[i]);
}

void Session::postFx(int track, int slot)
{
    const FxData& f = project_.tracks[track].fx[slot];
    post(CmdType::SetFxType, track, slot, 0, f.type);
    const int n = fx::paramCount((FxType)f.type);
    for (int i = 0; i < n; ++i)
        post(CmdType::SetFxParam, track, slot, i, f.p[i]);
    post(CmdType::SetFxBypass, track, slot, 0, f.bypass);
}

void Session::syncMixer()
{
    for (int t = 0; t < cfg::kMixBuses; ++t) {
        const MixerTrackData& m = project_.tracks[t];
        if (t > 0) {
            post(CmdType::SetTrackParam, t, 0, 0, m.volume);
            post(CmdType::SetTrackParam, t, 1, 0, m.pan);
            post(CmdType::SetTrackParam, t, 2, 0, m.mute);
            post(CmdType::SetTrackParam, t, 3, 0, m.solo);
        }
        for (int s = 0; s < cfg::kFxSlots; ++s)
            postFx(t, s);
    }
}

void Session::setRoute(int channel, int track)
{
    if (!inRange(channel, cfg::kMaxChannels) || !inRange(track, cfg::kMixBuses))
        return;
    project_.channels[channel].route = (uint8_t)track;
    post(CmdType::SetChannelRoute, channel, 0, 0, track);
}

void Session::setMixVolume(int track, int volume)
{
    if (!inRange(track, cfg::kMixBuses) || track == 0)
        return;
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    project_.tracks[track].volume = (uint8_t)volume;
    post(CmdType::SetTrackParam, track, 0, 0, volume);
}

void Session::setMixPan(int track, int pan)
{
    if (!inRange(track, cfg::kMixBuses) || track == 0)
        return;
    pan = pan < -100 ? -100 : (pan > 100 ? 100 : pan);
    project_.tracks[track].pan = (int8_t)pan;
    post(CmdType::SetTrackParam, track, 1, 0, pan);
}

void Session::setMixMute(int track, bool mute)
{
    if (!inRange(track, cfg::kMixBuses) || track == 0)
        return;
    project_.tracks[track].mute = mute ? 1 : 0;
    post(CmdType::SetTrackParam, track, 2, 0, mute ? 1 : 0);
}

void Session::setMixSolo(int track, bool solo)
{
    if (!inRange(track, cfg::kMixBuses) || track == 0)
        return;
    project_.tracks[track].solo = solo ? 1 : 0;
    post(CmdType::SetTrackParam, track, 3, 0, solo ? 1 : 0);
}

void Session::setMixerTrackName(int track, const char* name)
{
    if (!inRange(track, cfg::kMixBuses) || !name)
        return;
    str::copy(project_.tracks[track].name, sizeof(project_.tracks[track].name), name);
}

void Session::clearClipLatch() { post(CmdType::ClearClipLatch); }

void Session::setFxType(int track, int slot, FxType type)
{
    if (!inRange(track, cfg::kMixBuses) || !inRange(slot, cfg::kFxSlots) || (int)type >= kFxTypeCount)
        return;
    fx::setDefaults(project_.tracks[track].fx[slot], type);
    postFx(track, slot);
}

void Session::setFxParam(int track, int slot, int index, int value)
{
    if (!inRange(track, cfg::kMixBuses) || !inRange(slot, cfg::kFxSlots))
        return;
    FxData& f = project_.tracks[track].fx[slot];
    if (!inRange(index, fx::paramCount((FxType)f.type)))
        return;
    f.p[index] = (int16_t)params::clampTo(fx::param((FxType)f.type, index), value);
    post(CmdType::SetFxParam, track, slot, index, f.p[index]);
}

void Session::setFxBypass(int track, int slot, bool bypass)
{
    if (!inRange(track, cfg::kMixBuses) || !inRange(slot, cfg::kFxSlots))
        return;
    project_.tracks[track].fx[slot].bypass = bypass ? 1 : 0;
    post(CmdType::SetFxBypass, track, slot, 0, bypass ? 1 : 0);
}

bool Session::moveFx(int track, int slot, int dir)
{
    const int other = slot + dir;
    if (!inRange(track, cfg::kMixBuses) || !inRange(slot, cfg::kFxSlots) || !inRange(other, cfg::kFxSlots))
        return false;
    FxData tmp = project_.tracks[track].fx[slot];
    project_.tracks[track].fx[slot] = project_.tracks[track].fx[other];
    project_.tracks[track].fx[other] = tmp;
    postFx(track, slot);
    postFx(track, other);
    return true;
}

void Session::setChannelKind(int channel, InstrKind kind)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    project_.channels[channel].inst.kind = (uint8_t)kind;
    post(CmdType::SetChannelKind, channel, 0, 0, (int)kind);
    if (kind == InstrKind::Synth) // synth notes always have a length
        setChannelGate(channel, true);
}

void Session::setEnvParam(int channel, int index, int value)
{
    if (!inRange(channel, cfg::kMaxChannels) || !inRange(index, cfg::kEnvParams))
        return;
    InstrumentData& in = project_.channels[channel].inst;
    in.env[index] = (int16_t)params::clampTo(instr::envParam(index), value);
    post(CmdType::SetEnvParam, channel, index, 0, in.env[index]);
}

void Session::setSynthParam(int channel, int index, int value)
{
    if (!inRange(channel, cfg::kMaxChannels) || !inRange(index, instr::synthParamCount()))
        return;
    InstrumentData& in = project_.channels[channel].inst;
    in.synth[index] = (int16_t)params::clampTo(instr::synthParam(index), value);
    post(CmdType::SetSynthParam, channel, index, 0, in.synth[index]);
}

void Session::applySynthPreset(int channel, int preset)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    instr::applySynthPreset(project_.channels[channel].inst, preset);
    str::copy(project_.channels[channel].name, sizeof(project_.channels[channel].name), instr::synthPreset(preset).name);
    syncInstrument(channel);
    setChannelKind(channel, InstrKind::Synth);
}

void Session::applyEnvPreset(int channel, int preset)
{
    if (!inRange(channel, cfg::kMaxChannels))
        return;
    instr::applyEnvPreset(project_.channels[channel].inst, preset);
    for (int i = 0; i < cfg::kEnvParams; ++i)
        post(CmdType::SetEnvParam, channel, i, 0, project_.channels[channel].inst.env[i]);
}

// ---- audio clips ----

int Session::audioClipSlot(int index) const
{
    if (!inRange(index, project_.audioClipCount))
        return -1;
    return project_.audioSlot[project_.audioClips[index].source];
}

void Session::syncAudioClip(int i)
{
    const AudioClipData& k = project_.audioClips[i];
    const int slot = project_.audioSlot[k.source];
    post(CmdType::SetAudioClip, i, k.track, slot >= 0 ? slot : 0xff, (int32_t)(k.startBar | ((uint32_t)k.lengthBars << 16)));
    post(CmdType::SetAudioClipMix, i, k.volume * 127 / 100, (k.loop ? 1 : 0) | (k.route << 1), 0);
    post(CmdType::SetAudioClipTrim, i, 0, 0, (int32_t)k.trimStart);
    post(CmdType::SetAudioClipTrim, i, 1, 0, (int32_t)k.trimEnd);
}

void Session::syncAudioClips()
{
    for (int i = 0; i < project_.audioClipCount; ++i)
        syncAudioClip(i);
    post(CmdType::SetAudioClipCount, 0, 0, 0, project_.audioClipCount);
}

int Session::audioSourceFor(const Sample& s)
{
    Project& p = project_;
    for (int i = 0; i < cfg::kMaxAudioSources; ++i)
        if (p.audioRefs[i][0] && strcmp(p.audioRefs[i], s.ref) == 0)
            return i;
    // Free entry: empty, or not used by any clip.
    for (int i = 0; i < cfg::kMaxAudioSources; ++i) {
        bool used = false;
        for (int k = 0; k < p.audioClipCount; ++k)
            used |= p.audioClips[k].source == i;
        if (!p.audioRefs[i][0] || !used) {
            str::copy(p.audioRefs[i], sizeof(p.audioRefs[i]), s.ref);
            p.audioSlot[i] = -1;
            return i;
        }
    }
    return -1;
}

int Session::placeAudioClip(int track, int startBar, int sampleSlot, int lengthBars)
{
    Project& p = project_;
    const Sample* s = bank_.get(sampleSlot);
    if (!s || s->hwOnly || !s->data || !inRange(track, cfg::kPlaylistTracks) || !inRange(startBar, cfg::kMaxSongBars) ||
        p.audioClipCount >= cfg::kMaxAudioClips)
        return -1;
    if (lengthBars < 1) {
        // beats = seconds * bpm / 60; 4 beats per bar
        const uint64_t num = (uint64_t)s->frames * p.bpmCenti;
        const uint64_t den = (uint64_t)(s->sampleRate ? s->sampleRate : 48000) * 6000ull * cfg::kBeatsPerBar;
        lengthBars = (int)((num + den - 1) / den);
        if (lengthBars < 1)
            lengthBars = 1;
        if (lengthBars > 32)
            lengthBars = 32;
    }
    if (startBar + lengthBars > cfg::kMaxSongBars)
        lengthBars = cfg::kMaxSongBars - startBar;
    if (!p.trackFree(track, startBar, lengthBars))
        return -1;
    const int src = audioSourceFor(*s);
    if (src < 0)
        return -1;
    p.audioSlot[src] = (int8_t)sampleSlot;
    const int i = p.audioClipCount++;
    AudioClipData& k = p.audioClips[i];
    memset(&k, 0, sizeof(k));
    k.track = (uint8_t)track;
    k.source = (uint8_t)src;
    k.startBar = (uint16_t)startBar;
    k.lengthBars = (uint16_t)lengthBars;
    k.volume = 100;
    syncAudioClip(i);
    post(CmdType::SetAudioClipCount, 0, 0, 0, p.audioClipCount);
    return i;
}

bool Session::removeAudioClip(int index)
{
    Project& p = project_;
    if (!inRange(index, p.audioClipCount))
        return false;
    for (int i = index; i + 1 < p.audioClipCount; ++i)
        p.audioClips[i] = p.audioClips[i + 1];
    --p.audioClipCount;
    memset(&p.audioClips[p.audioClipCount], 0, sizeof(AudioClipData));
    syncAudioClips();
    return true;
}

bool Session::moveAudioClip(int index, int track, int startBar)
{
    Project& p = project_;
    if (!inRange(index, p.audioClipCount) || !inRange(track, cfg::kPlaylistTracks) || !inRange(startBar, cfg::kMaxSongBars))
        return false;
    AudioClipData& k = p.audioClips[index];
    int len = k.lengthBars;
    if (startBar + len > cfg::kMaxSongBars)
        return false;
    if (!p.trackFree(track, startBar, len, -1, index))
        return false;
    k.track = (uint8_t)track;
    k.startBar = (uint16_t)startBar;
    syncAudioClip(index);
    return true;
}

int Session::duplicateAudioClip(int index)
{
    Project& p = project_;
    if (!inRange(index, p.audioClipCount) || p.audioClipCount >= cfg::kMaxAudioClips)
        return -1;
    const AudioClipData src = p.audioClips[index];
    // Right after the original; if that spot is taken, the first free spot on the track.
    for (int start = src.startBar + src.lengthBars; start + src.lengthBars <= cfg::kMaxSongBars; ++start) {
        if (!p.trackFree(src.track, start, src.lengthBars))
            continue;
        const int i = p.audioClipCount++;
        p.audioClips[i] = src;
        p.audioClips[i].startBar = (uint16_t)start;
        syncAudioClip(i);
        post(CmdType::SetAudioClipCount, 0, 0, 0, p.audioClipCount);
        return i;
    }
    return -1;
}

int Session::setAudioClipLength(int index, int lengthBars)
{
    Project& p = project_;
    if (!inRange(index, p.audioClipCount))
        return 0;
    AudioClipData& k = p.audioClips[index];
    if (lengthBars < 1)
        lengthBars = 1;
    if (k.startBar + lengthBars > cfg::kMaxSongBars)
        lengthBars = cfg::kMaxSongBars - k.startBar;
    while (lengthBars > k.lengthBars && !p.trackFree(k.track, k.startBar, lengthBars, -1, index))
        --lengthBars;
    k.lengthBars = (uint16_t)lengthBars;
    syncAudioClip(index);
    return lengthBars;
}

bool Session::setAudioClipTrim(int index, uint32_t startFrames, uint32_t endFrames)
{
    Project& p = project_;
    if (!inRange(index, p.audioClipCount))
        return false;
    AudioClipData& k = p.audioClips[index];
    const Sample* s = bank_.get(p.audioSlot[k.source]);
    const uint32_t total = s ? s->frames : 0xffffffffu;
    if (endFrames > total)
        endFrames = total;
    if (startFrames >= total)
        startFrames = total ? total - 1 : 0;
    if (endFrames && endFrames <= startFrames)
        endFrames = 0;
    if (s && endFrames == total)
        endFrames = 0; // the whole sample: stored as "to the end"
    k.trimStart = startFrames;
    k.trimEnd = endFrames;
    post(CmdType::SetAudioClipTrim, index, 0, 0, (int32_t)k.trimStart);
    post(CmdType::SetAudioClipTrim, index, 1, 0, (int32_t)k.trimEnd);
    return true;
}

void Session::setAudioClipVolume(int index, int volume)
{
    if (!inRange(index, project_.audioClipCount))
        return;
    project_.audioClips[index].volume = (uint8_t)(volume < 0 ? 0 : (volume > 100 ? 100 : volume));
    syncAudioClip(index);
}

void Session::setAudioClipLoop(int index, bool loop)
{
    if (!inRange(index, project_.audioClipCount))
        return;
    project_.audioClips[index].loop = loop ? 1 : 0;
    syncAudioClip(index);
}

void Session::setAudioClipRoute(int index, int track)
{
    if (!inRange(index, project_.audioClipCount) || !inRange(track, cfg::kMixBuses))
        return;
    project_.audioClips[index].route = (uint8_t)track;
    syncAudioClip(index);
}

void Session::setPlaylistTrackName(int track, const char* name)
{
    if (!inRange(track, cfg::kPlaylistTracks) || !name)
        return;
    str::copy(project_.playlistTrackName[track], sizeof(project_.playlistTrackName[track]), name);
}

void Session::setExportMode(int passes)
{
    post(CmdType::SetExportMode, 0, 0, 0, passes < 0 ? 0 : passes);
}

// ---- rack channels ----

void Session::syncChannel(int ch)
{
    const ChannelData& c = project_.channels[ch];
    post(CmdType::SetChannelSample, ch, 0, 0, c.sampleSlot);
    post(CmdType::SetChannelVolume, ch, 0, 0, c.volume);
    post(CmdType::SetChannelPan, ch, 0, 0, c.pan);
    post(CmdType::SetChannelMute, ch, 0, 0, c.mute);
    post(CmdType::SetChannelSolo, ch, 0, 0, c.solo);
    post(CmdType::SetChannelVoiceMode, ch, 0, 0, c.voiceMode);
    syncInstrument(ch);
    post(CmdType::SetChannelRoute, ch, 0, 0, c.route);
    post(CmdType::SetChannelGate, ch, 0, 0, c.gate);
}

int Session::addChannel(bool synth)
{
    Project& p = project_;
    if (p.channelCount >= cfg::kMaxChannels)
        return -1;
    const int ch = p.channelCount;
    ChannelData& c = p.channels[ch];
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "CH%d", ch + 1);
    c.sampleSlot = -1;
    c.volume = 78;
    instr::setDefaults(c.inst);
    // A new row starts empty in every pattern.
    for (auto& pd : p.patterns) {
        memset(pd.velocity[ch], 0, sizeof(pd.velocity[ch]));
        memset(pd.notes[ch], 0, sizeof(pd.notes[ch]));
        pd.noteCount[ch] = 0;
    }
    ++p.channelCount;
    // (the engine's grid for a spare row is already empty: syncAll and removeChannel clear it)
    if (synth) {
        instr::applySynthPreset(c.inst, 0);
        c.inst.kind = (uint8_t)InstrKind::Synth;
        c.gate = 1;
        str::copy(c.name, sizeof(c.name), "SYNTH");
        syncChannel(ch);
    } else {
        // first loaded sample, so the new row makes a sound straight away
        for (int slot = 0; slot < cfg::kMaxSamples; ++slot)
            if (bank_.get(slot)) {
                syncChannel(ch);
                setSample(ch, slot);
                return ch;
            }
        syncChannel(ch);
    }
    return ch;
}

bool Session::removeChannel(int ch)
{
    Project& p = project_;
    if (!inRange(ch, p.channelCount) || p.channelCount <= 1)
        return false;
    for (int i = ch; i + 1 < p.channelCount; ++i)
        p.channels[i] = p.channels[i + 1];
    for (auto& pd : p.patterns) {
        for (int i = ch; i + 1 < p.channelCount; ++i) {
            memcpy(pd.velocity[i], pd.velocity[i + 1], sizeof(pd.velocity[i]));
            memcpy(pd.notes[i], pd.notes[i + 1], sizeof(pd.notes[i]));
            pd.noteCount[i] = pd.noteCount[i + 1];
        }
        const int last = p.channelCount - 1;
        memset(pd.velocity[last], 0, sizeof(pd.velocity[last]));
        memset(pd.notes[last], 0, sizeof(pd.notes[last]));
        pd.noteCount[last] = 0;
    }
    const int last = p.channelCount - 1;
    memset(&p.channels[last], 0, sizeof(ChannelData));
    snprintf(p.channels[last].name, sizeof(p.channels[last].name), "CH%d", last + 1);
    p.channels[last].sampleSlot = -1;
    p.channels[last].volume = 78;
    instr::setDefaults(p.channels[last].inst);
    --p.channelCount;
    loadProject(p); // the engine learns the new layout in one consistent sweep
    return true;
}

// ---- ungrouping pattern clips ----

bool Session::channelHasContent(int pattern, int channel) const
{
    if (!inRange(pattern, cfg::kMaxPatterns) || !inRange(channel, cfg::kMaxChannels))
        return false;
    const PatternData& pd = project_.patterns[pattern];
    if (pd.noteCount[channel])
        return true;
    for (int s = 0; s < cfg::kMaxSteps; ++s)
        if (pd.velocity[channel][s])
            return true;
    return false;
}

void Session::setClipMask(int index, uint16_t mask)
{
    if (!inRange(index, project_.clipCount))
        return;
    project_.clips[index].chanMask = mask ? mask : kAllChannels;
    syncClips();
}

// Appends a clip like `base` that plays only `mask` on `track`.
static bool appendMaskedClip(Project& p, const PlaylistClip& base, int track, uint16_t mask)
{
    if (p.clipCount >= Project::kMaxClips)
        return false;
    PlaylistClip nc = base;
    nc.track = (uint8_t)track;
    nc.chanMask = mask;
    p.clips[p.clipCount++] = nc;
    return true;
}

static int firstFreeTrack(const Project& p, int exceptTrack, int start, int len)
{
    for (int t = 0; t < cfg::kPlaylistTracks; ++t)
        if (t != exceptTrack && p.trackFree(t, start, len))
            return t;
    return -1;
}

bool Session::splitClipChannel(int index, int channel)
{
    Project& p = project_;
    if (!inRange(index, p.clipCount) || !inRange(channel, cfg::kMaxChannels))
        return false;
    const PlaylistClip base = p.clips[index];
    const uint16_t bit = (uint16_t)(1u << channel);
    if (!(base.chanMask & bit) || (base.chanMask & ~bit) == 0)
        return false;
    const int t = firstFreeTrack(p, base.track, base.startBar, base.lengthBars);
    if (t < 0 || !appendMaskedClip(p, base, t, bit))
        return false;
    p.clips[index].chanMask = (uint16_t)(base.chanMask & ~bit);
    p.sanitizeClips(); // keeps the table sorted (indices change)
    syncClips();
    return true;
}

int Session::ungroupClip(int index)
{
    Project& p = project_;
    if (!inRange(index, p.clipCount))
        return 0;
    const PlaylistClip base = p.clips[index];
    int made = 0;
    uint16_t moved = 0;
    bool keptFirst = false;
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        if (!((base.chanMask >> ch) & 1) || !channelHasContent(base.pattern, ch))
            continue;
        if (!keptFirst) { // the first instrument stays in the original clip
            keptFirst = true;
            continue;
        }
        const int t = firstFreeTrack(p, base.track, base.startBar, base.lengthBars);
        if (t < 0 || !appendMaskedClip(p, base, t, (uint16_t)(1u << ch)))
            break;
        moved |= (uint16_t)(1u << ch);
        ++made;
    }
    if (made) {
        p.clips[index].chanMask = (uint16_t)(base.chanMask & ~moved);
        p.sanitizeClips();
        syncClips();
    }
    return made;
}
