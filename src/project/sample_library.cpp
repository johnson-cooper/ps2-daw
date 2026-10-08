#include "project/sample_library.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/sample_import.hpp"
#include "audio/sample_ref.hpp"
#include "core/strutil.hpp"
#include "project/session.hpp"

namespace {
constexpr uint32_t kMaxAdpBytes = 1024u * 1024u;
constexpr uint32_t kMinAdpBytes = 16;
} // namespace

SampleLibrary::SampleLibrary(Session& session, SampleBank& bank, SampleFileSource& files, HwSink* hw, StatusLog& log)
    : session_(session), bank_(bank), files_(files), hw_(hw), log_(log)
{
    memset(missing_, 0, sizeof(missing_));
    memset(queue_, 0, sizeof(queue_));
}

SampleLibrary::~SampleLibrary()
{
    abortJob();
}

void SampleLibrary::say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message_, sizeof(message_), fmt, ap);
    va_end(ap);
    ++messageSerial_;
}

void SampleLibrary::abortJob()
{
    if (job_.handle >= 0)
        files_.close(job_.handle);
    free(job_.buf);
    job_ = Job();
}

void SampleLibrary::addMissing(const char* ref)
{
    for (int i = 0; i < missingCount_; ++i)
        if (str::equalsNoCase(missing_[i], ref))
            return;
    if (missingCount_ < kMaxMissing)
        str::copy(missing_[missingCount_++], sizeof(missing_[0]), ref);
}

bool SampleLibrary::push(const Request& r, bool front)
{
    if (queued_ >= kQueue)
        return false;
    if (front) {
        for (int i = queued_; i > 0; --i)
            queue_[i] = queue_[i - 1];
        queue_[0] = r;
    } else {
        queue_[queued_] = r;
    }
    ++queued_;
    return true;
}

bool SampleLibrary::request(const char* ref, Action action, int channel)
{
    if (sampleref::classify(ref) != sampleref::Kind::Samples) {
        say("Invalid sample reference");
        log_.error(Subsystem::Samples, "invalid sample reference");
        return false;
    }
    // The same load already waiting? Keep one.
    for (int i = 0; i < queued_; ++i)
        if (queue_[i].action == action && queue_[i].channel == channel && str::equalsNoCase(queue_[i].ref, ref))
            return true;
    Request r;
    memset(&r, 0, sizeof(r));
    str::copy(r.ref, sizeof(r.ref), ref);
    r.action = action;
    r.channel = (uint8_t)channel;
    if (!push(r, false)) {
        say("Loader busy, try again");
        return false;
    }
    return true;
}

int SampleLibrary::progressPercent() const
{
    if (!job_.active || job_.size == 0)
        return 0;
    return (int)((uint64_t)job_.done * 100u / job_.size);
}

void SampleLibrary::failJob(const char* why)
{
    const char* base = sampleref::baseName(job_.req.ref);
    say("%s: %s", base, why);
    log_.error(Subsystem::Samples, "%.20s: %.60s", base, why);
    if (job_.req.action == Action::Bind)
        addMissing(job_.req.ref);
    ++loadFailed_;
    if (missingCount_)
        log_.set(Subsystem::Samples, Health::Warning, "%d sample(s) missing/failed", missingCount_);
    abortJob();
}

bool SampleLibrary::startJob()
{
    if (queued_ == 0)
        return false;
    const Request req = queue_[0];
    for (int i = 1; i < queued_; ++i)
        queue_[i - 1] = queue_[i];
    --queued_;

    // Already resident (loaded earlier): no I/O.
    const int existing = bank_.findByRef(req.ref);
    if (existing >= 0) {
        completed(existing, req);
        return true;
    }
    job_ = Job();
    job_.req = req;
    job_.active = true;

    const char* rel = sampleref::relPart(req.ref);
    if (!rel) {
        failJob("bad reference");
        return true;
    }
    const sampleref::FileType type = sampleref::classifyFile(rel, false);
    if (type != sampleref::FileType::Wav && type != sampleref::FileType::Adp) {
        failJob("unsupported file type");
        return true;
    }
    uint32_t size = 0;
    job_.handle = files_.openSample(rel, &size);
    if (job_.handle < 0) {
        failJob("file not found");
        return true;
    }
    const bool adp = type == sampleref::FileType::Adp;
    if (size < (adp ? kMinAdpBytes : sampleimport::kMinFileBytes)) {
        failJob("file too small");
        return true;
    }
    if (size > (adp ? kMaxAdpBytes : sampleimport::kMaxFileBytes)) {
        failJob(adp ? "file too large (max 1 MiB)" : "file too large (max 3 MiB)");
        return true;
    }
    job_.buf = (uint8_t*)malloc(size);
    if (!job_.buf) {
        failJob("out of memory");
        return true;
    }
    job_.size = size;
    return true;
}

