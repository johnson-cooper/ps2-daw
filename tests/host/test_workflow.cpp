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
