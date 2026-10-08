// Session: the one place where the UI changes the song.
//
// Every edit updates the Project (UI-owned document) and mirrors the change
// into the AudioEngine as a Command. Views never touch the engine directly.
// This is the Ardour "session/model vs. realtime engine" split at PS2 scale.
#pragma once

#include <stdint.h>

#include "audio/audio_engine.hpp"
#include "audio/sample.hpp"
#include "project/project.hpp"

class Session {
public:
    // `waitFn` is called while the command queue is full; it should briefly
    // yield and return true, or return false if nobody is draining the queue
    // (audio not running), in which case the command is dropped instead of
    // hanging the UI. May be null (never wait).
    Session(AudioEngine& engine, SampleBank& bank, bool (*waitFn)());

    Project& project() { return project_; }
    const Project& project() const { return project_; }
    const SampleBank& bank() const { return bank_; }

    // Replaces the whole project and re-syncs the engine from scratch.
    void loadProject(const Project& p);
    // Re-sends the current project to the engine (e.g. after boot).
    void resync() { loadProject(project_); }

    // Transport
    void play();
    void pause();
    void stop();
    void togglePlayPause();
    void setBpmCenti(int bpmCenti);
    // Swing delays every odd 16th step by `percent` (0..50) of a step; saved with the project.
    void setSwing(int percent);
    // Click on every beat (louder on the bar) while playing. Not saved; never rendered into exports.
    void setMetronome(bool on);
    bool metronome() const { return metronome_; }

    // Pattern editing (pattern index is always explicit or the current one)
    void toggleStep(int channel, int step);
    void setStep(int pattern, int channel, int step, int velocity);
    void clearChannelSteps(int channel);
    void fillChannelEvery(int channel, int interval);
    void clearPattern(int pattern);
    void setPatternLength(int pattern, int steps);
    // Edits the pattern and, per switchMode(), takes the engine there at once
    // or at the next beat/bar while playing.
    void selectPattern(int pattern);
    SwitchMode switchMode() const { return switchMode_; }
    void setSwitchMode(SwitchMode m) { switchMode_ = m; }
    // Copies steps+length (not the name) from one pattern to another.
    bool copyPattern(int src, int dst);
    // Copies `src` into the first empty pattern and selects it. Returns the
    // new pattern index, or -1 when every pattern has content.
    int duplicatePattern(int src);
    bool patternIsEmpty(int pattern) const;
    void setPatternName(int pattern, const char* name);

    // Playlist. A clip plays `pattern` on `track` from `startBar` for
    // `lengthBars`. Clips on one track never overlap: placing over existing
    // clips replaces them. All of these return false (and change nothing)
    // for out-of-range arguments or when the clip table is full.
    bool placeClip(int track, int startBar, int pattern, int lengthBars, uint16_t chanMask = kAllChannels);
    bool removeClip(int index);
    // Ungrouping: a pattern clip can play only some of its pattern's instruments.
    // splitClipChannel moves one instrument out of the clip into a new clip (same pattern, bars
    // and length) on the first free track, so it can be moved, trimmed, muted or deleted on its own.
    // False when there is no free track, the clip table is full, or the clip plays only that instrument.
    bool splitClipChannel(int index, int channel);
    // Gives every instrument that has steps or notes in the pattern a clip of its own (the first
    // stays in the original clip). Returns how many new clips were made; it stops when the tracks run out.
    int ungroupClip(int index);
    // Restores a clip to play the whole pattern again.
    void setClipMask(int index, uint16_t mask);
    // True when the pattern has steps or piano-roll notes for the channel.
    bool channelHasContent(int pattern, int channel) const;
    // Grows/shrinks a clip, never into the next clip on its track or past the
    // song limit. Returns the resulting length (0 if the index is invalid).
    int setClipLength(int index, int lengthBars);
    void clearTrack(int track);
    void clearPlaylist();
    // Song mode plays the playlist; otherwise the current pattern loops.
    void setSongMode(bool on);
    bool songMode() const { return project_.songMode != 0; }
    // Pattern length in bars (ceil(steps / 16)), the natural clip length.
    int patternBars(int pattern) const;

    // Piano roll: polyphonic notes with pitch and length, per pattern and
    // channel, in addition to the step grid. A note at an existing (step, pitch)
    // is replaced. Pitch 60 is the sample's native speed. All return false and
    // change nothing for bad arguments or when a channel holds kMaxNotes notes.
    bool addNote(int pattern, int channel, int step, int pitch, int length, int velocity);
    bool removeNote(int pattern, int channel, int index);
    // Replaces the channel's whole note list (validated and clamped; at most kMaxNotes).
    // The piano roll's selection, paste, move, quantize and undo all go through this.
    bool setNotes(int pattern, int channel, const PianoNote* notes, int count);
    int noteIndexAt(int pattern, int channel, int step, int pitch) const;
    int setNoteLength(int pattern, int channel, int index, int length);
    void clearNotes(int pattern, int channel);
    // Shifts every note of the channel; refuses (false, nothing changed) if one would leave 0..127.
    bool transposeNotes(int pattern, int channel, int semitones);
    // Sustained instrument mode: note lengths cut the sample and notes layer.
    void setChannelGate(int channel, bool gate);
    // Playlist tracks
    void setTrackMute(int track, bool mute);
    void setTrackSolo(int track, bool solo);
    // Starts the song at `bar` (song mode) or plays normally otherwise.
    void playFromBar(int bar);

