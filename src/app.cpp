#include "app.hpp"

#include <debug.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "audio/drum_synth.hpp"
#include "platform/debug_mailbox.hpp"
#include "platform/ps2_system.hpp"
#include "ui/debug_overlay.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"
#include "version.hpp"

App* App::instance_ = nullptr;

App::App()
    : session_(engine_, bank_, &App::waitForAudio),
      testSource_(storage_),
      library_(session_, bank_, testSource_, &audio_, log_),
      ctx_{session_, engine_, audio_, storage_, log_, bank_, menu_, library_}
{
    instance_ = this;
    views_[(int)ViewId::ChannelRack] = &rack_;
    views_[(int)ViewId::Playlist] = &playlist_;
    views_[(int)ViewId::Mixer] = &mixer_;
    views_[(int)ViewId::Browser] = &browser_;
    views_[(int)ViewId::Project] = &projectView_;
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
    gfx_.text(theme::kSafeRight - Gfx::textWidth("SELECT: VIEW", 1), 50, "SELECT: VIEW", theme::kTextDim, 1, 2);
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
    if (!menu_.isOpen())
        handleGlobalKeys(in);
    view.update(in, ctx_);
    View& shown = *views_[current_];

    gfx_.beginFrame(theme::kBackground);
    drawHeader();
    shown.draw(gfx_, ctx_);
    if (ctx_.debugOverlay)
        ui::drawDebugOverlay(gfx_, ctx_, fpsTimes10_, frameUs_);
    menu_.draw(gfx_);
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
