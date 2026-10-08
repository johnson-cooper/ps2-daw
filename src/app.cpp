#include "app.hpp"

#include <debug.h>
#include <kernel.h>
#include <stdio.h>
#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "audio/drum_synth.hpp"
#include "core/strutil.hpp"
#include "platform/debug_mailbox.hpp"
#include "platform/ps2_system.hpp"
#include "project/project_io.hpp"
#include "project/slot_store.hpp"
#include "ui/debug_overlay.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"
#include "version.hpp"

App* App::instance_ = nullptr;

App::App()
    : session_(engine_, bank_, &App::waitForAudio),
      testSource_(storage_),
      library_(session_, bank_, testSource_, &audio_, log_),
      exportFile_(storage_),
      audioHold_(audio_),
      exporter_(engine_, session_, exportFile_, &audioHold_),
      ctx_{session_, engine_, audio_, storage_, log_, bank_, menu_, library_, exporter_, waves_}
{
    instance_ = this;
    views_[(int)ViewId::ChannelRack] = &rack_;
    views_[(int)ViewId::PianoRoll] = &roll_;
    views_[(int)ViewId::Instrument] = &instrument_;
    views_[(int)ViewId::Playlist] = &playlist_;
    views_[(int)ViewId::Mixer] = &mixer_;
    views_[(int)ViewId::Browser] = &browser_;
    views_[(int)ViewId::Project] = &projectView_;
}

uint64_t App::profileClock()
{
    return ps2sys::timeUs();
}

bool App::waitForAudio()
{
    // Called by Session when the command queue is full. Only worth waiting
    // if the render thread is actually draining it.
    if (!instance_ || !instance_->audio_.stats().streaming)
        return false;
    ps2sys::sleepUs(1000);
    return true;
}

bool App::anyFailure() const
{
    for (int i = 0; i < (int)Subsystem::Count; ++i)
        if (log_.entry((Subsystem)i).health == Health::Failed)
            return true;
    return false;
}

void App::drawBootScreen(const char* stage, bool waitingForKey)
{
    if (!gfxOk_)
        return;
    gfx_.beginFrame(theme::kBackground);
    gfx_.text(theme::kSafeLeft, 24, G_BLOCK " " PS2DAW_NAME, theme::kAccent, 4, 4);
    gfx_.text(theme::kSafeLeft, 64, "version " PS2DAW_VERSION "   built " PS2DAW_BUILD_DATE, theme::kTextDim, 1, 2);

    int y = 96;
    for (int i = 0; i < (int)Subsystem::Count; ++i, y += 24) {
        const StatusLog::Entry& e = log_.entry((Subsystem)i);
        uint32_t c = theme::kTextDim;
        if (e.health == Health::Ok)
            c = theme::kOk;
        else if (e.health == Health::Failed)
            c = theme::kError;
        else if (e.health == Health::Warning)
            c = theme::kWarning;
        char line[96];
        snprintf(line, sizeof(line), "[%s] %-11s %s", StatusLog::healthName(e.health), StatusLog::name((Subsystem)i), e.detail);
        gfx_.text(theme::kSafeLeft, y, line, c, 1, 2);
    }
    gfx_.text(theme::kSafeLeft, y + 8, stage, theme::kText, 2, 2);
    if (waitingForKey)
        gfx_.text(theme::kSafeLeft, y + 32, "A subsystem failed. Press " G_CROSS " to continue anyway.", theme::kError, 1, 2);
    gfx_.endFrame();
}

