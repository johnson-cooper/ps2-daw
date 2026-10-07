#include "platform/ps2_audio.hpp"

#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audio_irx.h"
#include "libsd_irx.h"

#include "audio/adpcm.hpp"
#include "platform/ps2_system.hpp"
#include "project/session.hpp"

namespace {

// EE thread priorities (lower number = higher priority). The audio library
// creates its event thread at (caller priority - 1) inside audio_init(), so
// init() runs at kInitPriority and the order ends up:
//   63 audio library event thread  >  64 render thread  >  80 UI/main thread
// The render thread therefore preempts drawing whenever it is runnable, and
// never starves the library thread that services its RPC events.
constexpr int kInitPriority = 64;
constexpr int kRenderPriority = 64;
constexpr int kUiPriority = 80;

constexpr int kStackBytes = 16 * 1024;
constexpr int kBytesPerFrame = 4; // 16-bit stereo
constexpr int kPreviewSpuChannel = 23;

int16_t g_block[Ps2Audio::kBlockFrames * 2] __attribute__((aligned(64)));

} // namespace

const char* Ps2Audio::errorText(int code)
{
    return audio_get_error_message(code);
}

bool Ps2Audio::init(StatusLog& log)
{
    memset(spuVolume_, -1, sizeof(spuVolume_));
    memset(spuPan_, 0, sizeof(spuPan_));

    // audio.irx needs libsd.irx resident first (see the audio library's
    // "Loading the driver" documentation).
    if (!ps2sys::loadModule("libsd", libsd_irx, size_libsd_irx)) {
        log.fail(Subsystem::AudioIrx, "libsd.irx failed to load");
        return false;
    }
    if (!ps2sys::loadModule("audio", audio_irx, size_audio_irx)) {
        log.fail(Subsystem::AudioIrx, "audio.irx failed to load");
        return false;
    }
    stats_.driverLoaded = 1;
    log.set(Subsystem::AudioIrx, Health::Ok, "libsd.irx + audio.irx loaded");

    ps2sys::setMainThreadPriority(kInitPriority);
    int rc = audio_init();
    if (rc != AUDIO_OK) {
        stats_.lastError = rc;
        log.fail(Subsystem::AudioInit, "audio_init: %s (%d)", audio_get_error_message(rc), rc);
        return false;
    }
    stats_.initialized = 1;

    audio_version_t ver;
    if (audio_get_version(&ver) == AUDIO_OK)
        log.set(Subsystem::AudioInit, Health::Ok, "lib %u.%u.%u drv %u.%u.%u", ver.library_major, ver.library_minor,
                ver.library_patch, ver.driver_major, ver.driver_minor, ver.driver_patch);
    else
        log.set(Subsystem::AudioInit, Health::Ok, "connected");

    audio_set_master_volume(100);
    audio_stream_set_volume(100);

    // Our engine already produces the SPU2's native layout, so the library's
    // converter is a straight copy.
    const audio_stream_format_t format = {cfg::kSampleRate, 16, cfg::kOutputChannels};
    rc = audio_stream_start(&format);
    if (rc != AUDIO_OK) {
        stats_.lastError = rc;
        log.fail(Subsystem::AudioStream, "stream_start: %s", audio_get_error_message(rc));
        return false;
    }
    log.set(Subsystem::AudioStream, Health::Pending, "48000 Hz s16 stereo, thread not started");
    return true;
}

bool Ps2Audio::resident(int slot) const
{
    return slot >= 0 && slot < cfg::kMaxSamples && spuHandle_[slot] != AUDIO_SOUND_INVALID;
}

