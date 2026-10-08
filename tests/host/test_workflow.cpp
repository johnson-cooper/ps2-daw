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
    CHECK(!slotstore::save(fs, dir, -1, p1, a, b, sizeof(a), err, sizeof(err)));
    CHECK(!slotstore::save(fs, dir, 9, p1, a, b, sizeof(a), err, sizeof(err)));
    // Slot 0 is the autosave: same safe rotation, its own file name, independent of the numbered slots.
    CHECK(slotstore::save(fs, dir, slotstore::kAutosaveSlot, p1, a, b, sizeof(a), err, sizeof(err)));
    CHECK(fs.exists("mass0:/PS2DAW/AUTOSAVE.ps2daw"));
    CHECK(slotstore::peek(fs, dir, slotstore::kAutosaveSlot, a, sizeof(a)).exists);
    CHECK(slotstore::load(fs, dir, slotstore::kAutosaveSlot, out, a, sizeof(a), &bak, err, sizeof(err)) && !bak);
    CHECK(slotstore::save(fs, dir, slotstore::kAutosaveSlot, p1, a, b, sizeof(a), err, sizeof(err)) && fs.exists("mass0:/PS2DAW/AUTOSAVE.BAK"));

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
    add(cfg::kPlaylistTracks + 3, 0, 0, 1);       // track out of range
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
    CHECK(b.clipAt(cfg::kPlaylistTracks + 3, 0) < 0 && b.clipAt(0, 500) < 0);
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
    // Rewrites the file with the last `drop` payload bytes of PLST removed (older layouts),
    // fixing the chunk length and the checksum.
    auto older = [&](size_t drop, Project& out) {
        static uint8_t copy[projectio::kMaxFileBytes];
        memcpy(copy, buf, n);
        size_t len = n;
        memmove(copy + endChunk - drop, copy + endChunk, len - endChunk);
        len -= drop;
        const uint32_t pl = plen - (uint32_t)drop;
        copy[plst + 4] = (uint8_t)pl;
        copy[plst + 5] = (uint8_t)(pl >> 8);
        copy[plst + 6] = (uint8_t)(pl >> 16);
        copy[plst + 7] = (uint8_t)(pl >> 24);
        const size_t newEnd = endChunk - drop;
        const uint32_t crc = projectio::crc32(copy, newEnd);
        copy[newEnd + 8] = (uint8_t)crc;
        copy[newEnd + 9] = (uint8_t)(crc >> 8);
        copy[newEnd + 10] = (uint8_t)(crc >> 16);
        copy[newEnd + 11] = (uint8_t)(crc >> 24);
        return projectio::load(copy, len, out, err, sizeof(err));
    };
    a.trackMute = 0x05;
    a.trackSolo = 0x02;
    n = projectio::save(a, buf, sizeof(buf));
    CHECK(projectio::load(buf, n, b, err, sizeof(err)) && b.trackMute == 0x05 && b.trackSolo == 0x02 && b.songMode == 1);
    // Layout 1: before song mode existed (no trailing bytes at all).
    CHECK(older(7, b) && b.clipCount == 1 && b.songMode == 0 && b.trackMute == 0 && b.trackSolo == 0);
    // Layout 2: song mode but no track masks.
    CHECK(older(6, b) && b.clipCount == 1 && b.songMode == 1 && b.trackMute == 0 && b.trackSolo == 0);

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

// ---- piano roll, pitch, gate, playlist extras ---------------------------------------
#include "audio/pitch.hpp"

// A rig with one DC sample (always 20000) of `frames` frames on channel 0, 120 BPM.
struct DcRig {
    H h;
    int slot;
    std::vector<int16_t> left;
    explicit DcRig(uint32_t frames)
    {
        int16_t* dc = (int16_t*)malloc(frames * sizeof(int16_t));
        for (uint32_t i = 0; i < frames; ++i)
            dc[i] = 20000;
        slot = h.bank.add("DC", dc, frames, cfg::kSampleRate, 1, true, "builtin:DC");
        h.session.setSample(0, slot);
        h.session.setVolume(0, 100);
        h.session.setMasterVolume(100);
    }
    // Renders `frames` frames and records the left channel.
    void run(uint32_t frames)
    {
        int16_t buf[cfg::kMaxBlockFrames * 2];
        uint32_t done = 0;
        while (done < frames) {
            const int n = (int)(frames - done < 256 ? frames - done : 256);
            h.engine.render(buf, n);
            for (int i = 0; i < n; ++i)
                left.push_back(buf[i * 2]);
            done += (uint32_t)n;
        }
    }
    // Number of leading non-silent frames.
    size_t audibleRun() const
    {
        size_t n = 0;
        while (n < left.size() && left[n] != 0)
            ++n;
        return n;
    }
};

