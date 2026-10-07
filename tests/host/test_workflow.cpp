// Host tests for the project workflow: queued pattern switching, pattern
// copy/duplicate, and crash-safe slot saves.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "audio/audio_engine.hpp"
#include "audio/drum_synth.hpp"
#include "project/project_io.hpp"
#include "project/session.hpp"
#include "project/slot_store.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static bool noYield() { return false; }

struct H {
    SampleBank bank;
    AudioEngine engine;
    Session session;
    std::vector<StepMark> marks;
    uint32_t seen = 0;

    H() : session(engine, bank, noYield)
    {
        engine.setSampleBank(&bank);
        drumsynth::generateKit(bank);
        Project p;
        p.resetEmpty(); // empty patterns: only the marks matter
        session.loadProject(p);
        session.setBpmCenti(12000);
    }
    void render(uint32_t frames)
    {
        int16_t buf[cfg::kMaxBlockFrames * 2];
        uint32_t done = 0;
        while (done < frames) {
            const int n = (int)(frames - done < 480 ? frames - done : 480);
            engine.render(buf, n);
            const EngineStatus& st = engine.status();
            while (seen < st.markSerial)
                marks.push_back(st.marks[seen++ % EngineStatus::kMarks]);
            done += (uint32_t)n;
        }
    }
    // Renders until the engine has fired `steps` more steps.
    void steps(size_t steps)
    {
        const size_t target = marks.size() + steps;
        while (marks.size() < target)
            render(480);
    }
};

static void testQueuedSwitch()
{
    // 120 BPM: a step is 6000 frames; a bar of 16 steps is 96000 frames.
    {
        H h;
        h.session.setSwitchMode(SwitchMode::NextBar);
        h.session.play();
        h.steps(6); // played steps 0..5 of pattern 1
        CHECK(h.marks.back().step == 5 && h.marks.back().pattern == 0);
        h.session.selectPattern(1);
        CHECK(h.session.project().currentPattern == 1); // the editor moves at once
        h.render(480);                                  // command applied
        CHECK(h.engine.status().queuedPattern == 1 && h.engine.status().pattern == 0);
        h.steps(10); // up to and including step 15
        for (size_t i = 6; i < h.marks.size(); ++i)
            CHECK(h.marks[i].pattern == 0);
        h.steps(1); // step 0 of the next bar
        CHECK(h.marks.back().step == 0 && h.marks.back().pattern == 1);
        CHECK(h.engine.status().queuedPattern == 0xff);
    }
    {
        H h; // next beat: the next multiple of 4
        h.session.setSwitchMode(SwitchMode::NextBeat);
        h.session.play();
        h.steps(6);
        h.session.selectPattern(2);
        h.steps(3); // 6, 7 still pattern 0; 8 switches
        CHECK(h.marks[6].pattern == 0 && h.marks[7].pattern == 0);
        CHECK(h.marks[8].step == 8 && h.marks[8].pattern == 2);
    }
    {
        H h; // immediate: the very next step
        h.session.setSwitchMode(SwitchMode::Immediate);
        h.session.play();
        h.steps(6);
        h.session.selectPattern(3);
        h.steps(2);
        CHECK(h.marks.back().pattern == 3);
    }
    {
        H h; // target shorter than the current position: wait for the loop
        h.session.setPatternLength(0, 32);
        h.session.setPatternLength(1, 8);
        h.session.setSwitchMode(SwitchMode::NextBar);
        h.session.play();
        h.steps(14);
        h.session.selectPattern(1);
        h.steps(4); // steps 14..17: step 16 is a bar line but pattern 2 has only 8 steps
        for (size_t i = 14; i < h.marks.size(); ++i)
            CHECK(h.marks[i].pattern == 0);
        h.steps(16); // runs to the loop point
        bool switched = false;
        for (size_t i = 0; i < h.marks.size(); ++i)
            if (h.marks[i].pattern == 1) {
                CHECK(h.marks[i].step == 0 && i > 20);
                switched = true;
                break;
            }
        CHECK(switched);
    }
    {
        H h; // stopped: switches at once; a queue pending at Stop is applied
        h.session.setSwitchMode(SwitchMode::NextBar);
        h.session.selectPattern(4);
        h.render(480);
        CHECK(h.engine.status().pattern == 4 && h.engine.status().queuedPattern == 0xff);
        h.session.play();
        h.steps(3);
        h.session.selectPattern(5);
        h.render(480);
        CHECK(h.engine.status().queuedPattern == 5);
        h.session.stop();
        h.render(480);
        CHECK(h.engine.status().pattern == 5 && h.engine.status().queuedPattern == 0xff);
        // A later immediate selection cancels nothing stale.
        h.session.setSwitchMode(SwitchMode::Immediate);
        h.session.play();
        h.steps(2);
        CHECK(h.marks.back().pattern == 5);
    }
}

