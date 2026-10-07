#include "project/project_io.hpp"

#include <stdio.h>
#include <string.h>

#include "core/strutil.hpp"

namespace projectio {
namespace {

const uint8_t kMagic[8] = {'P', 'S', '2', 'D', 'A', 'W', 'P', 'J'};
constexpr size_t kHeaderBytes = 12; // magic + u16 version + u16 min reader

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap), pos_(0), ok_(true) {}
    void u8(uint8_t v)
    {
        if (pos_ + 1 > cap_) {
            ok_ = false;
            return;
        }
        buf_[pos_++] = v;
    }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void bytes(const void* p, size_t n)
    {
        if (pos_ + n > cap_) {
            ok_ = false;
            return;
        }
        memcpy(buf_ + pos_, p, n);
        pos_ += n;
    }
    void str(const char* s, size_t maxLen)
    {
        size_t n = strnlen(s, maxLen);
        if (n > 255)
            n = 255;
        u8((uint8_t)n);
        bytes(s, n);
    }
    // Chunk framing: write tag + placeholder size, patch on end.
    size_t begin(const char tag[4])
    {
        bytes(tag, 4);
        size_t at = pos_;
        u32(0);
        return at;
    }
    void end(size_t at)
    {
        if (!ok_)
            return;
        const uint32_t size = (uint32_t)(pos_ - at - 4);
        buf_[at] = (uint8_t)size;
        buf_[at + 1] = (uint8_t)(size >> 8);
        buf_[at + 2] = (uint8_t)(size >> 16);
        buf_[at + 3] = (uint8_t)(size >> 24);
    }
    size_t pos() const { return pos_; }
    bool ok() const { return ok_; }
    uint8_t* data() { return buf_; }

private:
    uint8_t* buf_;
    size_t cap_, pos_;
    bool ok_;
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n), pos_(0), ok_(true) {}
    uint8_t u8()
    {
        if (pos_ + 1 > n_) {
            ok_ = false;
            return 0;
        }
        return d_[pos_++];
    }
    uint16_t u16() { uint16_t lo = u8(); return (uint16_t)(lo | (u8() << 8)); }
    uint32_t u32() { uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
    void str(char* dst, size_t cap)
    {
        const size_t n = u8();
        if (pos_ + n > n_) {
            ok_ = false;
            dst[0] = '\0';
            return;
        }
        const size_t keep = n < cap - 1 ? n : cap - 1;
        for (size_t i = 0; i < keep; ++i) {
            const char c = (char)d_[pos_ + i];
            dst[i] = (c >= 32 && c < 127) ? c : '?'; // only printable ASCII reaches the UI
        }
        dst[keep] = '\0';
        pos_ += n;
    }
    void skip(size_t n)
    {
        if (pos_ + n > n_) {
            ok_ = false;
            pos_ = n_;
            return;
        }
        pos_ += n;
    }
    size_t pos() const { return pos_; }
    size_t remaining() const { return n_ - pos_; }
    bool ok() const { return ok_; }

private:
    const uint8_t* d_;
    size_t n_, pos_;
    bool ok_;
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool fail(char* err, size_t cap, const char* msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
    return false;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t save(const Project& p, uint8_t* buf, size_t cap)
{
    Writer w(buf, cap);
    w.bytes(kMagic, sizeof(kMagic));
    w.u16(kFormatVersion);
    w.u16(1); // oldest reader version that can load this file

    size_t c = w.begin("PROJ");
    w.str(p.name, sizeof(p.name));
    w.u32(p.bpmCenti);
    w.u8(p.masterVolume);
    w.u8(p.currentPattern);
    w.u8(p.channelCount);
    w.end(c);

    for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
        const ChannelData& cd = p.channels[ch];
        c = w.begin("CHAN");
        w.u8((uint8_t)ch);
        w.str(cd.name, sizeof(cd.name));
        w.str(cd.sampleRef, sizeof(cd.sampleRef));
        w.u8(cd.volume);
        w.u8((uint8_t)cd.pan);
        w.u8((uint8_t)((cd.mute ? 1 : 0) | (cd.solo ? 2 : 0)));
        w.u8(cd.voiceMode);
        w.u8(cd.route);
        w.u8(cd.gate ? 1 : 0); // trailing optional field
        w.end(c);
    }

    for (int pi = 0; pi < cfg::kMaxPatterns; ++pi) {
        const PatternData& pd = p.patterns[pi];
        c = w.begin("PATT");
        w.u8((uint8_t)pi);
        w.str(pd.name, sizeof(pd.name));
        w.u8(pd.length);
        w.u8((uint8_t)cfg::kMaxChannels);
        w.u8((uint8_t)cfg::kMaxSteps);
        for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
            w.bytes(pd.velocity[ch], cfg::kMaxSteps);
        w.end(c);
        for (int ch = 0; ch < cfg::kMaxChannels; ++ch) {
            const int n = pd.noteCount[ch] > cfg::kMaxNotes ? cfg::kMaxNotes : pd.noteCount[ch];
            if (n == 0)
                continue;
            c = w.begin("NOTE");
            w.u8((uint8_t)pi);
            w.u8((uint8_t)ch);
            w.u8((uint8_t)n);
            for (int i = 0; i < n; ++i) {
                w.u8(pd.notes[ch][i].step);
                w.u8(pd.notes[ch][i].pitch);
                w.u8(pd.notes[ch][i].velocity);
                w.u8(pd.notes[ch][i].length);
            }
            w.end(c);
        }
    }

    c = w.begin("PLST");
    const uint16_t clips = p.clipCount > Project::kMaxClips ? Project::kMaxClips : p.clipCount;
    w.u16(clips);
    for (uint16_t i = 0; i < clips; ++i) {
        w.u8(p.clips[i].track);
        w.u8(p.clips[i].pattern);
        w.u16(p.clips[i].startBar);
        w.u16(p.clips[i].lengthBars);
    }
    w.u8(p.songMode ? 1 : 0); // trailing optional fields: older files simply end after the clips
    w.u8(p.trackMute);
    w.u8(p.trackSolo);
    w.end(c);

    // Trailer: CRC32 of everything before the END chunk.
    const uint32_t crc = w.ok() ? crc32(w.data(), w.pos()) : 0;
    c = w.begin("END ");
    w.u32(crc);
    w.end(c);

    return w.ok() ? w.pos() : 0;
}

bool load(const uint8_t* data, size_t size, Project& out, char* err, size_t errCap)
{
    out.resetEmpty();
    if (!data || size < kHeaderBytes + 8)
        return fail(err, errCap, "file too small");
    if (size > kMaxFileBytes)
        return fail(err, errCap, "file too large");
    if (memcmp(data, kMagic, sizeof(kMagic)) != 0)
        return fail(err, errCap, "not a .ps2daw project");

    Reader r(data, size);
    r.skip(sizeof(kMagic));
    const uint16_t version = r.u16();
    const uint16_t minReader = r.u16();
    if (minReader > kFormatVersion) {
        char msg[64];
        snprintf(msg, sizeof(msg), "needs newer PS2 DAW (format v%u)", (unsigned)version);
        return fail(err, errCap, msg);
    }

    Project p;
    p.resetEmpty();
    bool sawEnd = false, sawProj = false;

    while (r.remaining() >= 8 && !sawEnd) {
        const size_t chunkStart = r.pos();
        char tag[4];
        for (char& t : tag)
            t = (char)r.u8();
        const uint32_t len = r.u32();
        if (!r.ok() || len > r.remaining())
            return fail(err, errCap, "truncated chunk");
        const size_t bodyStart = r.pos();
        Reader b(data + bodyStart, len);

        if (memcmp(tag, "PROJ", 4) == 0) {
            b.str(p.name, sizeof(p.name));
            p.bpmCenti = (uint32_t)clampi((int)b.u32(), (int)cfg::kMinBpmCenti, (int)cfg::kMaxBpmCenti);
            p.masterVolume = (uint8_t)clampi(b.u8(), 0, 100);
            p.currentPattern = (uint8_t)clampi(b.u8(), 0, cfg::kMaxPatterns - 1);
            p.channelCount = (uint8_t)clampi(b.u8(), 1, cfg::kMaxChannels);
            sawProj = b.ok();
        } else if (memcmp(tag, "CHAN", 4) == 0) {
            const int ch = b.u8();
            if (ch < cfg::kMaxChannels) {
                ChannelData& cd = p.channels[ch];
                b.str(cd.name, sizeof(cd.name));
                b.str(cd.sampleRef, sizeof(cd.sampleRef));
                cd.sampleSlot = -1; // resolved against the sample bank later
                cd.volume = (uint8_t)clampi(b.u8(), 0, 100);
                cd.pan = (int8_t)clampi((int8_t)b.u8(), -100, 100);
                const uint8_t flags = b.u8();
                cd.mute = flags & 1;
                cd.solo = (flags >> 1) & 1;
                const uint8_t mode = b.u8();
                cd.voiceMode = mode == (uint8_t)VoiceMode::Spu2 ? mode : (uint8_t)VoiceMode::Software;
                cd.route = b.u8();
                if (!b.ok())
                    return fail(err, errCap, "corrupt channel chunk");
                cd.gate = b.remaining() >= 1 ? (b.u8() ? 1 : 0) : 0;
            }
        } else if (memcmp(tag, "PATT", 4) == 0) {
            const int pi = b.u8();
            if (pi < cfg::kMaxPatterns) {
                PatternData& pd = p.patterns[pi];
                b.str(pd.name, sizeof(pd.name));
                pd.length = (uint8_t)clampi(b.u8(), 1, cfg::kMaxSteps);
                const int chans = b.u8();
                const int steps = b.u8();
                for (int ch = 0; ch < chans; ++ch) {
                    for (int s = 0; s < steps; ++s) {
                        const uint8_t v = b.u8();
                        if (ch < cfg::kMaxChannels && s < cfg::kMaxSteps)
                            pd.velocity[ch][s] = v > 127 ? 127 : v;
                    }
                }
                if (!b.ok())
                    return fail(err, errCap, "corrupt pattern chunk");
            }
        } else if (memcmp(tag, "NOTE", 4) == 0) {
            const int pi = b.u8();
            const int ch = b.u8();
            const int count = b.u8();
            if (pi < cfg::kMaxPatterns && ch < cfg::kMaxChannels) {
                PatternData& pd = p.patterns[pi];
                int kept = 0;
                for (int i = 0; i < count; ++i) {
                    PianoNote n;
                    n.step = b.u8();
                    n.pitch = b.u8();
                    n.velocity = b.u8();
                    n.length = b.u8();
                    if (!b.ok())
                        return fail(err, errCap, "corrupt note chunk");
                    // Drop what cannot be played; clamp what can.
                    if (n.step >= cfg::kMaxSteps || n.pitch > 127 || n.velocity == 0 || kept >= cfg::kMaxNotes)
                        continue;
                    if (n.velocity > 127)
                        n.velocity = 127;
                    if (n.length < 1)
                        n.length = 1;
                    if (n.length > cfg::kMaxSteps)
                        n.length = cfg::kMaxSteps;
                    pd.notes[ch][kept++] = n;
                }
                pd.noteCount[ch] = (uint8_t)kept;
            }
        } else if (memcmp(tag, "PLST", 4) == 0) {
            uint16_t n = b.u16();
            if (n > Project::kMaxClips)
                n = Project::kMaxClips;
            for (uint16_t i = 0; i < n; ++i) {
                PlaylistClip& clip = p.clips[i];
                clip.track = b.u8();
                clip.pattern = (uint8_t)clampi(b.u8(), 0, cfg::kMaxPatterns - 1);
                clip.startBar = b.u16();
                clip.lengthBars = b.u16();
            }
            if (!b.ok())
                return fail(err, errCap, "corrupt playlist chunk");
            p.clipCount = n;
            p.songMode = b.remaining() >= 1 ? (b.u8() ? 1 : 0) : 0;
            if (b.remaining() >= 2) {
                p.trackMute = b.u8() & ((1u << cfg::kPlaylistTracks) - 1);
                p.trackSolo = b.u8() & ((1u << cfg::kPlaylistTracks) - 1);
            }
            p.sanitizeClips();
        } else if (memcmp(tag, "END ", 4) == 0) {
            const uint32_t stored = b.u32();
            if (!b.ok() || stored != crc32(data, chunkStart))
                return fail(err, errCap, "checksum mismatch (file damaged)");
            sawEnd = true;
        }
        // Unknown chunks are skipped for forward compatibility.
        r.skip(len);
    }

    if (!sawProj)
        return fail(err, errCap, "missing PROJ chunk");
    if (!sawEnd)
        return fail(err, errCap, "missing END chunk (truncated)");
    out = p;
    return true;
}

} // namespace projectio