void App::boot(int argc, char** argv)
{
    // 1. IOP: reset and patch before anything else talks to it.
    ps2sys::resetIop(log_);

    // 2. Graphics: first, so every later failure can be shown on screen.
    gfxOk_ = gfx_.init(log_);
    drawBootScreen("Starting controller...", false);

    // 3. Controller.
    input_.init(log_);
    drawBootScreen("Starting audio driver...", false);

    // 4. Audio driver + stream.
    const bool audioOk = audio_.init(log_);
    drawBootScreen("Generating built-in sounds...", false);

    // 5. Built-in kit (also the test sounds), then sync the demo song.
    const int made = drumsynth::generateKit(bank_);
    if (made == (int)drumsynth::Kind::Count)
        log_.set(Subsystem::Samples, Health::Ok, "%d built-in sounds, %lu KiB", made, (unsigned long)(bank_.bytesUsed() / 1024));
    else
        log_.fail(Subsystem::Samples, "only %d/%d sounds generated (out of memory?)", made, (int)drumsynth::Kind::Count);
    engine_.setSampleBank(&bank_);
    session_.resync();

    // 6. SPU2 copies of the kit.
    if (audioOk) {
        drawBootScreen("Uploading sounds to SPU2 RAM...", false);
        audio_.uploadHardwareSamples(bank_, session_, log_);
    } else {
        log_.set(Subsystem::Spu2, Health::Skipped, "audio driver not running");
    }

    // 7. Give the pad ~0.6 s to come up; holding SELECT skips USB storage
    //    (recovery path if a particular drive hangs the USB stack). Storage
    //    loads before the audio thread starts so module loading cannot steal
    //    IOP time from a running stream.
    bool skipUsb = false;
    for (int i = 0; i < 36; ++i) {
        input_.update((uint32_t)(ps2sys::timeUs() / 1000), log_);
        if (input_.state().down(btn::Select))
            skipUsb = true;
        drawBootScreen(skipUsb ? "SELECT held: USB storage will be skipped" : "Hold SELECT now to skip USB storage", false);
    }
    drawBootScreen("Loading USB storage drivers...", false);
    storage_.init(log_, skipUsb, argc > 0 ? argv[0] : "");

    // Realtime audio thread last: from here on it preempts the UI.
    engine_.setProfileClock(&App::profileClock);
    if (audioOk)
        audio_.start(engine_, log_);

    // 8. If anything failed, keep the report on screen until acknowledged.
    //    Times out after 20 s so a dead controller cannot trap the screen.
    if (anyFailure() && gfxOk_) {
        const uint64_t until = ps2sys::timeUs() + 20000000ull;
        while (ps2sys::timeUs() < until) {
            input_.update((uint32_t)(ps2sys::timeUs() / 1000), log_);
            if (input_.state().hit(btn::Cross) || input_.state().hit(btn::Start))
                break;
            drawBootScreen("Boot finished with errors.", true);
        }
    }
    log_.set(Subsystem::Project, Health::Ok, "%s", session_.project().name);
    ctx_.toast("Ready. START plays the demo beat.");
}

InputState App::applyStickNavigation(const InputState& raw)
{
    InputState in = raw;
    uint32_t dir = 0;
    const int ax = raw.lx < 0 ? -raw.lx : raw.lx, ay = raw.ly < 0 ? -raw.ly : raw.ly;
    if (ax >= 64 && ax >= ay)
        dir = raw.lx < 0 ? btn::Left : btn::Right;
    else if (ay >= 64)
        dir = raw.ly < 0 ? btn::Up : btn::Down;

    if (!dir) {
        stickDir_ = 0;
        return in;
    }
    const uint32_t now = ctx_.nowMs;
    const int mag = ax > ay ? ax : ay; // 64..127
    const uint32_t interval = 200 - (uint32_t)(mag - 64) * 150 / 63; // 200 ms .. 50 ms
    if (dir != stickDir_) {
        stickDir_ = dir;
        stickNextMs_ = now + 300;
        in.repeat |= dir;
    } else if ((int32_t)(now - stickNextMs_) >= 0) {
        stickNextMs_ = now + interval;
        in.repeat |= dir;
    }
    return in;
}

void App::handleGlobalKeys(const InputState& in)
{
    if (in.hit(btn::Start))
        session_.togglePlayPause();
    if (in.hit(btn::L3)) {
        session_.stop();
        ctx_.toast("Stopped");
    }
    if (in.hit(btn::R3))
        ctx_.debugOverlay = !ctx_.debugOverlay;
    if (in.hit(btn::Select)) {
        current_ = (current_ + 1) % (int)ViewId::Count;
        views_[current_]->onEnter(ctx_);
    }
}