static int countSteps(const Session& s, int pattern)
{
    int n = 0;
    for (int ch = 0; ch < cfg::kMaxChannels; ++ch)
        for (int st = 0; st < cfg::kMaxSteps; ++st)
            n += s.project().patterns[pattern].velocity[ch][st] != 0;
    return n;
}

static void testPatternOps()
{
    H h;
    Session& s = h.session;
    s.setStep(0, 0, 0, 100);
    s.setStep(0, 0, 8, 90);
    s.setStep(0, 3, 15, 70);
    s.setPatternLength(0, 24);
    CHECK(s.patternIsEmpty(1) && !s.patternIsEmpty(0));

    CHECK(s.copyPattern(0, 2));
    CHECK(countSteps(s, 2) == 3 && s.project().patterns[2].length == 24);
    CHECK(s.project().patterns[2].velocity[3][15] == 70);
    CHECK(strcmp(s.project().patterns[2].name, "PATTERN 3") == 0); // copy keeps the target's name
    CHECK(!s.copyPattern(0, 8) && !s.copyPattern(-1, 1));
    CHECK(s.copyPattern(2, 2)); // no-op, no damage
    CHECK(countSteps(s, 2) == 3);

    // The engine received the copy: pattern 3 plays like pattern 1.
    s.selectPattern(2);
    s.play();
    h.steps(18);
    CHECK(h.engine.status().triggerCount[0] >= 2 && h.engine.status().triggerCount[3] >= 1);

    // Duplicate goes into the next empty pattern and selects it.
    s.setPatternName(0, "VERSE");
    const int d = s.duplicatePattern(0);
    CHECK(d == 1 && s.project().currentPattern == 1);
    CHECK(countSteps(s, 1) == 3 && strcmp(s.project().patterns[1].name, "VERSE+") == 0);
    CHECK(s.duplicatePattern(0) == 3); // pattern 2 (index 1) is taken now; 3rd index 2 holds a copy
    // Fill every pattern: nothing left to duplicate into.
    for (int p = 0; p < cfg::kMaxPatterns; ++p)
        s.setStep(p, 0, 0, 100);
    CHECK(s.duplicatePattern(0) == -1);

    // Clearing keeps length and name.
    s.clearPattern(0);
    CHECK(s.patternIsEmpty(0) && s.project().patterns[0].length == 24 && strcmp(s.project().patterns[0].name, "VERSE") == 0);

    // Names survive a save/load round trip.
    static uint8_t buf[projectio::kMaxFileBytes];
    const size_t n = projectio::save(s.project(), buf, sizeof(buf));
    Project q;
    char err[64];
    CHECK(n && projectio::load(buf, n, q, err, sizeof(err)) && strcmp(q.patterns[1].name, "VERSE+") == 0);
    CHECK(q.patterns[2].length == 24);
}

// ---- slot store ----------------------------------------------------------------
struct FakeFs : ProjectFiles {
    std::map<std::string, std::vector<uint8_t>> files;
    int writes = 0, failWriteAt = -1;      // the Nth write fails (partial file left behind)
    int renames = 0, failRenameAt = -1;    // the Nth rename fails
    bool corruptWrites = false;            // writes succeed but store damaged bytes

