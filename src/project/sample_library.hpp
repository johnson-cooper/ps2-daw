// Sample library: asynchronous USB loading and the sample lifecycle policy.
//
// Everything here runs on the UI thread. Loading is a small state machine
// advanced once per UI frame (tick()): it reads one bounded chunk of the file
// per frame, so a large file never stalls a frame, and the audio thread is
// never involved in file I/O, decoding or allocation. The finished sample is
// published to the bank in one step; releasing goes through the bank's
// request/acknowledge/reap handshake.
//
// Ownership policy: a sample is "in use" while a rack channel references its
// slot. Imported samples that nothing references are released on project
// load and whenever memory/slots run out; built-ins are never released.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/sample.hpp"
#include "audio/sample_io.hpp"
#include "core/status_log.hpp"

class Session;

class SampleLibrary {
public:
    enum class Action : uint8_t {
        Load,        // just load it
        Assign,      // load, then assign to `channel`
        Preview,     // load, then audition (software, or SPU2 for .adp)
        PreviewSpu2, // load, upload to SPU2 if needed, audition there
        UploadSpu2,  // load, upload to SPU2
        Bind,        // project load: load, then re-bind channels by reference
    };

    static constexpr int kQueue = 10;
    static constexpr int kMaxMissing = 8;
    static constexpr uint32_t kChunkBytes = 32 * 1024;

    SampleLibrary(Session& session, SampleBank& bank, SampleFileSource& files, HwSink* hw, StatusLog& log);
    ~SampleLibrary();
    SampleLibrary(const SampleLibrary&) = delete;
    SampleLibrary& operator=(const SampleLibrary&) = delete;

    // Queues a load of an external reference ("samples:..."). Returns false
    // (with message()) if the reference is invalid or the queue is full.
    bool request(const char* ref, Action action, int channel = 0);

    // Advance loading, project re-binding and deferred frees. Once per frame.
    void tick();

    // Explicit unload of one imported sample. Refused while a channel uses it.
    bool unload(int slot);
    // Releases every imported sample no channel uses. Returns the count.
    int releaseUnreferenced();
    // Uploads an already-loaded mono sample to SPU2 / frees its SPU2 copy.
    bool uploadSpu2(int slot);
    bool unloadSpu2(int slot);

    bool busy() const { return job_.active || queued_ > 0; }
    int progressPercent() const;
    const char* currentName() const { return job_.active ? job_.req.ref : ""; }
    int pending() const { return queued_ + (job_.active ? 1 : 0); }

    int missingCount() const { return missingCount_; }
    const char* missingRef(int i) const { return (i >= 0 && i < missingCount_) ? missing_[i] : ""; }

    // Latest user-facing result line; the serial bumps for each new one.
    const char* message() const { return message_; }
    uint32_t messageSerial() const { return messageSerial_; }
    uint32_t loadedOk() const { return loadedOk_; }
    uint32_t loadFailed() const { return loadFailed_; }

private:
    struct Request {
        char ref[64];
        Action action;
        uint8_t channel;
        uint8_t retried;
    };
    struct Job {
        bool active = false;
        Request req;
        int handle = -1;
        uint8_t* buf = nullptr;
        uint32_t size = 0, done = 0;
    };

    void say(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void failJob(const char* why);
    void addMissing(const char* ref);
    bool startJob();
    void stepJob();
    void finishJob();
    void completed(int slot, const Request& req);
    void abortJob();
    bool push(const Request& r, bool front);
    void onProjectLoaded();

    Session& session_;
    SampleBank& bank_;
    SampleFileSource& files_;
    HwSink* hw_;
    StatusLog& log_;

    Request queue_[kQueue];
    int queued_ = 0;
    Job job_;

    char missing_[kMaxMissing][64];
    int missingCount_ = 0;
    uint32_t seenLoadSerial_ = 0;

    char message_[80] = "";
    uint32_t messageSerial_ = 0;
    uint32_t loadedOk_ = 0, loadFailed_ = 0;
};