bool Ps2Audio::uploadPcm(int slot, const Sample& s, char* err, size_t cap)
{
    auto fail = [&](const char* msg) {
        if (err && cap)
            snprintf(err, cap, "%s", msg);
        return false;
    };
    if (!stats_.initialized)
        return fail("audio not initialised");
    if (slot < 0 || slot >= cfg::kMaxSamples)
        return fail("bad slot");
    if (resident(slot))
        return true;
    if (!s.data || s.channels != 1)
        return fail("SPU2 needs a mono sample");
    if (s.sampleRate < 4000 || s.sampleRate > 48000)
        return fail("rate must be 4-48 kHz for SPU2");
    const size_t bytes = adpcm::encodedSize(s.frames);
    uint8_t* buf = (uint8_t*)memalign(64, bytes);
    if (!buf)
        return fail("out of memory");
    adpcm::encodeMono(s.data, s.frames, buf);
    audio_sound_desc_t desc;
    desc.adpcm_data = buf;
    desc.adpcm_size_bytes = (int32_t)bytes;
    desc.sample_rate_hz = (int32_t)s.sampleRate;
    desc.channel_count = 1;
    desc.loops = 0;
    audio_sound_t handle = AUDIO_SOUND_INVALID;
    const int rc = audio_sound_load_adpcm(&desc, &handle); // copies; blocks until uploaded
    free(buf);
    if (rc != AUDIO_OK || handle == AUDIO_SOUND_INVALID) {
        stats_.lastError = rc;
        return fail(rc == -AUDIO_ERR_OUT_OF_SPU_MEMORY ? "SPU2 RAM full" : audio_get_error_message(rc));
    }
    spuSlotBytes_[slot] = (uint32_t)bytes;
    stats_.spuBytes += (uint32_t)bytes;
    stats_.spuSounds = (uint8_t)(stats_.spuSounds + 1);
    __atomic_store_n(&spuHandle_[slot], (int32_t)handle, __ATOMIC_RELEASE);
    return true;
}

bool Ps2Audio::uploadApcm(int slot, const uint8_t* file, uint32_t size, char* err, size_t cap)
{
    auto fail = [&](const char* msg) {
        if (err && cap)
            snprintf(err, cap, "%s", msg);
        return false;
    };
    if (!stats_.initialized)
        return fail("audio not initialised");
    if (slot < 0 || slot >= cfg::kMaxSamples)
        return fail("bad slot");
    audio_sound_t handle = AUDIO_SOUND_INVALID;
    // The library validates the APCM image; the file buffer is only read.
    const int rc = audio_sound_load_apcm(file, (int32_t)size, &handle);
    if (rc != AUDIO_OK || handle == AUDIO_SOUND_INVALID) {
        stats_.lastError = rc;
        return fail(rc == -AUDIO_ERR_OUT_OF_SPU_MEMORY ? "SPU2 RAM full"
                    : rc == -AUDIO_ERR_BAD_SOUND_DATA  ? "not a valid .adp file"
                                                      : audio_get_error_message(rc));
    }
    spuSlotBytes_[slot] = size;
    stats_.spuBytes += size;
    stats_.spuSounds = (uint8_t)(stats_.spuSounds + 1);
    __atomic_store_n(&spuHandle_[slot], (int32_t)handle, __ATOMIC_RELEASE);
    return true;
}

void Ps2Audio::unload(int slot)
{
    if (!resident(slot))
        return;
    // Invalidate first so the render thread stops dispatching notes for it,
    // then free the SPU2 memory (which also stops any voice still playing it).
    const int32_t handle = spuHandle_[slot];
    __atomic_store_n(&spuHandle_[slot], (int32_t)AUDIO_SOUND_INVALID, __ATOMIC_RELEASE);
    audio_sound_free(handle);
    stats_.spuBytes -= spuSlotBytes_[slot];
    spuSlotBytes_[slot] = 0;
    if (stats_.spuSounds)
        stats_.spuSounds = (uint8_t)(stats_.spuSounds - 1);
}

void Ps2Audio::uploadHardwareSamples(const SampleBank& bank, Session& session, StatusLog& log)
{
    if (!stats_.initialized) {
        log.set(Subsystem::Spu2, Health::Skipped, "audio not initialised");
        return;
    }
    int loaded = 0, failed = 0;
    char err[48] = "";
    for (int i = 0; i < bank.count(); ++i) {
        const Sample* s = bank.get(i);
        if (!s || !s->builtin || s->channels != 1 || s->sampleRate < 4000 || s->sampleRate > 48000)
            continue;
        if (uploadPcm(i, *s, err, sizeof(err))) {
            session.setSampleHwReady(i, true);
            ++loaded;
        } else {
            ++failed;
        }
    }
    if (failed == 0)
        log.set(Subsystem::Spu2, Health::Ok, "%d sounds, %lu KiB in SPU2 RAM", loaded, (unsigned long)(stats_.spuBytes / 1024));
    else if (loaded > 0)
        log.set(Subsystem::Spu2, Health::Warning, "%d ok, %d failed: %s", loaded, failed, err);
    else
        log.fail(Subsystem::Spu2, "upload failed: %s", err);
}