    int readAll(const char* p, uint8_t* buf, size_t cap) override
    {
        auto it = files.find(p);
        if (it == files.end())
            return -1;
        if (it->second.size() > cap)
            return -2;
        memcpy(buf, it->second.data(), it->second.size());
        return (int)it->second.size();
    }
    bool writeAll(const char* p, const uint8_t* d, size_t n) override
    {
        ++writes;
        if (writes == failWriteAt) {
            files[p] = std::vector<uint8_t>(d, d + n / 2); // torn write
            return false;
        }
        std::vector<uint8_t> v(d, d + n);
        if (corruptWrites)
            v[v.size() / 2] ^= 0xff;
        files[p] = v;
        return true;
    }
    bool renameTo(const char* a, const char* b) override
    {
        ++renames;
        if (renames == failRenameAt)
            return false;
        auto it = files.find(a);
        if (it == files.end())
            return false;
        files[b] = it->second;
        files.erase(a);
        return true;
    }
    bool removeFile(const char* p) override { return files.erase(p) > 0; }
    bool exists(const char* p) override { return files.count(p) > 0; }
};

static void testSlotStore()
{
    static uint8_t a[projectio::kMaxFileBytes], b[projectio::kMaxFileBytes];
    const char* dir = "mass0:/PS2DAW";
    FakeFs fs;
    Project p1, p2, out;
    p1.resetDemo();
    strcpy(p1.name, "FIRST");
    p2.resetDemo();
    strcpy(p2.name, "SECOND");
    strcpy(p2.channels[1].sampleRef, "samples:A/SNARE.WAV");
    char err[64];
    bool bak = false;

    CHECK(slotstore::save(fs, dir, 1, p1, a, b, sizeof(a), err, sizeof(err)));
    CHECK(fs.exists("mass0:/PS2DAW/SLOT1.ps2daw") && !fs.exists("mass0:/PS2DAW/SLOT1.TMP") && !fs.exists("mass0:/PS2DAW/SLOT1.BAK"));
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.name, "FIRST") == 0 && !bak);

    // Second save rotates the first into .BAK.
    CHECK(slotstore::save(fs, dir, 1, p2, a, b, sizeof(a), err, sizeof(err)));
    CHECK(fs.exists("mass0:/PS2DAW/SLOT1.BAK") && !fs.exists("mass0:/PS2DAW/SLOT1.TMP"));
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.name, "SECOND") == 0);
    Project bakProject;
    CHECK(projectio::load(fs.files["mass0:/PS2DAW/SLOT1.BAK"].data(), fs.files["mass0:/PS2DAW/SLOT1.BAK"].size(), bakProject, err, sizeof(err)) &&
          strcmp(bakProject.name, "FIRST") == 0);

    // A write error mid-save (torn temp file): the good save is untouched, no litter.
    Project p3 = p2;
    strcpy(p3.name, "THIRD");
    fs.failWriteAt = fs.writes + 1;
    CHECK(!slotstore::save(fs, dir, 1, p3, a, b, sizeof(a), err, sizeof(err)));
    CHECK(strstr(err, "write") != nullptr);
    CHECK(!fs.exists("mass0:/PS2DAW/SLOT1.TMP"));
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.name, "SECOND") == 0 && !bak);

    // Silent corruption on write is caught by the read-back before any swap.
    fs.corruptWrites = true;
    CHECK(!slotstore::save(fs, dir, 1, p3, a, b, sizeof(a), err, sizeof(err)));
    CHECK(strstr(err, "verify") != nullptr);
    fs.corruptWrites = false;
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.name, "SECOND") == 0);
    CHECK(strcmp(bakProject.name, "FIRST") == 0 && fs.exists("mass0:/PS2DAW/SLOT1.BAK")); // backup not disturbed

    // Failing to rotate or to finalize keeps a loadable slot.
    fs.failRenameAt = fs.renames + 1;
    CHECK(!slotstore::save(fs, dir, 1, p3, a, b, sizeof(a), err, sizeof(err)));
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.name, "SECOND") == 0);
    CHECK(!fs.exists("mass0:/PS2DAW/SLOT1.TMP"));
    fs.failRenameAt = fs.renames + 2; // main->bak succeeds, tmp->main fails: previous file restored
    CHECK(!slotstore::save(fs, dir, 1, p3, a, b, sizeof(a), err, sizeof(err)));
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.name, "SECOND") == 0);
    fs.failRenameAt = -1;
    CHECK(slotstore::save(fs, dir, 1, p3, a, b, sizeof(a), err, sizeof(err))); // healthy again: THIRD current, SECOND backup

    // Main file damaged later (bad sector, pulled stick): the .BAK is used and reported.
    std::vector<uint8_t>& mainFile = fs.files["mass0:/PS2DAW/SLOT1.ps2daw"];
    mainFile[mainFile.size() / 2] ^= 0x55;
    CHECK(slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)) && bak && strcmp(out.name, "SECOND") == 0);
    const slotstore::Info info = slotstore::peek(fs, dir, 1, a, sizeof(a));
    CHECK(info.exists && info.fromBackup && !info.corrupt && strcmp(info.name, "SECOND") == 0);
    // Both damaged: reported, nothing crashes, `out` stays a valid empty project.
    std::vector<uint8_t>& bakFile = fs.files["mass0:/PS2DAW/SLOT1.BAK"];
    bakFile.resize(10);
    CHECK(!slotstore::load(fs, dir, 1, out, a, sizeof(a), &bak, err, sizeof(err)));
    CHECK(err[0] != '\0');
    CHECK(slotstore::peek(fs, dir, 1, a, sizeof(a)).corrupt);

    // Empty slots and slot numbers outside 1..8.
    CHECK(!slotstore::peek(fs, dir, 5, a, sizeof(a)).exists);
    CHECK(!slotstore::load(fs, dir, 5, out, a, sizeof(a), &bak, err, sizeof(err)));
    CHECK(!slotstore::save(fs, dir, 0, p1, a, b, sizeof(a), err, sizeof(err)));
    CHECK(!slotstore::save(fs, dir, 9, p1, a, b, sizeof(a), err, sizeof(err)));

    // External sample references round-trip through a slot and are counted.
    CHECK(slotstore::save(fs, dir, 2, p2, a, b, sizeof(a), err, sizeof(err)));
    const slotstore::Info i2 = slotstore::peek(fs, dir, 2, a, sizeof(a));
    CHECK(i2.exists && i2.externalSamples == 1 && i2.bpmCenti == p2.bpmCenti);
    CHECK(slotstore::load(fs, dir, 2, out, a, sizeof(a), &bak, err, sizeof(err)) && strcmp(out.channels[1].sampleRef, "samples:A/SNARE.WAV") == 0);
}