void App::drawHeader()
{
    const EngineStatus& es = ctx_.engine.status();
    const int y = theme::kSafeTop;
    gfx_.fillRect(0, 0, Gfx::kWidth, 44, theme::kPanelDark);
    gfx_.text(theme::kSafeLeft, y + 4, G_BLOCK " PS2 DAW", theme::kAccent);

    const bool playing = es.transport == (uint8_t)Transport::State::Playing;
    const bool paused = es.transport == (uint8_t)Transport::State::Paused;
    const char* state = playing ? G_PLAY " PLAY" : (paused ? G_PAUSE " PAUSE" : G_STOP " STOP");
    gfx_.text(150, y + 4, state, playing ? theme::kAlive : (paused ? theme::kWarning : theme::kTextDim));

    const uint32_t bpm = session_.project().bpmCenti;
    gfx_.textf(250, y + 4, theme::kText, "%3lu.%02lu BPM", (unsigned long)(bpm / 100), (unsigned long)(bpm % 100));

    const int step = ctx_.heardStep();
    const int songStep = ctx_.heardSongStep();
    char pos[32];
    if (songStep >= 0) {
        const int perBar = cfg::kStepsPerBeat * cfg::kBeatsPerBar;
        snprintf(pos, sizeof(pos), "BAR %d.%d", songStep / perBar + 1, (songStep % perBar) / cfg::kStepsPerBeat + 1);
    } else if (step >= 0)
    {
        if (es.queuedPattern != 0xff)
            snprintf(pos, sizeof(pos), "P%d>%d %d.%d", es.pattern + 1, es.queuedPattern + 1, step / 4 + 1, step % 4 + 1);
        else
            snprintf(pos, sizeof(pos), "P%d %d.%d", es.pattern + 1, step / 4 + 1, step % 4 + 1);
    }
    else
        snprintf(pos, sizeof(pos), "P%d -.-", session_.project().currentPattern + 1);
    gfx_.text(390, y + 4, pos, theme::kText);

    const Ps2Audio::Stats& as = audio_.stats();
    if (!as.streaming)
        gfx_.text(510, y + 4, "NO AUDIO", theme::kError);
    else if (es.clipSamples)
        gfx_.text(540, y + 4, "CLIP", theme::kError);
    else
        gfx_.text(540, y + 4, G_NOTE " OK", theme::kOk);

    static const char* labels[(int)ViewId::Count];
    for (int i = 0; i < (int)ViewId::Count; ++i)
        labels[i] = views_[i]->tabName();
    ui::tabBar(gfx_, theme::kSafeLeft, 48, labels, (int)ViewId::Count, current_);
}

void App::drawStatusBar(View& view)
{
    const int y = kViewBottom + 4;
    gfx_.fillRect(0, y, Gfx::kWidth, Gfx::kHeight - y, theme::kPanelDark);
    if (ctx_.message[0] && (int32_t)(ctx_.messageUntilMs - ctx_.nowMs) > 0)
        gfx_.text(theme::kSafeLeft, y + 2, ctx_.message, theme::kText, 1, 2);
    else
        gfx_.text(theme::kSafeLeft, y + 2, view.hint(), theme::kTextDim, 1, 2);
    // Errors are sticky: they stay until replaced or cleared in PROJECT.
    if (log_.hasError())
        gfx_.text(theme::kSafeLeft, y + 18, log_.lastError(), theme::kError, 1, 2);
    else
        gfx_.text(theme::kSafeLeft, y + 18, "START play/pause  L3 stop  R3 debug", theme::kTextDim, 1, 2);
}