static void testPitchAndGate()
{
    CHECK(pitch::ratioQ16(0) == 65536 && pitch::ratioQ16(12) == 131072 && pitch::ratioQ16(-12) == 32768);
    CHECK(pitch::ratioQ16(1000) == pitch::ratioQ16(pitch::kMaxSemis) && pitch::ratioQ16(-1000) == pitch::ratioQ16(-pitch::kMaxSemis));
    CHECK(pitch::ratioQ16(7) > 98000 && pitch::ratioQ16(7) < 98400); // a fifth: 1.4983

    // Transposition changes playback speed, so the sample's length scales by 2^(-semis/12).
    const struct { int pitch; int expectFrames; } cases[] = {{60, 4800}, {72, 2400}, {48, 9600}, {67, 3204}, {84, 1200}};
    for (const auto& k : cases) {
        DcRig r(4800);
        r.h.session.addNote(0, 0, 0, k.pitch, 1, 127);
        r.h.session.play();
        r.run(12000);
        const size_t run = r.audibleRun();
        CHECK(run + 4 >= (size_t)k.expectFrames && run <= (size_t)k.expectFrames + 4);
    }

    // One-shot channel: the note length does not cut the sample.
    {
        DcRig r(48000);
        r.h.session.addNote(0, 0, 0, 60, 1, 127); // 1 step = 6000 frames
        r.h.session.play();
        r.run(20000);
        CHECK(r.audibleRun() >= 19000);
    }
    // Sustained channel: the length cuts the sample (plus a short declick fade).
    {
        DcRig r(48000);
        r.h.session.setChannelGate(0, true);
        r.h.session.addNote(0, 0, 0, 60, 1, 127);
        r.h.session.play();
        r.run(20000);
        const size_t run = r.audibleRun();
        CHECK(run >= 5990 && run <= 6000 + 300);
        // Longer notes last proportionally longer; tempo scales the length.
        DcRig r2(96000);
        r2.h.session.setChannelGate(0, true);
        r2.h.session.setBpmCenti(24000);
        r2.h.session.addNote(0, 0, 0, 60, 4, 127);
        r2.h.session.play();
        r2.run(20000);
        const size_t run2 = r2.audibleRun();
        CHECK(run2 >= 11990 && run2 <= 12000 + 300); // 4 steps at 240 BPM = 4 x 3000
        // The grid hit on a sustained channel still plays the whole sample.
        DcRig r3(8000);
        r3.h.session.setChannelGate(0, true);
        r3.h.session.setStep(0, 0, 0, 100);
        r3.h.session.play();
        r3.run(12000);
        CHECK(r3.audibleRun() >= 7990);
    }
}