int runWorkflowTests()
{
    printf("running workflow tests\n");
    testQueuedSwitch();
    testPatternOps();
    testSlotStore();
    return g_fail;
}

// ---- playlist ---------------------------------------------------------------------
// Sets one pattern step through the Session, like the UI does.
static void note(H& h, int pattern, int ch, int step, int vel = 100) { h.session.setStep(pattern, ch, step, vel); }

static void testSongPlayback()
{
    {
        H h; // bars 1-2: P1 (kick on step 0, looped), bar 3: P2 (snare on step 4)
        note(h, 0, 0, 0);
        note(h, 1, 1, 4);
        CHECK(h.session.placeClip(0, 0, 0, 2));
        CHECK(h.session.placeClip(0, 2, 1, 1));
        h.session.setSongMode(true);
        h.session.play();
        h.steps(48);
        CHECK(h.marks.size() == 48);
        for (size_t i = 0; i < 48; ++i)
            CHECK(h.marks[i].songStep == i); // a linear timeline, not a pattern loop
        CHECK(h.engine.status().triggerCount[0] == 2); // step 0 of bar 1 and of bar 2 (pattern looped inside the clip)
        CHECK(h.engine.status().triggerCount[1] == 1);
        CHECK(h.engine.status().songMode == 1 && h.engine.status().songBars == 3);
        h.steps(48); // the song loops at its end
        CHECK(h.marks[48].songStep == 0);
        CHECK(h.engine.status().triggerCount[0] == 4 && h.engine.status().triggerCount[1] == 2);
        // The pattern shown as sounding follows the clips.
        CHECK(h.marks[0].pattern == 0 && h.marks[36].pattern == 1 && h.marks[36].step == 4);
    }
    {
        H h; // overlapping clips on different tracks play together; one hit per channel (the louder)
        note(h, 0, 0, 0, 60);
        note(h, 1, 0, 0, 120);
        note(h, 1, 2, 0, 100);
        h.session.placeClip(0, 0, 0, 1);
        h.session.placeClip(1, 0, 1, 1);
        h.session.setSongMode(true);
        h.session.play();
        h.steps(16);
        CHECK(h.engine.status().triggerCount[0] == 1);
        CHECK(h.engine.status().triggerCount[2] == 1);
    }
    {
        H h; // empty bars are silent but time still advances
        note(h, 0, 0, 0);
        h.session.placeClip(0, 2, 0, 1); // song = 3 bars, the first two empty
        h.session.setSongMode(true);
        h.session.play();
        h.steps(32);
        CHECK(h.engine.status().triggerCount[0] == 0);
        h.steps(16);
        CHECK(h.engine.status().triggerCount[0] == 1);
    }
    {
        H h; // a pattern shorter than a bar repeats inside its clip
        note(h, 0, 0, 0);
        h.session.setPatternLength(0, 8);
        h.session.placeClip(0, 0, 0, 1);
        h.session.setSongMode(true);
        h.session.play();
        h.steps(16);
        CHECK(h.engine.status().triggerCount[0] == 2);
        CHECK(h.session.patternBars(0) == 1);
        h.session.setPatternLength(0, 17);
        CHECK(h.session.patternBars(0) == 2);
    }
    {
        H h; // pattern mode ignores the playlist; an empty playlist falls back to the pattern
        note(h, 0, 0, 0);
        h.session.placeClip(0, 0, 1, 1);
        h.session.setSongMode(false);
        h.session.play();
        h.steps(40);
        CHECK(h.marks[39].songStep == 39 % 16 && h.engine.status().songMode == 0);
        CHECK(h.engine.status().triggerCount[0] == 3);
        h.session.clearPlaylist();
        h.session.setSongMode(true); // song mode on, nothing to play
        h.steps(16);
        CHECK(h.engine.status().songMode == 0); // the engine keeps looping the pattern
        CHECK(h.engine.status().triggerCount[0] >= 4);
    }
    {
        H h; // editing while playing: shrinking the song under the playhead wraps safely
        note(h, 0, 0, 0);
        h.session.placeClip(0, 0, 0, 4);
        h.session.setSongMode(true);
        h.session.play();
        h.steps(40); // inside bar 3
        h.session.setClipLength(0, 1);
        h.steps(40);
        for (size_t i = 41; i < h.marks.size(); ++i)
            CHECK(h.marks[i].songStep < 16);
        h.session.stop();
        h.render(480);
        h.session.setSongMode(false); // leaving song mode restores the edited pattern
        h.session.selectPattern(5);
        h.session.play();
        h.steps(2);
        CHECK(h.marks.back().pattern == 5);
    }
}