void App::serviceDebugMailbox(InputState& in)
{
    DebugMailbox& mb = g_debugMailbox;
    if (mb.inputMask) {
        const uint32_t m = mb.inputMask;
        mb.inputMask = 0;
        in.held |= m;
        in.pressed |= m;
        in.repeat |= m;
    }
    const uint32_t cmd = mb.command;
    if (!cmd)
        return;
    mb.command = 0;
    const char* sf = "__SELFTEST/";
    char ref[64];
    switch (cmd) {
    case DbgSampleSelfTest:
        // Synthetic files through the real asynchronous load path.
        snprintf(ref, sizeof(ref), "samples:%sSINE.WAV", sf);
        library_.request(ref, SampleLibrary::Action::Assign, 6);
        snprintf(ref, sizeof(ref), "samples:%sSTEREO.WAV", sf);
        library_.request(ref, SampleLibrary::Action::Assign, 7);
        snprintf(ref, sizeof(ref), "samples:%sGARBAGE.WAV", sf);
        library_.request(ref, SampleLibrary::Action::Load);
        snprintf(ref, sizeof(ref), "samples:%sMISSING.WAV", sf);
        library_.request(ref, SampleLibrary::Action::Load);
        break;
    case DbgReleaseUnreferenced:
        library_.releaseUnreferenced();
        break;
    case DbgStressProject:
        buildStressProject();
        break;
    case DbgExportRam:
        startRamExport(mb.arg != 0);
        break;
    case DbgFxBench:
        benchFx();
        break;
    case DbgBeginMeasure:
        const_cast<Ps2Audio::Stats&>(audio_.stats()).renderUsMax = 0;
        break;
    case DbgSwitchView:
        if (mb.arg < (uint32_t)ViewId::Count) {
            current_ = (int)mb.arg;
            views_[current_]->onEnter(ctx_);
        }
        break;
    default:
        break;
    }
}

void App::publishTelemetry()
{
    volatile uint32_t* t = g_debugMailbox.telemetry;
    const Ps2Audio::Stats& as = audio_.stats();
    t[0] = t[0] + 1;
    t[1] = library_.loadedOk();
    t[2] = library_.loadFailed();
    t[3] = (uint32_t)bank_.liveCount();
    t[4] = bank_.externalBytesUsed();
    t[5] = (uint32_t)library_.missingCount();
    t[6] = as.underruns;
    t[7] = as.tailRetries;
    t[8] = as.queuedFrames;
    t[9] = engine_.status().voicesActive;
    t[10] = (uint32_t)current_;
    t[11] = (uint32_t)ctx_.selectedChannel;
    for (int ch = 0; ch < 8; ++ch)
        t[12 + ch] = (uint32_t)(int32_t)session_.project().channels[ch].sampleSlot;
    t[20] = engine_.status().transport;
    t[21] = as.hwStale;
    t[22] = as.blocks;
    t[23] = as.spuBytes;
    t[24] = (uint32_t)ctx_.heardSongStep();
    t[25] = session_.project().clipCount;
    t[26] = engine_.status().songMode;
    t[27] = engine_.status().songBars;
    t[28] = engine_.status().pattern;
    const EngineStatus& es = engine_.status();
    t[32] = es.profCommandsUs;
    t[33] = es.profVoicesUs;
    t[34] = es.profFxUs;
    t[35] = es.profFinishUs;
    t[36] = es.commands;
    t[29] = as.renderUsAvg;          // engine render time per 512-frame block (us)
    t[30] = as.renderUsMax;
    {
        const Exporter& ex = ramExporter_ ? *ramExporter_ : exporter_;
        t[31] = ex.framesWritten() | ((uint32_t)ex.state() << 28); // export progress and state (0 idle, 1 running, 2 done, 3 failed)
    }
}

