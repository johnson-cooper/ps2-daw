// Offline WAV render (Milestone 8).
//
// The exporter drives the *same* AudioEngine that plays live: the realtime
// thread is parked, the UI thread calls render() in a loop and streams the
// blocks into a 16-bit stereo 48 kHz WAV file. Mixer routing, inserts,
// effects, envelopes, synthesizers, audio clips and note timing are therefore
// exactly what you hear. SPU2 hardware voices are the one exception: they live
// outside the EE mixer, so during an export software copies are rendered
// instead (SPU2-only .adp samples cannot be, and are counted and reported).
//
// The file is written as <name>.TMP, finished (header patched), verified by
// reading the header back, and only then renamed over the final name, so a
// pulled USB stick or a full disk never leaves a half-written .WAV behind.
//
// Platform access goes through ExportFile / RenderHold, so the host tests
// drive this class with an in-memory file.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_engine.hpp"
#include "project/session.hpp"

class ExportFile {
public:
    virtual ~ExportFile() {}
    virtual bool open(const char* path) = 0;                  // create / truncate for writing
    virtual bool write(const void* data, uint32_t bytes) = 0; // append
    virtual bool rewriteHeader(const uint8_t* hdr, uint32_t bytes) = 0; // overwrite the first bytes
    virtual bool close() = 0;
    virtual bool readHeader(const char* path, uint8_t* out, uint32_t bytes, uint32_t* fileSize) = 0;
    virtual bool rename(const char* from, const char* to) = 0; // replaces `to`
    virtual void remove(const char* path) = 0;
};

class RenderHold {
public:
    virtual ~RenderHold() {}
    // Parks the realtime render thread so the caller may call render() itself.
    // Returns false if it could not be parked in time.
    virtual bool hold() = 0;
    virtual void release() = 0;
};

namespace wavexport {

constexpr uint32_t kHeaderBytes = 44;
// Writes the canonical 44-byte header for `dataBytes` of 48 kHz stereo 16-bit PCM.
void makeHeader(uint8_t* out, uint32_t dataBytes);
// Checks a header against the file size; returns the data length in bytes or -1.
int64_t checkHeader(const uint8_t* hdr, uint32_t fileSize);

} // namespace wavexport

class Exporter {
public:
    enum class State : uint8_t { Idle, Running, Done, Failed };
    enum class Source : uint8_t { Song, Pattern };

    static constexpr uint32_t kMaxFrames = 20u * 60u * (uint32_t)cfg::kSampleRate; // 20 minutes
    static constexpr uint32_t kMinTailFrames = 12000;     // 250 ms of ring-out at least
    static constexpr uint32_t kMaxTailFrames = 6u * 48000u;
    static constexpr uint32_t kSilenceEndFrames = 9600;   // stop early after 200 ms of digital silence

    Exporter(AudioEngine& engine, Session& session, ExportFile& file, RenderHold* hold);

    // Starts an export. Song mode renders the playlist once; pattern mode
    // renders the current pattern's loop once. Returns false (with `err`) when
    // there is nothing to render, the render thread could not be parked, or the
    // file could not be created.
    bool begin(const char* path, Source source, char* err, size_t errCap);
    // Renders and writes up to `maxBlocks` blocks of 512 frames. Call every UI frame.
    void tick(int maxBlocks);
    void cancel();

    State state() const { return state_; }
    bool running() const { return state_ == State::Running; }
    // 0..1000
    int progressPermille() const;
    uint32_t framesWritten() const { return framesWritten_; }
    uint32_t expectedFrames() const { return expectedFrames_; }
    uint32_t skippedNotes() const { return skipped_; }
    uint32_t clippedSamples() const { return clipped_; }
    uint32_t peak() const { return peak_; }
    const char* message() const { return message_; }
    const char* path() const { return path_; }

private:
    void fail(const char* msg);
    void finish();
    void restore();
    bool flush();

    AudioEngine& engine_;
    Session& session_;
    ExportFile& file_;
    RenderHold* hold_;

    State state_;
    Source source_;
    bool held_, fileOpen_, prevSong_;
    uint8_t prevPattern_;
    uint32_t framesWritten_, expectedFrames_, doneAtFrame_, silentRun_;
    uint32_t skipped_, clipped_, peak_;
    uint32_t pending_;      // bytes in buffer_
    uint32_t tailFrames_;
    char path_[160], tmpPath_[168], message_[96];
    alignas(16) uint8_t buffer_[16384 * 4];
};