static void testClipEditing()
{
    H h;
    Session& s = h.session;
    const Project& p = s.project();

    CHECK(!s.placeClip(-1, 0, 0, 1) && !s.placeClip(cfg::kPlaylistTracks, 0, 0, 1) && !s.placeClip(0, -1, 0, 1));
    CHECK(!s.placeClip(0, cfg::kMaxSongBars, 0, 1) && !s.placeClip(0, 0, cfg::kMaxPatterns, 1) && !s.placeClip(0, 0, 0, 0));
    CHECK(p.clipCount == 0);

    CHECK(s.placeClip(0, 4, 1, 4)); // bars 5-8
    CHECK(s.placeClip(0, 0, 2, 2)); // bars 1-2, sorted before the first
    CHECK(p.clipCount == 2 && p.clips[0].startBar == 0 && p.clips[1].startBar == 4);

    // Placing over an existing clip replaces it: one clip per track and bar.
    CHECK(s.placeClip(0, 6, 3, 4)); // overlaps bars 7-8 of the clip at 5-8
    CHECK(p.clipCount == 2 && p.clipAt(0, 5) < 0 && p.clips[p.clipAt(0, 6)].pattern == 3);
    // Different tracks may overlap freely.
    CHECK(s.placeClip(1, 0, 4, 8));
    CHECK(p.clipCount == 3 && p.songBars() == 10);

    // Lengths clamp to the next clip on the track and to the song limit.
    CHECK(s.setClipLength(p.clipAt(0, 0), 10) == 6); // the clip at bar 1 can grow until bar 7
    CHECK(s.setClipLength(p.clipAt(0, 0), 0) == 1);
    CHECK(s.setClipLength(p.clipAt(1, 0), 500) == cfg::kMaxSongBars);
    CHECK(s.setClipLength(99, 4) == 0);
    CHECK(s.placeClip(2, cfg::kMaxSongBars - 2, 0, 10)); // clamped, not rejected
    CHECK(p.clips[p.clipAt(2, cfg::kMaxSongBars - 1)].lengthBars == 2);

    // Remove / clear.
    const int n = p.clipCount;
    CHECK(!s.removeClip(-1) && !s.removeClip(n) && s.removeClip(p.clipAt(2, cfg::kMaxSongBars - 1)));
    CHECK(p.clipCount == n - 1);
    s.clearTrack(0);
    CHECK(p.clipAt(0, 0) < 0 && p.clipAt(0, 6) < 0 && p.clipAt(1, 0) >= 0);
    s.clearPlaylist();
    CHECK(p.clipCount == 0 && p.songBars() == 0);

    // The clip table has a hard limit; a rejected placement changes nothing.
    int placed = 0;
    for (int t = 0; t < cfg::kPlaylistTracks; ++t)
        for (int b = 0; b < cfg::kMaxSongBars && placed < cfg::kMaxClips; b += 2)
            placed += s.placeClip(t, b, t % cfg::kMaxPatterns, 1);
    CHECK(placed == cfg::kMaxClips && p.clipCount == cfg::kMaxClips);
    const int before = p.clipCount;
    CHECK(!s.placeClip(cfg::kPlaylistTracks - 1, 127, 0, 1)); // a free bar, but the table is full
    CHECK(p.clipCount == before && p.clipAt(cfg::kPlaylistTracks - 1, 127) < 0);
    // Replacing an existing clip still works when full.
    CHECK(s.placeClip(0, 0, 7, 1) && p.clipCount == before && p.clips[p.clipAt(0, 0)].pattern == 7);
}