void SampleLibrary::stepJob()
{
    const uint32_t want = job_.size - job_.done;
    const int n = files_.read(job_.handle, job_.buf + job_.done, want > kChunkBytes ? kChunkBytes : want);
    if (n < 0) {
        failJob("read error");
        return;
    }
    if (n == 0) {
        failJob("file ended early");
        return;
    }
    job_.done += (uint32_t)n;
    if (job_.done >= job_.size)
        finishJob();
}

void SampleLibrary::finishJob()
{
    files_.close(job_.handle);
    job_.handle = -1;

    const char* rel = sampleref::relPart(job_.req.ref);
    char name[16];
    sampleimport::displayName(name, sizeof(name), rel);
    const bool adp = sampleref::classifyFile(rel, false) == sampleref::FileType::Adp;
    char err[48] = "";
    int slot = -1;

    if (adp) {
        slot = bank_.addHwOnly(name, job_.req.ref, job_.size);
        if (slot < 0) {
            failJob("no free sample slots");
            return;
        }
        if (!hw_ || !hw_->uploadApcm(slot, job_.buf, job_.size, err, sizeof(err))) {
            bank_.discardNew(slot); // never reached the engine
            failJob(err[0] ? err : "SPU2 not available");
            return;
        }
        session_.setSampleHwReady(slot, true);
    } else {
        const sampleimport::Result r =
            sampleimport::importWav(bank_, name, job_.req.ref, job_.buf, job_.size, &slot, err, sizeof(err));
        if (r == sampleimport::Result::BudgetFull || r == sampleimport::Result::SlotsFull) {
            // Make room by releasing imports nothing uses, then retry once.
            if (!job_.req.retried && releaseUnreferenced() > 0) {
                Request again = job_.req;
                again.retried = 1;
                abortJob();
                push(again, true);
                return;
            }
        }
        if (r != sampleimport::Result::Ok) {
            failJob(err);
            return;
        }
    }

    const Request req = job_.req;
    abortJob();
    ++loadedOk_;
    completed(slot, req);
}

void SampleLibrary::completed(int slot, const Request& req)
{
    const Sample* s = bank_.get(slot);
    if (!s)
        return;
    switch (req.action) {
    case Action::Load:
        say("Loaded %s", s->name);
        break;
    case Action::Bind:
        session_.bindSamples();
        say("Loaded %s", s->name);
        break;
    case Action::Assign:
        session_.setSample(req.channel, slot);
        say("Channel %d <- %s", req.channel + 1, s->name);
        break;
    case Action::Preview:
        session_.previewSample(slot, s->hwOnly ? VoiceMode::Spu2 : VoiceMode::Software);
        break;
    case Action::PreviewSpu2:
        if (uploadSpu2(slot))
            session_.previewSample(slot, VoiceMode::Spu2);
        break;
    case Action::UploadSpu2:
        uploadSpu2(slot);
        break;
    }
    if (missingCount_ == 0)
        log_.set(Subsystem::Samples, Health::Ok, "%d samples, %lu KiB imported", bank_.liveCount(),
                 (unsigned long)(bank_.externalBytesUsed() / 1024));
}