void App::frame()
{
    const uint64_t t0 = ps2sys::timeUs();
    ctx_.nowMs = (uint32_t)(t0 / 1000);

    input_.update(ctx_.nowMs, log_);
    storage_.poll(ctx_.nowMs, log_);
    library_.tick(); // one bounded file chunk per frame; never touches the audio thread
    if (library_.messageSerial() != seenLibraryMsg_) {
        seenLibraryMsg_ = library_.messageSerial();
        ctx_.toast("%s", library_.message());
    }
    InputState raw = input_.state();
    serviceDebugMailbox(raw);
    const InputState in = applyStickNavigation(raw);

    View& view = *views_[current_];
    waves_.tick(bank_);
    if (ramExporter_ && ramExporter_->running()) {
        // Debug: the same exporter, writing into a RAM block instead of USB.
        const uint64_t until = ps2sys::timeUs() + 10000;
        do {
            ramExporter_->tick(2);
        } while (ramExporter_->running() && ps2sys::timeUs() < until);
    }
    if (exporter_.running()) {
        // An offline render owns the frame: render for ~10 ms, show progress, Circle cancels.
        const uint64_t until = ps2sys::timeUs() + 10000;
        do {
            exporter_.tick(2);
        } while (exporter_.running() && ps2sys::timeUs() < until);
        if (in.hit(btn::Circle))
            exporter_.cancel();
        if (!exporter_.running())
            ctx_.toast("%s", exporter_.state() == Exporter::State::Done ? exporter_.message() : exporter_.message());
    } else {
        if (!menu_.isOpen())
            handleGlobalKeys(in);
        view.update(in, ctx_);
        serviceAutosave();
    }
    if (ctx_.requestedView != ViewId::Count) { // a view asked to go somewhere (e.g. rack -> piano roll)
        current_ = (int)ctx_.requestedView;
        ctx_.requestedView = ViewId::Count;
        views_[current_]->onEnter(ctx_);
    }
    View& shown = *views_[current_];

    gfx_.beginFrame(theme::kBackground);
    drawHeader();
    shown.draw(gfx_, ctx_);
    if (ctx_.debugOverlay)
        ui::drawDebugOverlay(gfx_, ctx_, fpsTimes10_, frameUs_);
    menu_.draw(gfx_);
    if (exporter_.running())
        drawExportOverlay();
    drawStatusBar(shown);
    frameUs_ = (uint32_t)(ps2sys::timeUs() - t0); // CPU time before the vsync wait
    gfx_.endFrame();

    publishTelemetry();
    ++fpsFrames_;
    const uint64_t now = ps2sys::timeUs();
    if (fpsStartUs_ == 0)
        fpsStartUs_ = now;
    if (now - fpsStartUs_ >= 1000000) {
        fpsTimes10_ = (uint32_t)((uint64_t)fpsFrames_ * 10000000ull / (now - fpsStartUs_));
        fpsFrames_ = 0;
        fpsStartUs_ = now;
    }
}

int App::run(int argc, char** argv)
{
    boot(argc, argv);
    if (!gfxOk_) {
        // No GS: fall back to the BIOS-style debug text screen so the user
        // still sees which subsystem failed instead of a black screen.
        init_scr();
        scr_printf("\n  PS2 DAW " PS2DAW_VERSION ": graphics init failed\n\n");
        for (int i = 0; i < (int)Subsystem::Count; ++i) {
            const StatusLog::Entry& e = log_.entry((Subsystem)i);
            scr_printf("  [%s] %s %s\n", StatusLog::healthName(e.health), StatusLog::name((Subsystem)i), e.detail);
        }
        for (;;)
            SleepThread();
    }
    for (;;)
        frame();
    return 0;
}

void App::drawExportOverlay()
{
    const int w = 440, h = 120;
    const int x = (Gfx::kWidth - w) / 2, y = (Gfx::kHeight - h) / 2;
    gfx_.fillRect(0, 0, Gfx::kWidth, Gfx::kHeight, 0x000000, 0x50);
    gfx_.fillRect(x - 2, y - 2, w + 4, h + 4, theme::kAccent);
    ui::panel(gfx_, x, y, w, h, "EXPORTING WAV");
    const int permille = exporter_.progressPermille();
    gfx_.fillRect(x + 16, y + 40, w - 32, 18, theme::kPanelDark);
    gfx_.fillRect(x + 16, y + 40, (w - 32) * permille / 1000, 18, theme::kAlive);
    const uint32_t sec = exporter_.framesWritten() / cfg::kSampleRate;
    gfx_.textf(x + 16, y + 66, theme::kText, "%d%%  %lu:%02lu rendered", permille / 10, (unsigned long)(sec / 60),
               (unsigned long)(sec % 60));
    gfx_.textf(x + 16, y + 84, theme::kTextDim, "%.50s", exporter_.path());
    gfx_.text(x + 16, y + h - 20, G_CIRCLE " CANCEL", theme::kTextDim, 1, 2);
}

