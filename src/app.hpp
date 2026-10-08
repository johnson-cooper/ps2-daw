// Application shell: owns every subsystem, runs the boot sequence and the UI
// loop, routes input to the active view and draws shared chrome.
//
// The UI loop is paced by vsync. Audio runs on its own thread (Ps2Audio), so
// a slow frame here can never stall sound or the sequencer clock.
#pragma once

#include "audio/audio_engine.hpp"
#include "audio/sample.hpp"
#include "core/status_log.hpp"
#include "platform/ps2_audio.hpp"
#include "platform/ps2_filesystem.hpp"
#include "platform/ps2_graphics.hpp"
#include "platform/ps2_input.hpp"
#include "platform/selftest_source.hpp"
#include "project/sample_library.hpp"
#include "project/session.hpp"
#include "ui/browser.hpp"
#include "audio/ram_export_file.hpp"
#include "audio/wav_export.hpp"
#include "platform/ps2_export_file.hpp"
#include "ui/channel_rack.hpp"
#include "ui/instrument_view.hpp"
#include "ui/mixer_view.hpp"
#include "ui/piano_roll.hpp"
#include "ui/playlist_view.hpp"
#include "ui/project_view.hpp"
#include "ui/view.hpp"
#include "ui/waveform.hpp"
#include "ui/widgets.hpp"

class App {
public:
    App();
    int run(int argc, char** argv);

private:
    void boot(int argc, char** argv);
    void drawBootScreen(const char* stage, bool waitingForKey);
    bool anyFailure() const;
    void frame();
    InputState applyStickNavigation(const InputState& raw);
    void handleGlobalKeys(const InputState& in);
    void drawHeader();
    void drawStatusBar(View& view);
    void serviceDebugMailbox(InputState& in);
    void publishTelemetry();
    static bool waitForAudio();
    static uint64_t profileClock();

    StatusLog log_;
    Gfx gfx_;
    Ps2Input input_;
    Ps2Audio audio_;
    Storage storage_;
    SampleBank bank_;
    AudioEngine engine_;
    Session session_;
    SelfTestSource testSource_;
    SampleLibrary library_;
    ui::ContextMenu menu_;
    UsbExportFile exportFile_;
    Ps2AudioHold audioHold_;
    Exporter exporter_;
    WaveformCache waves_;
    UiContext ctx_;

    ChannelRackView rack_;
    PianoRollView roll_;
    InstrumentView instrument_;
    PlaylistView playlist_;
    MixerView mixer_;
    BrowserView browser_;
    ProjectView projectView_;
    View* views_[(int)ViewId::Count];
    int current_ = 0;

    bool gfxOk_ = false;
    uint32_t fpsTimes10_ = 0;
    uint32_t frameUs_ = 0;
    uint32_t fpsFrames_ = 0;
    uint64_t fpsStartUs_ = 0;

    // Left-stick-as-D-pad repeat state.
    uint32_t stickDir_ = 0;
    uint32_t stickNextMs_ = 0;
    uint32_t seenLibraryMsg_ = 0;

    // Autosave: a changed project is written to PS2DAW/AUTOSAVE.ps2daw every
    // kAutosaveMs, so a crash or power loss costs at most a couple of minutes.
    void serviceAutosave();
    void buildStressProject();
    void benchFx();
    void startRamExport(bool song);
    void drawExportOverlay();
    RamExportFile* ramFile_ = nullptr;
    Exporter* ramExporter_ = nullptr;
    uint32_t autosaveCheckMs_ = 0;
    uint32_t autosaveLastMs_ = 0;
    uint32_t autosaveCrc_ = 0;
    bool autosaveHaveCrc_ = false;
    bool autosaveDirty_ = false;
    bool recoveryOffered_ = false;

    static App* instance_;
};
