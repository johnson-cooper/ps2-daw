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
    bool placeClip(int track, int startBar, int pattern, int lengthBars);
    bool removeClip(int index);
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
    uint32_t releasePending_; // bitmask of slots whose Release command is not yet queued
};
