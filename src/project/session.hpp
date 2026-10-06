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
    Session(AudioEngine& engine, const SampleBank& bank, bool (*waitFn)());

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
    void selectPattern(int pattern);

    // Channel strip
    void setVolume(int channel, int volume);
    void setPan(int channel, int pan);
    void setMute(int channel, bool mute);
    void setSolo(int channel, bool solo);
    void setSample(int channel, int slot);
    void setVoiceMode(int channel, VoiceMode mode);
    void setMasterVolume(int volume);

    // Auditioning
    void previewChannel(int channel);
    void previewSample(int slot, VoiceMode mode);

    // Platform -> engine: which samples have SPU2 copies.
    void setSampleHwReady(int slot, bool ready);

    uint32_t droppedCommands() const { return dropped_; }

private:
    void post(CmdType t, int a = 0, int b = 0, int c = 0, int32_t value = 0);
    void syncAll();
    int resolveSample(const ChannelData& c) const;

    AudioEngine& engine_;
    const SampleBank& bank_;
    bool (*wait_)();
    Project project_;
    uint32_t dropped_;
};