void App::serviceAutosave()
{
    constexpr uint32_t kCheckMs = 2000;
    constexpr uint32_t kAutosaveMs = 90000;
    if (!storage_.ready() || exporter_.running())
        return;
    const uint32_t now = ctx_.nowMs;
    if (now - autosaveCheckMs_ < kCheckMs)
        return;
    autosaveCheckMs_ = now;

    // First time a drive is ready: tell the user if a crash left an autosave behind.
    char dir[64];
    if (!storage_.appPath(dir, sizeof(dir), ""))
        return;
    static uint8_t scratch[projectio::kMaxFileBytes];
    static uint8_t verify[projectio::kMaxFileBytes];
    class Files : public ProjectFiles {
    public:
        explicit Files(Storage& s) : st(s) {}
        int readAll(const char* p, uint8_t* b, size_t c) override { return st.readFile(p, b, c); }
        bool writeAll(const char* p, const uint8_t* d, size_t n) override { return st.writeFile(p, d, n); }
        bool renameTo(const char* a, const char* b) override { return st.renameFile(a, b); }
        bool removeFile(const char* p) override { return st.removeFile(p); }
        bool exists(const char* p) override { return st.fileExists(p); }
        Storage& st;
    } fs(storage_);
    if (!recoveryOffered_) {
        recoveryOffered_ = true;
        const slotstore::Info info = slotstore::peek(fs, dir, slotstore::kAutosaveSlot, scratch, sizeof(scratch));
        if (info.exists && !info.corrupt)
            ctx_.toast("Autosave found: PROJECT > Recover autosave");
    }

    const size_t n = projectio::save(session_.project(), scratch, sizeof(scratch));
    if (n == 0)
        return;
    const uint32_t crc = projectio::crc32(scratch, n);
    if (!autosaveHaveCrc_) { // the project as booted is the baseline: nothing to save yet
        autosaveHaveCrc_ = true;
        autosaveCrc_ = crc;
        autosaveLastMs_ = now;
        return;
    }
    if (crc != autosaveCrc_)
        autosaveDirty_ = true;
    if (!autosaveDirty_ || now - autosaveLastMs_ < kAutosaveMs)
        return;
    // Never compete with a sample load for the USB bus.
    if (library_.busy())
        return;
    storage_.ensureDir(dir);
    char err[64];
    if (slotstore::save(fs, dir, slotstore::kAutosaveSlot, session_.project(), scratch, verify, sizeof(scratch), err, sizeof(err))) {
        autosaveCrc_ = crc;
        autosaveDirty_ = false;
        log_.set(Subsystem::Project, Health::Ok, "autosaved %s", session_.project().name);
    } else {
        log_.set(Subsystem::Project, Health::Warning, "autosave: %s", err);
    }
    autosaveLastMs_ = now;
}

// ---- debugger-driven test helpers (see platform/debug_mailbox.hpp) ----------

uint8_t* g_ramExportBuf = nullptr; // exported WAV bytes of the last DbgExportRam (find with nm)

