// View framework: each DAW screen is a View; the App owns them, routes input
// to the active one and draws shared chrome (header, tabs, status bar).
#pragma once

#include <stdint.h>

#include "audio/audio_engine.hpp"
#include "audio/sample.hpp"
#include "core/status_log.hpp"
#include "platform/ps2_audio.hpp"
#include "platform/ps2_filesystem.hpp"
#include "platform/ps2_graphics.hpp"
#include "platform/ps2_input.hpp"
#include "project/sample_library.hpp"
#include "project/session.hpp"
#include "ui/widgets.hpp"

enum class ViewId : uint8_t { ChannelRack, Playlist, Mixer, Browser, Project, Count };

// Everything a view may use. Views change the song only through `session`.
struct UiContext {
    Session& session;
    const AudioEngine& engine;
    Ps2Audio& audio;
    Storage& storage;
    StatusLog& log;
    const SampleBank& bank;
    ui::ContextMenu& menu;
    SampleLibrary& library;

    uint32_t nowMs = 0;
    int selectedChannel = 0;  // shared between rack, mixer and browser
    bool debugOverlay = false;
    ViewId requestedView = ViewId::Count; // set to switch views

    char message[80] = "";
    uint32_t messageUntilMs = 0;

    void toast(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    // The engine frame the listener hears now, or the engine's render clock
    // when the stream is not running.
    uint32_t heardFrame() const;
    // Pattern step currently audible, or -1 when stopped.
    int heardStep() const;
    // Absolute song step (playlist mode) currently audible, or -1 when stopped.
    int heardSongStep() const;
    // True for ~90 ms after channel `ch` was heard triggering.
    bool channelActive(int ch) const;
};

class View {
public:
    virtual ~View() = default;
    virtual const char* tabName() const = 0;
    virtual const char* hint() const = 0;
    virtual void onEnter(UiContext&) {}
    virtual void update(const InputState& in, UiContext& ctx) = 0;
    virtual void draw(Gfx& g, UiContext& ctx) = 0;
};

// Top of the view area (below header and tabs) and bottom (above status bar).
constexpr int kViewTop = 72;
constexpr int kViewBottom = 398;
