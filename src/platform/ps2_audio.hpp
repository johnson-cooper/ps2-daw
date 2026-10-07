// PS2 audio backend built on the PS2Build `audio` library (audio.irx on top
// of libsd.irx).
//
// Two output paths, chosen per rack channel:
//  * PCM stream: a dedicated EE thread renders the AudioEngine in fixed
//    blocks and keeps the library's stream queue at a target depth. This is
//    the master bus; it is sample-accurate and fully metered.
//  * SPU2 hardware voices: generated sounds are PS-ADPCM encoded at boot and
//    uploaded to SPU2 RAM. Hardware notes produced by the engine are queued
//    with their target frame and fired when the stream playhead (frames
//    written minus frames still queued) reaches them, so they line up with
//    the stream to within one scheduler tick.
//
// The UI thread never calls into the audio library; it only reads Stats.
#pragma once

#include <stdint.h>

#include "audio/audio_engine.hpp"
#include "audio/sample.hpp"
#include "audio/sample_io.hpp"
#include "core/status_log.hpp"

class Session;

class Ps2Audio : public HwSink {
public:
    struct Stats {
        volatile uint8_t driverLoaded = 0;
        volatile uint8_t initialized = 0;
        volatile uint8_t streaming = 0;
        volatile uint32_t queuedFrames = 0;
        volatile uint32_t heardFrame = 0;       // engine frame currently audible
        volatile uint32_t underruns = 0;
        volatile uint32_t blocks = 0;
        volatile uint32_t renderUsLast = 0;
        volatile uint32_t renderUsMax = 0;
        volatile uint32_t renderUsAvg = 0;      // exponential average
        volatile uint32_t rpcErrors = 0;
        volatile uint32_t hwPlayed = 0;
        volatile uint32_t hwDropped = 0;        // hardware triggers lost (pending list full)
        volatile uint32_t hwStale = 0;          // notes skipped: sample unloaded meanwhile
        volatile uint32_t tailRetries = 0;      // blocks the stream only partly accepted
        volatile uint32_t hwLateUs = 0;         // worst observed dispatch lateness
        volatile int32_t lastError = 0;         // last audio library error code
        uint8_t spuSounds = 0;                  // samples resident in SPU2 RAM
        uint32_t spuBytes = 0;
    };

    static constexpr int kBlockFrames = 512;            // 10.7 ms per render
    static constexpr int kDefaultLatencyFrames = 2048;  // ~43 ms queued target
    static constexpr int kMinLatencyFrames = 1024;
    static constexpr int kMaxLatencyFrames = 8192;

    // Loads libsd.irx + audio.irx, connects, starts the 48 kHz stereo stream.
    // Logs exactly which stage failed. The render thread is not started yet.
    bool init(StatusLog& log);

    // PS-ADPCM-encodes every bank sample and uploads it to SPU2 RAM, then
    // tells the engine (via the session) which samples have hardware copies.
    void uploadHardwareSamples(const SampleBank& bank, Session& session, StatusLog& log);

    // Starts the realtime render thread. `mainPriority` is the priority the
    // caller had when calling init(); the render thread runs just above the
    // UI thread and just below the audio library's own event thread.
    bool start(AudioEngine& engine, StatusLog& log);

    const Stats& stats() const { return stats_; }
    int latencyFrames() const { return latency_; }
    void setLatencyFrames(int frames);

    static const char* errorText(int code);

    // HwSink: explicit SPU2 residency for individual samples (UI thread).
    bool uploadPcm(int slot, const Sample& s, char* err, size_t cap) override;
    bool uploadApcm(int slot, const uint8_t* file, uint32_t size, char* err, size_t cap) override;
    void unload(int slot) override;
    bool resident(int slot) const override;
    uint32_t bytesUsed() const override { return stats_.spuBytes; }

private:
    struct Pending {
        uint32_t frame;
        uint8_t channel, sample, volume;
        int8_t pan;
    };
    static constexpr int kPending = 64;

    static void threadEntry(void* self);
    void run();
    void collectHwTriggers();
    void dispatchHw(uint32_t heard);
    // Hands the unsubmitted part of the rendered block to the stream without
    // waiting. Advances written_ by exactly the frames accepted. Returns false
    // on a library error (the block is then abandoned).
    bool submitTail();

    AudioEngine* engine_ = nullptr;
    Stats stats_;
    volatile int latency_ = kDefaultLatencyFrames;
    uint32_t written_ = 0;      // frames ACCEPTED by the stream (not merely rendered)
    int tailFrames_ = 0;        // rendered frames not yet accepted
    int tailOffset_ = 0;        // first unaccepted frame within the block
    bool primed_ = false;

    int32_t spuHandle_[cfg::kMaxSamples] = {};   // written by the UI thread, read by the render thread
    uint32_t spuSlotBytes_[cfg::kMaxSamples] = {};
    int8_t spuVolume_[24] = {};
    int8_t spuPan_[24] = {};

    Pending pending_[kPending];
    int pendingCount_ = 0;

    int threadId_ = -1;
    void* stack_ = nullptr;
};