void App::buildStressProject()
{
    static Project p;
    p.resetEmpty();
    str::copy(p.name, sizeof(p.name), "STRESS");
    p.bpmCenti = 12800;
    p.masterVolume = 80;
    for (auto& c : p.channels)
        c.sampleSlot = -1; // resolved by loadProject() against the built-in kit
    // Channels 0..3 are synths, 4..7 stay samplers (the built-in kit) with envelopes.
    static const int kPreset[4] = {4, 1, 2, 6};
    for (int ch = 0; ch < 4; ++ch) {
        instr::applySynthPreset(p.channels[ch].inst, kPreset[ch]);
        p.channels[ch].inst.kind = (uint8_t)InstrKind::Synth;
        p.channels[ch].gate = 1;
        p.channels[ch].volume = 70;
    }
    for (int ch = 4; ch < 8; ++ch) {
        p.channels[ch].inst.env[kEnvEnabled] = 1;
        p.channels[ch].inst.env[kEnvDecay] = 250;
        p.channels[ch].inst.env[kEnvSustain] = 40;
        p.channels[ch].inst.env[kEnvRelease] = 120;
    }
    static const uint8_t kRoute[8] = {1, 2, 3, 3, 4, 4, 5, 6};
    for (int ch = 0; ch < 8; ++ch)
        p.channels[ch].route = kRoute[ch];
    PatternData& pat = p.patterns[0];
    pat.length = 16;
    auto note = [&](int ch, int step, int pitch, int len, int vel) {
        PianoNote& n = pat.notes[ch][pat.noteCount[ch]++];
        n.step = (uint8_t)step;
        n.pitch = (uint8_t)pitch;
        n.length = (uint8_t)len;
        n.velocity = (uint8_t)vel;
    };
    for (int pc : {48, 55, 60, 64, 67, 71})        // pad chord: six voices held for the bar
        note(0, 0, pc, 16, 90);
    for (int s = 0; s < 16; ++s)                    // plucked 16th arpeggio
        note(1, s, 60 + (s % 4) * 4 + (s / 8) * 7, 1, 100);
    for (int s : {0, 3, 6, 10, 12})                 // bass
        note(2, s, 36 + (s % 5), 2, 110);
    note(3, 0, 43, 8, 100);                         // wobble
    note(3, 8, 46, 8, 100);
    for (int s = 0; s < 16; s += 4)
        pat.velocity[4][s] = 120;                   // kick
    pat.velocity[5][4] = pat.velocity[5][12] = 110; // snare
    for (int s = 0; s < 16; s += 2)
        pat.velocity[6][s] = 90;                    // hats
    pat.velocity[7][14] = 100;
    // Effects: every type is in use; two delays and two reverbs exhaust the pools.
    auto fxset = [&](int t, int slot, FxType ty) { fx::setDefaults(p.tracks[t].fx[slot], ty); };
    fxset(1, 0, FxType::Reverb);
    fxset(1, 1, FxType::Eq);
    fxset(2, 0, FxType::Delay);
    fxset(2, 1, FxType::Distortion);
    fxset(3, 0, FxType::Compressor);
    fxset(3, 1, FxType::Filter);
    fxset(4, 0, FxType::Compressor);
    fxset(5, 0, FxType::Reverb);
    fxset(6, 0, FxType::Delay);
    fxset(0, 0, FxType::Eq);
    fxset(0, 1, FxType::Compressor);
    p.clipCount = 1;
    p.clips[0] = {0, 0, 0, 4, kAllChannels};
    p.songMode = 1;
    session_.loadProject(p);
    session_.play();
    ctx_.toast("Stress song: 4 synths + 4 sampler voices, 6 inserts, 11 effects");
}

void App::startRamExport(bool song)
{
    if (ramExporter_ && ramExporter_->running())
        return;
    delete ramExporter_;
    delete ramFile_;
    free(g_ramExportBuf);
    constexpr uint32_t kCap = 6u * 1024u * 1024u;
    g_ramExportBuf = (uint8_t*)malloc(kCap);
    ramFile_ = new RamExportFile(g_ramExportBuf, kCap);
    ramExporter_ = new Exporter(engine_, session_, *ramFile_, &audioHold_);
    char err[96];
    if (!ramExporter_->begin("ram:/EXPORT/STRESS.WAV", song ? Exporter::Source::Song : Exporter::Source::Pattern, err, sizeof(err)))
        ctx_.toast("RAM export failed: %s", err);
}

// Times each effect type on the EE: 64 blocks of noise through a default-parameter unit.
// Runs on the UI thread while the render thread keeps playing, so numbers are upper bounds.
void App::benchFx()
{
    FxMemory* mem = new FxMemory;
    static float l[512], r[512], tl[512], tr[512];
    uint32_t seed = 1;
    for (int ty = 1; ty < kFxTypeCount; ++ty) {
        FxUnit u;
        u.setType((FxType)ty, *mem);
        uint64_t total = 0;
        for (int b = 0; b < 64; ++b) {
            for (int i = 0; i < 512; ++i) {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                l[i] = (float)((int)(seed & 0x3fff) - 8192);
                r[i] = (float)((int)((seed >> 8) & 0x3fff) - 8192);
            }
            const uint64_t t0 = ps2sys::timeUs();
            u.process(l, r, 512, tl, tr);
            total += ps2sys::timeUs() - t0;
        }
        g_debugMailbox.telemetry[36 + ty] = (uint32_t)(total / 64);
        u.setType(FxType::None, *mem);
    }
    delete mem;
}