bool SampleLibrary::uploadSpu2(int slot)
{
    const Sample* s = bank_.get(slot);
    if (!s || !hw_) {
        say("SPU2 not available");
        return false;
    }
    if (hw_->resident(slot))
        return true;
    char err[48] = "";
    if (s->hwOnly || !hw_->uploadPcm(slot, *s, err, sizeof(err))) {
        say("%s: %s", s->name, err[0] ? err : "cannot upload to SPU2");
        log_.error(Subsystem::Spu2, "%.14s: %.60s", s->name, err[0] ? err : "cannot upload");
        return false;
    }
    session_.setSampleHwReady(slot, true);
    say("%s in SPU2 (%lu KiB used)", s->name, (unsigned long)(hw_->bytesUsed() / 1024));
    return true;
}

bool SampleLibrary::unloadSpu2(int slot)
{
    const Sample* s = bank_.get(slot);
    if (!s || !hw_ || !hw_->resident(slot))
        return false;
    if (s->hwOnly) {
        say("%s lives only in SPU2; unload the sample", s->name);
        return false;
    }
    session_.setSampleHwReady(slot, false); // the engine stops generating SPU2 notes first
    hw_->unload(slot);
    say("%s removed from SPU2", s->name);
    return true;
}

bool SampleLibrary::unload(int slot)
{
    const Sample* s = bank_.get(slot);
    if (!s || s->builtin)
        return false;
    if (session_.slotInUse(slot)) {
        say("%s is assigned to a channel", s->name);
        return false;
    }
    char name[16];
    str::copy(name, sizeof(name), s->name);
    if (hw_ && hw_->resident(slot)) {
        session_.setSampleHwReady(slot, false);
        hw_->unload(slot);
    }
    if (!session_.releaseSample(slot))
        return false;
    say("Unloaded %s", name);
    return true;
}

int SampleLibrary::releaseUnreferenced()
{
    int n = 0;
    for (int i = 0; i < bank_.count(); ++i) {
        const Sample* s = bank_.get(i);
        if (!s || s->builtin || session_.slotInUse(i))
            continue;
        if (hw_ && hw_->resident(i)) {
            session_.setSampleHwReady(i, false);
            hw_->unload(i);
        }
        if (session_.releaseSample(i))
            ++n;
    }
    return n;
}

void SampleLibrary::onProjectLoaded()
{
    missingCount_ = 0;
    memset(missing_, 0, sizeof(missing_));
    // Re-bind what is already resident, free what the new project does not use.
    session_.bindSamples();
    releaseUnreferenced();
    const Project& p = session_.project();
    for (int ch = 0; ch < p.channelCount; ++ch) {
        const ChannelData& c = p.channels[ch];
        const sampleref::Kind k = sampleref::classify(c.sampleRef);
        if (k == sampleref::Kind::None || k == sampleref::Kind::Builtin)
            continue;
        if (c.sampleSlot >= 0 && bank_.get(c.sampleSlot))
            continue;
        if (k == sampleref::Kind::Invalid || !request(c.sampleRef, Action::Bind))
            addMissing(c.sampleRef);
    }
    // Samples used only by playlist audio clips.
    for (int i = 0; i < cfg::kMaxAudioSources; ++i) {
        bool used = false;
        for (int k = 0; k < p.audioClipCount; ++k)
            used |= p.audioClips[k].source == i;
        if (!used || !p.audioRefs[i][0])
            continue;
        const sampleref::Kind k = sampleref::classify(p.audioRefs[i]);
        if (k == sampleref::Kind::None || k == sampleref::Kind::Builtin)
            continue;
        if (p.audioSlot[i] >= 0 && bank_.get(p.audioSlot[i]))
            continue;
        if (k == sampleref::Kind::Invalid || !request(p.audioRefs[i], Action::Bind))
            addMissing(p.audioRefs[i]);
    }
    if (missingCount_)
        log_.set(Subsystem::Samples, Health::Warning, "%d sample(s) missing/failed", missingCount_);
}

void SampleLibrary::tick()
{
    if (session_.loadSerial() != seenLoadSerial_) {
        seenLoadSerial_ = session_.loadSerial();
        onProjectLoaded();
    }
    if (job_.active) {
        if (job_.buf)
            stepJob();
    } else if (queued_ > 0) {
        startJob(); // opens the file; reading starts next frame
    }
    bank_.reap();
    session_.pumpReleases();
}