static void testChordsAndChoke()
{
    // A chord on a one-shot channel: all notes sound together (no self-choke)...
    {
        DcRig r(30000);
        r.h.session.addNote(0, 0, 0, 60, 1, 100);
        r.h.session.addNote(0, 0, 0, 64, 1, 100);
        r.h.session.addNote(0, 0, 0, 67, 1, 100);
        r.h.session.play();
        r.run(512);
        CHECK(r.h.engine.status().voicesActive == 3);
        CHECK(r.h.engine.status().triggerCount[0] == 3);
        // ...and the next step retriggers like a drum pad: the old voices fade out.
        r.h.session.addNote(0, 0, 1, 60, 1, 100);
        r.run(48000 / 8 * 2 + 4000);
        CHECK(r.h.engine.status().voicesActive <= 2);
    }
    // A sustained channel layers notes across steps instead.
    {
        DcRig r(60000);
        r.h.session.setChannelGate(0, true);
        r.h.session.addNote(0, 0, 0, 60, 8, 100);
        r.h.session.addNote(0, 0, 1, 64, 8, 100);
        r.h.session.addNote(0, 0, 2, 67, 8, 100);
        r.h.session.play();
        r.run(6000 * 2 + 600);
        CHECK(r.h.engine.status().voicesActive == 3);
    }
    // Grid hit and a note on the same step both play.
    {
        DcRig r(30000);
        r.h.session.setStep(0, 0, 0, 100);
        r.h.session.addNote(0, 0, 0, 72, 1, 100);
        r.h.session.play();
        r.run(512);
        CHECK(r.h.engine.status().triggerCount[0] == 2);
    }
    // Muted channels stay silent for notes too.
    {
        DcRig r(30000);
        r.h.session.addNote(0, 0, 0, 60, 1, 100);
        r.h.session.setMute(0, true);
        r.h.session.play();
        r.run(2000);
        CHECK(r.h.engine.status().triggerCount[0] == 0);
    }
    // Preview with pitch.
    {
        DcRig r(4800);
        r.h.session.previewChannel(0, 12);
        r.run(8000);
        const size_t run = r.audibleRun();
        CHECK(run + 4 >= 2400 && run <= 2404);
    }
}

static void testNoteEditing()
{
    H h;
    Session& s = h.session;
    const Project& p = s.project();
    CHECK(!s.addNote(-1, 0, 0, 60, 1, 100) && !s.addNote(0, cfg::kMaxChannels, 0, 60, 1, 100) && !s.addNote(0, 0, 64, 60, 1, 100) &&
          !s.addNote(0, 0, 0, 128, 1, 100) && !s.addNote(0, 0, 0, -1, 1, 100));
    CHECK(p.patterns[0].noteCount[0] == 0);

    CHECK(s.addNote(0, 0, 4, 60, 2, 100));
    CHECK(s.addNote(0, 0, 4, 64, 2, 90)); // chord: a different pitch at the same step
    CHECK(p.patterns[0].noteCount[0] == 2);
    CHECK(s.addNote(0, 0, 4, 60, 5, 50)); // same step+pitch: replaced, not duplicated
    CHECK(p.patterns[0].noteCount[0] == 2 && p.patterns[0].notes[0][s.noteIndexAt(0, 0, 4, 60)].length == 5);
    CHECK(s.addNote(0, 0, 0, 60, 500, 500)); // clamped
    CHECK(p.patterns[0].notes[0][s.noteIndexAt(0, 0, 0, 60)].length == cfg::kMaxSteps);
    CHECK(p.patterns[0].notes[0][s.noteIndexAt(0, 0, 0, 60)].velocity == 127);
    CHECK(s.addNote(0, 0, 2, 60, 0, 0) && p.patterns[0].notes[0][s.noteIndexAt(0, 0, 2, 60)].length == 1 &&
          p.patterns[0].notes[0][s.noteIndexAt(0, 0, 2, 60)].velocity == 1);

    CHECK(s.setNoteLength(0, 0, s.noteIndexAt(0, 0, 4, 64), 3) == 3 && s.setNoteLength(0, 0, 99, 3) == 0);
    CHECK(!s.removeNote(0, 0, -1) && !s.removeNote(0, 0, 50));
    const int before = p.patterns[0].noteCount[0];
    CHECK(s.removeNote(0, 0, s.noteIndexAt(0, 0, 4, 64)) && p.patterns[0].noteCount[0] == before - 1);
    CHECK(s.noteIndexAt(0, 0, 4, 64) < 0 && s.noteIndexAt(0, 0, 4, 60) >= 0);

    // The channel holds at most kMaxNotes; other channels are independent.
    s.clearNotes(0, 0);
    for (int i = 0; i < cfg::kMaxNotes; ++i)
        CHECK(s.addNote(0, 0, i, 40 + i, 1, 100));
    CHECK(!s.addNote(0, 0, 40, 100, 1, 100) && p.patterns[0].noteCount[0] == cfg::kMaxNotes);
    CHECK(s.addNote(0, 0, 3, 40 + 3, 4, 100)); // replacing at the limit is fine
    CHECK(s.addNote(0, 1, 0, 60, 1, 100));

    // Transpose is all-or-nothing.
    CHECK(s.transposeNotes(0, 0, 12) && p.patterns[0].notes[0][0].pitch == 52);
    CHECK(!s.transposeNotes(0, 0, 100) && p.patterns[0].notes[0][0].pitch == 52);
    CHECK(s.transposeNotes(0, 0, -12) && p.patterns[0].notes[0][0].pitch == 40);
    CHECK(!s.transposeNotes(0, 0, -41)); // would go below 0

    // Notes count as content for copy/duplicate/empty checks.
    s.clearNotes(0, 0);
    CHECK(!s.patternIsEmpty(0)); // channel 1 still has one
    s.clearPattern(0);
    CHECK(s.patternIsEmpty(0));
    s.addNote(2, 5, 7, 70, 3, 80);
    CHECK(!s.patternIsEmpty(2));
    CHECK(s.copyPattern(2, 4) && p.patterns[4].noteCount[5] == 1 && p.patterns[4].notes[5][0].pitch == 70);
    const int d = s.duplicatePattern(2);
    CHECK(d >= 0 && p.patterns[d].noteCount[5] == 1);

    // Rack interaction: a cell showing only notes is removed by toggling it; the channel clear removes notes too.
    s.selectPattern(6);
    s.addNote(6, 2, 5, 60, 2, 100);
    s.toggleStep(2, 5);
    CHECK(p.patterns[6].noteCount[2] == 0 && p.patterns[6].velocity[2][5] == 0);
    s.addNote(6, 2, 5, 60, 2, 100);
    s.setStep(6, 2, 9, 100);
    s.clearChannelSteps(2);
    CHECK(p.patterns[6].noteCount[2] == 0 && p.patterns[6].velocity[2][9] == 0);
}