bool Ps2Audio::start(AudioEngine& engine, StatusLog& log)
{
    if (!stats_.initialized)
        return false;
    engine_ = &engine;

    stack_ = memalign(16, kStackBytes);
    if (!stack_) {
        log.fail(Subsystem::AudioStream, "no memory for audio thread stack");
        return false;
    }
    extern void* _gp;
    ee_thread_t t;
    memset(&t, 0, sizeof(t));
    t.func = (void*)&Ps2Audio::threadEntry;
    t.stack = stack_;
    t.stack_size = kStackBytes;
    t.gp_reg = &_gp;
    t.initial_priority = kRenderPriority;
    threadId_ = CreateThread(&t);
    if (threadId_ < 0) {
        log.fail(Subsystem::AudioStream, "CreateThread failed (%d)", threadId_);
        return false;
    }
    StartThread(threadId_, this);
    // Drop the UI below the render thread; from here on audio preempts drawing.
    ps2sys::setMainThreadPriority(kUiPriority);
    stats_.streaming = 1;
    log.set(Subsystem::AudioStream, Health::Ok, "48000 Hz s16 stereo, %d-frame blocks", kBlockFrames);
    return true;
}

void Ps2Audio::setLatencyFrames(int frames)
{
    if (frames < kMinLatencyFrames)
        frames = kMinLatencyFrames;
    if (frames > kMaxLatencyFrames)
        frames = kMaxLatencyFrames;
    latency_ = frames;
}

void Ps2Audio::threadEntry(void* self)
{
    static_cast<Ps2Audio*>(self)->run();
}

bool Ps2Audio::submitTail()
{
    // audio_stream_queue_pcm() never waits and may accept fewer bytes than
    // offered when the ring is full. Whatever is not accepted stays in the
    // block and is resubmitted before anything new is rendered, so no audio
    // is dropped and written_ only ever counts frames the stream holds.
    // That keeps heard = written_ - queued exact.
    while (tailFrames_ > 0) {
        const uint8_t* p = (const uint8_t*)g_block + tailOffset_ * kBytesPerFrame;
        const int n = audio_stream_queue_pcm(p, tailFrames_ * kBytesPerFrame);
        if (n < 0) {
            stats_.lastError = n;
            stats_.rpcErrors = stats_.rpcErrors + 1;
            // Stream is unusable: abandon the block but keep the engine
            // clock consistent with what was rendered.
            written_ += (uint32_t)tailFrames_;
            tailFrames_ = 0;
            return false;
        }
        const int frames = n / kBytesPerFrame;
        if (frames == 0)
            return true; // full; try again next iteration
        written_ += (uint32_t)frames;
        tailOffset_ += frames;
        tailFrames_ -= frames;
        if (tailFrames_ > 0)
            stats_.tailRetries = stats_.tailRetries + 1;
    }
    return true;
}

void Ps2Audio::collectHwTriggers()
{
    const int n = engine_->hwTriggerCount();
    for (int i = 0; i < n; ++i) {
        if (pendingCount_ >= kPending) {
            stats_.hwDropped = stats_.hwDropped + (uint32_t)(n - i);
            break;
        }
        const HwTrigger& t = engine_->hwTrigger(i);
        Pending& p = pending_[pendingCount_++];
        p.frame = t.frame;
        p.channel = t.channel;
        p.sample = t.sample;
        p.volume = t.volume;
        p.pan = t.pan;
    }
}