static void testPlaylistProject()
{
    static uint8_t buf[projectio::kMaxFileBytes];
    Project a;
    a.resetDemo();
    a.songMode = 1;
    a.clipCount = 0;
    auto add = [&](int track, int pattern, int start, int len) {
        PlaylistClip& c = a.clips[a.clipCount++];
        c.track = (uint8_t)track;
        c.pattern = (uint8_t)pattern;
        c.startBar = (uint16_t)start;
        c.lengthBars = (uint16_t)len;
    };
    add(0, 0, 8, 2);
    add(1, 3, 0, 4);
    add(0, 1, 0, 8);
    size_t n = projectio::save(a, buf, sizeof(buf));
    Project b;
    char err[64];
    CHECK(n && projectio::load(buf, n, b, err, sizeof(err)));
    CHECK(b.songMode == 1 && b.clipCount == 3);
    CHECK(b.clips[0].startBar == 0 && b.clips[2].startBar == 8); // sorted on load
    CHECK(b.songBars() == 10);

    // Hostile or broken clips are rejected or repaired, never trusted.
    a.clipCount = 0;
    add(9, 0, 0, 1);       // track out of range
    add(0, 9, 0, 1);       // pattern out of range (the loader clamps patterns; sanitize drops bad ones)
    add(0, 0, 0, 0);       // empty clip
    add(0, 0, 500, 1);     // beyond the song limit
    add(1, 1, 120, 60000); // far too long: clamped to the limit
    add(2, 2, 0, 4);
    add(2, 3, 2, 4);       // overlaps the previous clip on the same track: dropped
    n = projectio::save(a, buf, sizeof(buf));
    CHECK(n && projectio::load(buf, n, b, err, sizeof(err)));
    CHECK(b.clipAt(1, 120) >= 0 && b.clips[b.clipAt(1, 120)].lengthBars == cfg::kMaxSongBars - 120);
    CHECK(b.clipAt(2, 0) >= 0 && b.clips[b.clipAt(2, 0)].pattern == 2 && b.clipAt(2, 5) < 0);
    CHECK(b.clipAt(9, 0) < 0 && b.clipAt(0, 500) < 0);
    CHECK(b.songBars() <= cfg::kMaxSongBars);
    for (int i = 0; i < b.clipCount; ++i)
        CHECK(b.clips[i].track < cfg::kPlaylistTracks && b.clips[i].pattern < cfg::kMaxPatterns && b.clips[i].lengthBars > 0);

    // A file written before song mode existed (PLST without the trailing byte) still loads.
    a.clipCount = 0;
    add(0, 0, 0, 2);
    a.songMode = 1;
    n = projectio::save(a, buf, sizeof(buf));
    size_t plst = 0;
    for (size_t i = 12; i + 4 < n; ++i)
        if (memcmp(buf + i, "PLST", 4) == 0) {
            plst = i;
            break;
        }
    CHECK(plst > 0);
    uint32_t plen = buf[plst + 4] | (buf[plst + 5] << 8) | (buf[plst + 6] << 16) | ((uint32_t)buf[plst + 7] << 24);
    const size_t endChunk = plst + 8 + plen; // the "END " chunk starts here
    CHECK(memcmp(buf + endChunk, "END ", 4) == 0);
    // Drop the last payload byte, then fix the chunk length and the checksum.
    memmove(buf + endChunk - 1, buf + endChunk, n - endChunk);
    n -= 1;
    plen -= 1;
    buf[plst + 4] = (uint8_t)plen;
    buf[plst + 5] = (uint8_t)(plen >> 8);
    buf[plst + 6] = (uint8_t)(plen >> 16);
    buf[plst + 7] = (uint8_t)(plen >> 24);
    const size_t newEnd = endChunk - 1;
    const uint32_t crc = projectio::crc32(buf, newEnd);
    buf[newEnd + 8] = (uint8_t)crc;
    buf[newEnd + 9] = (uint8_t)(crc >> 8);
    buf[newEnd + 10] = (uint8_t)(crc >> 16);
    buf[newEnd + 11] = (uint8_t)(crc >> 24);
    CHECK(projectio::load(buf, n, b, err, sizeof(err)));
    CHECK(b.clipCount == 1 && b.songMode == 0);

    // The playlist reaches the engine on project load.
    H h;
    Project q;
    q.resetEmpty();
    q.patterns[0].velocity[0][0] = 100;
    q.songMode = 1;
    q.clips[0].track = 0;
    q.clips[0].pattern = 0;
    q.clips[0].startBar = 1;
    q.clips[0].lengthBars = 1;
    q.clipCount = 1;
    h.session.loadProject(q);
    h.session.play();
    h.steps(16);
    CHECK(h.engine.status().songMode == 1 && h.engine.status().triggerCount[0] == 0); // bar 1 is empty
    h.steps(16);
    CHECK(h.engine.status().triggerCount[0] == 1);
}

int runPlaylistTests()
{
    printf("running playlist tests\n");
    testSongPlayback();
    testClipEditing();
    testPlaylistProject();
    return g_fail;
}
