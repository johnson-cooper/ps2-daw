#include "audio/wav_export.hpp"

#include <stdio.h>
#include <string.h>

#include "core/strutil.hpp"

namespace wavexport {

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t* p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

void makeHeader(uint8_t* out, uint32_t dataBytes)
{
    memcpy(out, "RIFF", 4);
    put32(out + 4, 36 + dataBytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    put16(out + 20, 1); // PCM
    put16(out + 22, cfg::kOutputChannels);
    put32(out + 24, cfg::kSampleRate);
    put32(out + 28, cfg::kSampleRate * cfg::kOutputChannels * 2);
    put16(out + 32, cfg::kOutputChannels * 2);
    put16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put32(out + 40, dataBytes);
}

int64_t checkHeader(const uint8_t* h, uint32_t fileSize)
{
    if (fileSize < kHeaderBytes || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVEfmt ", 8) != 0 ||
        memcmp(h + 36, "data", 4) != 0)
        return -1;
    if (get32(h + 16) != 16 || h[20] != 1 || h[22] != cfg::kOutputChannels || get32(h + 24) != (uint32_t)cfg::kSampleRate ||
        h[34] != 16)
        return -1;
    const uint32_t data = get32(h + 40);
    if (get32(h + 4) != 36 + data || fileSize != kHeaderBytes + data)
        return -1;
    return (int64_t)data;
}

} // namespace wavexport

Exporter::Exporter(AudioEngine& engine, Session& session, ExportFile& file, RenderHold* hold)
    : engine_(engine), session_(session), file_(file), hold_(hold), state_(State::Idle), source_(Source::Song), held_(false),
      fileOpen_(false), prevSong_(false), prevPattern_(0), framesWritten_(0), expectedFrames_(0), doneAtFrame_(0), silentRun_(0),
      skipped_(0), clipped_(0), peak_(0), pending_(0), tailFrames_(0)
{
    path_[0] = tmpPath_[0] = message_[0] = '\0';
}

void Exporter::fail(const char* msg)
{
    str::copy(message_, sizeof(message_), msg);
    if (fileOpen_) {
        file_.close();
        fileOpen_ = false;
    }
    if (tmpPath_[0])
        file_.remove(tmpPath_);
    restore();
    state_ = State::Failed;
}

void Exporter::restore()
{
    session_.setExportMode(0);
    session_.stop();
    session_.setSongMode(prevSong_);
    session_.selectPattern(prevPattern_);
    if (held_ && hold_) {
        hold_->release();
        held_ = false;
    }
}

bool Exporter::begin(const char* path, Source source, char* err, size_t errCap)
{
    auto bad = [&](const char* m) {
        if (err && errCap)
            snprintf(err, errCap, "%s", m);
        str::copy(message_, sizeof(message_), m);
        state_ = State::Failed;
        return false;
    };
    if (state_ == State::Running)
        return bad("export already running");
    const Project& p = session_.project();
    source_ = source;
    if (source == Source::Song && p.songBars() == 0)
        return bad("playlist is empty: nothing to export");
    const int steps = source == Source::Song ? p.songBars() * cfg::kStepsPerBeat * cfg::kBeatsPerBar : p.pattern().length;
    expectedFrames_ = (uint32_t)((uint64_t)steps * 72000000ull / p.bpmCenti);

    // Longer tails when a delay or reverb is in the chain.
    tailFrames_ = kMinTailFrames;
    for (const auto& t : p.tracks)
        for (const auto& f : t.fx)
            if ((f.type == (uint8_t)FxType::Delay || f.type == (uint8_t)FxType::Reverb) && !f.bypass)
                tailFrames_ = kMaxTailFrames;
    if (expectedFrames_ + tailFrames_ > kMaxFrames)
        return bad("song is longer than 20 minutes");

    str::copy(path_, sizeof(path_), path);
    snprintf(tmpPath_, sizeof(tmpPath_), "%s.TMP", path);
    if (hold_ && !hold_->hold())
        return bad("could not park the audio thread");
    held_ = hold_ != nullptr;
    prevSong_ = p.songMode != 0;
    prevPattern_ = p.currentPattern;

    file_.remove(tmpPath_);
    if (!file_.open(tmpPath_)) {
        if (held_) {
            hold_->release();
            held_ = false;
        }
        return bad("cannot create the file on USB");
    }
    fileOpen_ = true;
    uint8_t hdr[wavexport::kHeaderBytes];
    wavexport::makeHeader(hdr, 0);
    if (!file_.write(hdr, sizeof(hdr))) {
        fail("USB write failed");
        if (err && errCap)
            snprintf(err, errCap, "%s", message_);
        return false;
    }

    framesWritten_ = 0;
    doneAtFrame_ = 0;
    silentRun_ = 0;
    skipped_ = clipped_ = peak_ = 0;
    pending_ = 0;
    message_[0] = '\0';

    // Start the engine from the top in export mode. The commands are queued
    // now and applied by the first render() below.
    session_.stop();
    session_.setSongMode(source == Source::Song);
    if (source == Source::Pattern)
        session_.selectPattern(p.currentPattern);
    session_.setExportMode(1);
    session_.play();
    state_ = State::Running;
    return true;
}

bool Exporter::flush()
{
    if (pending_ == 0)
        return true;
    const bool ok = file_.write(buffer_, pending_);
    pending_ = 0;
    return ok;
}

void Exporter::tick(int maxBlocks)
{
    if (state_ != State::Running)
        return;
    const uint32_t blockFrames = 512;
    for (int b = 0; b < maxBlocks; ++b) {
        if (pending_ + blockFrames * 4 > sizeof(buffer_) && !flush()) {
            fail("USB write failed (disk full or drive removed)");
            return;
        }
        int16_t* out = (int16_t*)(buffer_ + pending_);
        engine_.render(out, (int)blockFrames);
        pending_ += blockFrames * 4;
        framesWritten_ += blockFrames;

        bool silent = true;
        for (uint32_t i = 0; i < blockFrames * 2; ++i) {
            const int v = out[i] < 0 ? -out[i] : out[i];
            if (v) {
                silent = false;
                if ((uint32_t)v > peak_)
                    peak_ = (uint32_t)v;
            }
        }
        silentRun_ = silent ? silentRun_ + blockFrames : 0;

        const EngineStatus& st = engine_.status();
        if (st.exportDone && doneAtFrame_ == 0)
            doneAtFrame_ = framesWritten_;
        skipped_ = st.exportSkipped;
        clipped_ = st.clipSamples;

        if (doneAtFrame_ != 0) {
            const uint32_t tail = framesWritten_ - doneAtFrame_;
            if (tail >= tailFrames_ || (tail >= kMinTailFrames && silentRun_ >= kSilenceEndFrames)) {
                finish();
                return;
            }
        }
        if (framesWritten_ > expectedFrames_ + tailFrames_ + 4 * (uint32_t)cfg::kSampleRate && doneAtFrame_ == 0) {
            fail("render did not finish (engine error)");
            return;
        }
    }
}

void Exporter::finish()
{
    if (!flush()) {
        fail("USB write failed (disk full or drive removed)");
        return;
    }
    uint8_t hdr[wavexport::kHeaderBytes];
    wavexport::makeHeader(hdr, framesWritten_ * 4);
    if (!file_.rewriteHeader(hdr, sizeof(hdr))) {
        fail("could not finalise the WAV header");
        return;
    }
    const bool closed = file_.close();
    fileOpen_ = false;
    if (!closed) {
        fail("closing the file failed");
        return;
    }
    // Verify what is on the stick before replacing anything.
    uint8_t back[wavexport::kHeaderBytes];
    uint32_t size = 0;
    if (!file_.readHeader(tmpPath_, back, sizeof(back), &size) || wavexport::checkHeader(back, size) != (int64_t)framesWritten_ * 4) {
        fail("verification failed: the written file is damaged");
        return;
    }
    if (!file_.rename(tmpPath_, path_)) {
        fail("could not move the finished file into place");
        return;
    }
    restore();
    snprintf(message_, sizeof(message_), "%lu.%02lu s written", (unsigned long)(framesWritten_ / cfg::kSampleRate),
             (unsigned long)((framesWritten_ % cfg::kSampleRate) * 100 / cfg::kSampleRate));
    state_ = State::Done;
}

void Exporter::cancel()
{
    if (state_ != State::Running)
        return;
    fail("export cancelled");
}

int Exporter::progressPermille() const
{
    if (state_ == State::Done)
        return 1000;
    const uint32_t total = expectedFrames_ + tailFrames_;
    if (total == 0)
        return 0;
    const uint64_t p = (uint64_t)framesWritten_ * 1000 / total;
    return p > 999 ? 999 : (int)p;
}