void Ps2Audio::dispatchHw(uint32_t heard)
{
    int keep = 0;
    for (int i = 0; i < pendingCount_; ++i) {
        const Pending& p = pending_[i];
        if ((int32_t)(heard - p.frame) < 0) {
            pending_[keep++] = p; // not due yet
            continue;
        }
        const int lateFrames = (int32_t)(heard - p.frame);
        const uint32_t lateUs = (uint32_t)lateFrames * 1000u / 48u;
        if (lateUs > stats_.hwLateUs)
            stats_.hwLateUs = lateUs;

        const int spuCh = p.channel == 0xff ? kPreviewSpuChannel : cfg::kSpuChannelBase + p.channel;
        if (spuVolume_[spuCh] != (int8_t)p.volume || spuPan_[spuCh] != p.pan) {
            audio_channel_set_volume_and_pan(spuCh, p.volume, p.pan);
            spuVolume_[spuCh] = (int8_t)p.volume;
            spuPan_[spuCh] = p.pan;
        }
        const int32_t handle = __atomic_load_n(&spuHandle_[p.sample], __ATOMIC_ACQUIRE);
        if (handle != AUDIO_SOUND_INVALID) {
            const int rc = audio_channel_play_sound(spuCh, handle);
            if (rc == -AUDIO_ERR_INVALID_ARGUMENT) {
                // The UI unloaded this sample between the check and the call.
                stats_.hwStale = stats_.hwStale + 1;
            } else if (rc < 0) {
                stats_.lastError = rc;
                stats_.rpcErrors = stats_.rpcErrors + 1;
            } else {
                stats_.hwPlayed = stats_.hwPlayed + 1;
            }
        }
    }
    pendingCount_ = keep;
}

void Ps2Audio::run()
{
    for (;;) {
        // Finish handing over a partly accepted block before anything else.
        if (tailFrames_ > 0) {
            const bool ok = submitTail();
            if (ok && tailFrames_ == 0)
                stats_.blocks = stats_.blocks + 1;
            if (!ok)
                ps2sys::sleepUs(10000);
            else if (tailFrames_ > 0)
                ps2sys::sleepUs(1000);
            continue;
        }

        const int queuedBytes = audio_stream_get_queued_bytes();
        if (queuedBytes < 0) {
            stats_.lastError = queuedBytes;
            stats_.rpcErrors = stats_.rpcErrors + 1;
            ps2sys::sleepUs(10000);
            continue;
        }
        const uint32_t queued = (uint32_t)queuedBytes / kBytesPerFrame;
        stats_.queuedFrames = queued;

        // What the listener hears now: everything accepted minus what is
        // still waiting in the queue (unsigned arithmetic wraps correctly;
        // all comparisons below are signed differences).
        const uint32_t heard = written_ - queued;
        stats_.heardFrame = heard;
        if (pendingCount_)
            dispatchHw(heard);

        const int target = latency_;
        if ((int)queued < target) {
            // An empty queue after the first fill means the SPU2 played
            // silence: a genuine, audible underrun.
            if (primed_ && queued == 0)
                stats_.underruns = stats_.underruns + 1;

            const uint64_t t0 = ps2sys::timeUs();
            engine_->render(g_block, kBlockFrames);
            const uint32_t us = (uint32_t)(ps2sys::timeUs() - t0);
            stats_.renderUsLast = us;
            if (us > stats_.renderUsMax)
                stats_.renderUsMax = us;
            stats_.renderUsAvg = (stats_.renderUsAvg * 15 + us) / 16;
            collectHwTriggers();

            tailFrames_ = kBlockFrames;
            tailOffset_ = 0;
            if (submitTail() && tailFrames_ == 0)
                stats_.blocks = stats_.blocks + 1;
            if ((int)queued + kBlockFrames >= target)
                primed_ = true;
            continue; // top up again immediately if still below target
        }

        // Queue is full enough: sleep until roughly one block has drained,
        // but wake early for a due hardware note.
        int sleepUs = (int)(queued - (uint32_t)target) * 1000 / 48 + 1000;
        if (pendingCount_) {
            int32_t soonest = 0x7fffffff;
            for (int i = 0; i < pendingCount_; ++i) {
                const int32_t d = (int32_t)(pending_[i].frame - heard);
                if (d < soonest)
                    soonest = d;
            }
            const int dueUs = soonest <= 0 ? 0 : (int)(soonest * 1000 / 48);
            if (dueUs < sleepUs)
                sleepUs = dueUs;
        }
        if (sleepUs < 500)
            sleepUs = 500;
        if (sleepUs > 8000)
            sleepUs = 8000;
        ps2sys::sleepUs(sleepUs);
    }
}