static void testNotesPersistence()
{
    static uint8_t buf[projectio::kMaxFileBytes];
    H h;
    Session& s = h.session;
    s.addNote(0, 3, 0, 60, 2, 100);
    s.addNote(0, 3, 0, 67, 2, 80);
    s.addNote(5, 7, 15, 36, 16, 127);
    s.setChannelGate(3, true);
    s.setTrackMute(1, true);
    const size_t n = projectio::save(s.project(), buf, sizeof(buf));
    Project q;
    char err[64];
    CHECK(n && projectio::load(buf, n, q, err, sizeof(err)));
    CHECK(q.patterns[0].noteCount[3] == 2 && q.patterns[0].notes[3][1].pitch == 67 && q.patterns[0].notes[3][1].velocity == 80);
    CHECK(q.patterns[5].noteCount[7] == 1 && q.patterns[5].notes[7][0].length == 16 && q.channels[3].gate == 1 && q.channels[2].gate == 0);
    CHECK(q.trackMute == 2);

    // Loading reaches the engine: a fresh session plays the loaded notes.
    DcRig r(4800);
    Project loaded = q;
    for (auto& c : loaded.channels)
        c.sampleSlot = (int8_t)r.slot;
    loaded.patterns[0].noteCount[3] = 0; // keep only what we assert on
    loaded.patterns[0].noteCount[0] = 1;
    loaded.patterns[0].notes[0][0] = {0, 72, 127, 1};
    loaded.channels[0].volume = 100;
    loaded.masterVolume = 100;
    r.h.session.loadProject(loaded);
    r.h.session.play();
    r.run(6000);
    CHECK(r.audibleRun() + 4 >= 2400 && r.audibleRun() <= 2404);

    // Damaged / hostile note data is filtered, never trusted.
    Project bad;
    bad.resetEmpty();
    PatternData& pd = bad.patterns[1];
    pd.noteCount[0] = 8;
    pd.notes[0][0] = {99, 60, 100, 1};   // step beyond the pattern grid
    pd.notes[0][1] = {0, 200, 100, 1};   // pitch beyond MIDI
    pd.notes[0][2] = {0, 60, 0, 1};      // silent
    pd.notes[0][3] = {0, 60, 100, 0};    // zero length: becomes 1
    pd.notes[0][4] = {1, 61, 200, 250};  // velocity and length clamped
    pd.notes[0][5] = {2, 62, 100, 4};    // fine
    pd.notes[0][6] = {3, 63, 100, 4};    // fine
    pd.notes[0][7] = {64, 60, 100, 4};   // step == kMaxSteps: rejected
    pd.noteCount[1] = 200;               // count beyond the table: never reads past it
    const size_t m = projectio::save(bad, buf, sizeof(buf));
    CHECK(m && projectio::load(buf, m, q, err, sizeof(err)));
    CHECK(q.patterns[1].noteCount[0] == 4);
    CHECK(q.patterns[1].notes[0][0].length == 1 && q.patterns[1].notes[0][0].pitch == 60);
    CHECK(q.patterns[1].notes[0][1].velocity == 127 && q.patterns[1].notes[0][1].length == cfg::kMaxSteps);
    CHECK(q.patterns[1].noteCount[1] <= cfg::kMaxNotes);
    // A truncated NOTE chunk is rejected as corrupt rather than half-applied.
    size_t note = 0;
    for (size_t i = 12; i + 4 < m; ++i)
        if (memcmp(buf + i, "NOTE", 4) == 0) {
            note = i;
            break;
        }
    CHECK(note > 0);
    buf[note + 8 + 2] = 40; // claims 40 notes in a chunk that holds fewer
    const uint32_t crc = projectio::crc32(buf, m - 12);
    buf[m - 4] = (uint8_t)crc;
    buf[m - 3] = (uint8_t)(crc >> 8);
    buf[m - 2] = (uint8_t)(crc >> 16);
    buf[m - 1] = (uint8_t)(crc >> 24);
    CHECK(!projectio::load(buf, m, q, err, sizeof(err)));
}