    // Rack channels (up to cfg::kMaxChannels). A new sampler channel gets the first
    // loaded sample, a synth channel the INIT SAW patch. Returns the index or -1 when full.
    int addChannel(bool synth);
    // Removes a channel; later channels (and their steps and notes) move up one row.
    // Re-syncs the whole song, so playback stops. False if it is the last channel.
    bool removeChannel(int channel);

    // Channel strip
    void setVolume(int channel, int volume);
    void setPan(int channel, int pan);
    void setMute(int channel, bool mute);
    void setSolo(int channel, bool solo);
    void setSample(int channel, int slot);
    // Re-resolves every channel's sampleRef against the bank (after a load
    // finished). Returns how many external references are still unresolved.
    int bindSamples();
    bool slotInUse(int slot) const;
    // Starts the release handshake for an imported sample nobody uses.
    // The memory is freed later by SampleBank::reap().
    bool releaseSample(int slot);
    // Retries release commands the engine queue could not take earlier.
    void pumpReleases();
    // Bumps on every loadProject(); the sample library watches it.
    uint32_t loadSerial() const { return loadSerial_; }
    void setVoiceMode(int channel, VoiceMode mode);
    void setMasterVolume(int volume);

    // ---- Mixer routing (Milestone 5) ----
    // Routes a rack channel to an insert (1..cfg::kMixTracks) or the master (0).
    // Any number of channels may share an insert.
    void setRoute(int channel, int track);
    void setMixVolume(int track, int volume);
    void setMixPan(int track, int pan);
    void setMixMute(int track, bool mute);
    void setMixSolo(int track, bool solo);
    void setMixerTrackName(int track, const char* name);
    void clearClipLatch();

    // ---- Effects (Milestone 6): chain slots 0..cfg::kFxSlots-1 on track 0 (master) .. 8 ----
    void setFxType(int track, int slot, FxType type);
    void setFxParam(int track, int slot, int index, int value);
    void setFxBypass(int track, int slot, bool bypass);
    // Swaps a slot with its neighbour (dir -1 / +1). Effect order is part of the project.
    bool moveFx(int track, int slot, int dir);

    // ---- Instruments: sampler envelope and native synthesizer ----
    void setChannelKind(int channel, InstrKind kind);
    void setEnvParam(int channel, int index, int value);
    void setSynthParam(int channel, int index, int value);
    // Switches the channel to the synthesizer and loads preset `i`.
    void applySynthPreset(int channel, int preset);
    void applyEnvPreset(int channel, int preset);

    // ---- Playlist audio clips ----
    // Places a bank sample on a playlist track. The length defaults to the
    // sample's duration at the current tempo (rounded up to bars, 1..32).
    // Returns the clip index, or -1 (track busy, table full, bad sample).
    int placeAudioClip(int track, int startBar, int sampleSlot, int lengthBars = 0);
    bool removeAudioClip(int index);
    // Moves a clip; refuses (false) when the target overlaps another clip.
    bool moveAudioClip(int index, int track, int startBar);
    int duplicateAudioClip(int index);  // new clip index, placed right after the original
    int setAudioClipLength(int index, int lengthBars);
    // Frames cut from the start / the region end (0 = whole sample). Clamped to the sample.
    bool setAudioClipTrim(int index, uint32_t startFrames, uint32_t endFrames);
    void setAudioClipVolume(int index, int volume);
    void setAudioClipLoop(int index, bool loop);
    void setAudioClipRoute(int index, int track);
    // Bank slot of an audio clip's sample (-1 if missing).
    int audioClipSlot(int index) const;
    void setPlaylistTrackName(int track, const char* name);

    // Offline rendering: with passes > 0 the engine renders software voices only
    // and stops (exportDone) after that many passes of the song (or pattern loop);
    // 0 returns to normal playback. The caller drives AudioEngine::render().
    void setExportMode(int passes);

    // Auditioning
    void previewChannel(int channel, int semis = 0);
    void previewSample(int slot, VoiceMode mode);

    // Platform -> engine: which samples have SPU2 copies.
    void setSampleHwReady(int slot, bool ready);

    uint32_t droppedCommands() const { return dropped_; }

private:
    bool post(CmdType t, int a = 0, int b = 0, int c = 0, int32_t value = 0);
    void syncAll();
    void syncClips();
    void syncChannel(int channel);
    void syncAudioClips();
    void syncAudioClip(int index);
    void syncInstrument(int channel);
    void syncMixer();
    void postFx(int track, int slot);
    int audioSourceFor(const Sample& s);
    void syncNotes(int pattern, int channel);
    void syncTrackMask();
    int resolveSample(const ChannelData& c) const;

    AudioEngine& engine_;
    SampleBank& bank_;
    bool (*wait_)();
    Project project_;
    uint32_t dropped_;
    uint32_t loadSerial_;
    SwitchMode switchMode_ = SwitchMode::Immediate;
    bool metronome_ = false;
    uint32_t releasePending_; // bitmask of slots whose Release command is not yet queued
};