static void testPlaylistExtras()
{
    // Play from a bar: the first step fired is that bar's first step.
    {
        H h;
        h.session.setStep(0, 0, 0, 100);
        h.session.placeClip(0, 0, 0, 4);
        h.session.setSongMode(true);
        h.session.playFromBar(2);
        h.steps(3);
        CHECK(h.marks[0].songStep == 32 && h.marks[1].songStep == 33);
        CHECK(h.engine.status().triggerCount[0] == 1); // bar 3 step 0 sounded
        h.session.playFromBar(0); // restart while playing
        h.steps(2);
        CHECK(h.marks[3].songStep == 0);
        h.session.playFromBar(99); // past the end: starts from the top
        h.steps(2);
        CHECK(h.marks[5].songStep == 0);
        // Without song mode it is a plain play.
        H g;
        g.session.setSongMode(false);
        g.session.playFromBar(3);
        g.steps(2);
        CHECK(g.marks[0].songStep == 0);
    }
    // Track mute and solo.
    {
        H h;
        h.session.setStep(0, 0, 0, 100); // pattern 1: channel 0
        h.session.setStep(1, 1, 0, 100); // pattern 2: channel 1
        h.session.placeClip(0, 0, 0, 1);
        h.session.placeClip(1, 0, 1, 1);
        h.session.setSongMode(true);
        h.session.setTrackMute(1, true);
        h.session.play();
        h.steps(16);
        CHECK(h.engine.status().triggerCount[0] == 1 && h.engine.status().triggerCount[1] == 0);
        h.session.setTrackMute(1, false);
        h.session.setTrackSolo(1, true); // solo wins over the other track
        h.steps(16);
        CHECK(h.engine.status().triggerCount[0] == 1 && h.engine.status().triggerCount[1] == 1);
        h.session.setTrackSolo(1, false);
        h.steps(16);
        CHECK(h.engine.status().triggerCount[0] == 2 && h.engine.status().triggerCount[1] == 2);
        h.session.setTrackMute(99, true); // ignored
        CHECK(h.session.project().trackMute == 0);
    }
    // Notes inside song clips play with pitch, per clip pattern.
    {
        DcRig r(4800);
        r.h.session.addNote(0, 0, 0, 72, 1, 127);
        r.h.session.placeClip(0, 0, 0, 1);
        r.h.session.setSongMode(true);
        r.h.session.play();
        r.run(6000);
        CHECK(r.audibleRun() + 4 >= 2400 && r.audibleRun() <= 2404);
    }
}

int runPianoRollTests()
{
    printf("running piano roll tests\n");
    testPitchAndGate();
    testChordsAndChoke();
    testNoteEditing();
    testNotesPersistence();
    testPlaylistExtras();
    return g_fail;
}
