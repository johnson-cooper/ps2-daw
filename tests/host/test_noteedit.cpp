// Host tests for the piano-roll editing operations (tick resolution), the
// waveform summaries and sub-step note playback.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "audio/audio_engine.hpp"
#include "project/note_edit.hpp"
#include "project/project_io.hpp"
#include "project/session.hpp"
#include "ui/waveform.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

using namespace noteedit;

static constexpr int T = kTicks; // ticks per step

// A note on a whole step with a whole-step length.
static void add(Set& s, int step, int pitch, int len = 1, int vel = 100)
{
    PianoNote n = {};
    n.pitch = (uint8_t)pitch;
    n.velocity = (uint8_t)vel;
    setStart(n, step * T);
    setDuration(n, len * T);
    s.notes[s.count++] = n;
}

// A note at an exact tick position with an exact duration.
static void addT(Set& s, int startTicks, int pitch, int durTicks, int vel = 100)
{
    PianoNote n = {};
    n.pitch = (uint8_t)pitch;
    n.velocity = (uint8_t)vel;
    setStart(n, startTicks);
    setDuration(n, durTicks);
    s.notes[s.count++] = n;
}

static void testNoteTiming()
{
    PianoNote n = {};
    setStart(n, 3 * T + 7);
    setDuration(n, 2 * T);
    CHECK(n.step == 3 && n.tick == 7 && n.length == 2 && n.lenTicks == 0 && n.startTicks() == 79 && n.durTicks() == 48);
    setDuration(n, 6); // a quarter of a step
    CHECK(n.lenTicks == 6 && n.length == 1 && n.durTicks() == 6);
    setDuration(n, 30);
    CHECK(n.lenTicks == 30 && n.length == 2);
    setDuration(n, 0);
    CHECK(n.durTicks() == 1);
    setDuration(n, 100000);
    CHECK(n.durTicks() == cfg::kMaxSteps * T && n.lenTicks == 0 && n.length == cfg::kMaxSteps);
    // a note is found by any tick it covers
    Set s;
    addT(s, 30, 60, 6);
    CHECK(indexCovering(s, 30, 60) == 0 && indexCovering(s, 35, 60) == 0 && indexCovering(s, 36, 60) < 0 && indexCovering(s, 29, 60) < 0);
    CHECK(indexAt(s, 30, 60) == 0 && indexAt(s, 31, 60) < 0);
}

static void testSelectionAndMove()
{
    Set s;
    add(s, 0, 60, 2);
    add(s, 4, 64);
    add(s, 8, 67, 4);
    CHECK(selectedCount(s) == 0);
    toggleSelect(s, 1);
    toggleSelect(s, 2);
    CHECK(selectedCount(s) == 2 && isSelected(s, 1) && !isSelected(s, 0));
    CHECK(move(s, 2 * T, 7, 16 * T));
    CHECK(s.notes[0].step == 0 && s.notes[1].step == 6 && s.notes[1].pitch == 71 && s.notes[2].step == 10 && s.notes[2].pitch == 74);
    CHECK(!move(s, 20 * T, 0, 16 * T));
    CHECK(!move(s, 0, 60, 16 * T));
    CHECK(!move(s, -7 * T, 0, 16 * T));
    CHECK(s.notes[1].step == 6 && s.notes[1].pitch == 71);
    // moving by a few ticks
    CHECK(move(s, 5, 0, 16 * T) && s.notes[1].step == 6 && s.notes[1].tick == 5);
    CHECK(move(s, -10, 0, 16 * T) && s.notes[1].step == 5 && s.notes[1].tick == 19);
    // moving onto another note replaces it
    Set t;
    add(t, 0, 60);
    add(t, 4, 60, 3, 90);
    toggleSelect(t, 1);
    CHECK(move(t, -4 * T, 0, 16 * T));
    CHECK(t.count == 1 && t.notes[0].step == 0 && t.notes[0].length == 3 && isSelected(t, 0));
    // select helpers
    Set u;
    add(u, 0, 60);
    add(u, 0, 64);
    add(u, 4, 60);
    selectColumn(u, 0);
    CHECK(selectedCount(u) == 2);
    selectRow(u, 60);
    CHECK(selectedCount(u) == 2 && isSelected(u, 0) && isSelected(u, 2));
    selectFrom(u, 4 * T);
    CHECK(selectedCount(u) == 1 && isSelected(u, 2));
    selectAll(u);
    CHECK(selectedCount(u) == 3);
    selectNone(u);
    CHECK(selectedCount(u) == 0);
}

static void testTransposeQuantizeVelocity()
{
    Set s;
    add(s, 1, 60, 3, 50);
    add(s, 6, 72, 1, 100);
    add(s, 10, 48, 5, 120);
    CHECK(transpose(s, 12, true) && s.notes[0].pitch == 72 && s.notes[2].pitch == 60);
    CHECK(!transpose(s, 60, true) && s.notes[0].pitch == 72);
    CHECK(transpose(s, -12, true));
    toggleSelect(s, 1);
    CHECK(transpose(s, 2) && s.notes[1].pitch == 74 && s.notes[0].pitch == 60);
    // quantize to every 4 steps
    quantize(s, 4 * T, 16 * T, true);
    CHECK(s.notes[0].step == 0 && s.notes[1].step == 8 && s.notes[2].step == 12);
    CHECK(s.notes[0].length == 4 && s.notes[1].length == 4 && s.notes[2].length == 4);
    Set e;
    add(e, 15, 60);
    quantize(e, 8 * T, 16 * T, true);
    CHECK(e.notes[0].step == 8);
    Set m;
    add(m, 0, 60);
    add(m, 1, 60);
    quantize(m, 4 * T, 16 * T, true);
    CHECK(m.count == 1 && m.notes[0].step == 0);
    // sub-step quantize: a loose note snaps to the nearest 1/4 step (6 ticks) and keeps at least one grid length
    Set q;
    addT(q, 2 * T + 8, 62, 5);
    quantize(q, 6, 16 * T, true);
    CHECK(q.notes[0].startTicks() == 2 * T + 6 && q.notes[0].durTicks() == 6);
    Set v;
    add(v, 0, 60, 1, 60);
    add(v, 2, 62, 1, 126);
    addVelocity(v, 20, true);
    CHECK(v.notes[0].velocity == 80 && v.notes[1].velocity == 127);
    setVelocity(v, 1, true);
    addVelocity(v, -50, true);
    CHECK(v.notes[0].velocity == 1);
    setLength(v, 8, true);
    addLength(v, 100 * T, true);
    CHECK(v.notes[0].length == cfg::kMaxSteps);
    addLength(v, -200 * T, true);
    CHECK(v.notes[0].durTicks() == 1);
    setLengthTicks(v, 9, true);
    CHECK(v.notes[0].lenTicks == 9 && v.notes[0].length == 1);
}

static void testChop()
{
    // one whole-step note into 4 quarter-steps
    Set s;
    add(s, 2, 60, 1, 90);
    CHECK(chop(s, 4, true));
    CHECK(s.count == 4 && selectedCount(s) == 4);
    for (int i = 0; i < 4; ++i)
        CHECK(s.notes[i].startTicks() == 2 * T + i * 6 && s.notes[i].durTicks() == 6 && s.notes[i].velocity == 90 && s.notes[i].pitch == 60);
    // three parts of 8 ticks, and a remainder goes to the last piece
    Set r;
    addT(r, 0, 60, 26);
    CHECK(chop(r, 3, true));
    CHECK(r.count == 3 && r.notes[0].durTicks() == 8 && r.notes[1].durTicks() == 8 && r.notes[2].durTicks() == 10);
    CHECK(r.notes[2].startTicks() == 16);
    // a two-step note into two whole steps keeps whole-step lengths
    Set w;
    add(w, 4, 64, 2);
    CHECK(chop(w, 2, true) && w.notes[0].lenTicks == 0 && w.notes[0].length == 1 && w.notes[1].step == 5);
    // only the selection is chopped; others untouched; selection follows the pieces
    Set p;
    add(p, 0, 60);
    add(p, 4, 62);
    toggleSelect(p, 1);
    CHECK(chop(p, 2, false) && p.count == 3 && p.notes[0].durTicks() == T && selectedCount(p) == 2 && !isSelected(p, 0));
    // too short to chop: refused, nothing changes
    Set sh;
    addT(sh, 0, 60, 5);
    CHECK(!chop(sh, 8, true) && sh.count == 1 && sh.notes[0].durTicks() == 5);
    CHECK(!chop(sh, 1, true));
    // does not fit: refused
    Set full;
    for (int i = 0; i < cfg::kMaxNotes - 1; ++i)
        add(full, i % 16, 20 + i / 16 * 3 + (i % 3));
    add(full, 0, 100);
    CHECK(!chop(full, 4, true) && full.count == cfg::kMaxNotes);
    // chop then quantize back to the grid merges nothing it should not
    Set rt;
    add(rt, 1, 60);
    chop(rt, 2, true);
    quantize(rt, T, 16 * T, true);
    CHECK(rt.count == 1 || rt.count == 2);
}

static void testCopyPasteDuplicateDelete()
{
    Set s;
    add(s, 4, 62, 2);
    add(s, 6, 66, 1);
    add(s, 12, 70, 4);
    toggleSelect(s, 0);
    toggleSelect(s, 1);
    Clip c;
    CHECK(copy(s, c) == 2 && c.spanTicks == 3 * T && c.notes[0].startTicks() == 0 && c.notes[0].pitch == 0 && c.notes[1].startTicks() == 2 * T &&
          c.notes[1].pitch == 4);
    CHECK(paste(s, c, 8 * T, 50, 16 * T));
    CHECK(s.count == 5 && selectedCount(s) == 2);
    CHECK(indexAt(s, 8 * T, 50) >= 0 && indexAt(s, 10 * T, 54) >= 0 && isSelected(s, indexAt(s, 8 * T, 50)) && !isSelected(s, 0));
    Set t = s;
    CHECK(paste(t, c, 15 * T, 50, 16 * T) && indexAt(t, 15 * T, 50) >= 0 && indexAt(t, 17 * T, 54) < 0);
    Set full;
    for (int i = 0; i < cfg::kMaxNotes; ++i)
        add(full, i % 16, 20 + i / 16 * 4 + (i % 3));
    const int before = full.count;
    Clip big;
    big.count = 1;
    big.notes[0] = {};
    big.notes[0].velocity = 100;
    big.notes[0].length = 1;
    CHECK(!paste(full, big, 0, 100, 16 * T) && full.count == before);
    Set r;
    add(r, 0, 60, 1, 50);
    Clip one;
    one.count = 1;
    one.notes[0] = {};
    one.notes[0].velocity = 120;
    one.notes[0].length = 2;
    CHECK(paste(r, one, 0, 60, 16 * T) && r.count == 1 && r.notes[0].velocity == 120 && r.notes[0].length == 2);
    Set d;
    add(d, 0, 60, 2);
    add(d, 2, 64, 2);
    selectAll(d);
    CHECK(duplicate(d, 16 * T) && d.count == 4 && indexAt(d, 4 * T, 60) >= 0 && indexAt(d, 6 * T, 64) >= 0);
    CHECK(selectedCount(d) == 2 && isSelected(d, indexAt(d, 4 * T, 60)));
    // a sub-step phrase keeps its inner timing when copied
    Set ph;
    addT(ph, 2 * T + 6, 60, 6);
    addT(ph, 2 * T + 12, 60, 6);
    CHECK(copy(ph, c) == 2 && c.notes[1].startTicks() - c.notes[0].startTicks() == 6);
    CHECK(paste(ph, c, 8 * T + 3, 60, 16 * T) && indexAt(ph, 8 * T + 3, 60) >= 0 && indexAt(ph, 8 * T + 9, 60) >= 0);
    Set x;
    add(x, 0, 60);
    add(x, 1, 61);
    add(x, 2, 62);
    toggleSelect(x, 0);
    toggleSelect(x, 2);
    CHECK(removeSelected(x) == 2 && x.count == 1 && x.notes[0].pitch == 61 && x.sel == 0);
    Set all;
    add(all, 3, 70);
    add(all, 5, 72);
    CHECK(copy(all, c) == 2);
}

static void testSessionNotesAndPersistence()
{
    SampleBank bank;
    AudioEngine engine;
    Session session(engine, bank, [] { return false; });
    engine.setSampleBank(&bank);
    Set s;
    add(s, 0, 60, 2);
    add(s, 3, 64, 4, 90);
    addT(s, 5 * T + 9, 67, 6, 80); // sub-step note
    PianoNote bad = {};
    bad.step = 99;
    bad.length = 1;
    bad.velocity = 100;
    s.notes[s.count++] = bad; // invalid step: dropped
    PianoNote bad2 = {};
    bad2.step = 2;
    bad2.pitch = 200;
    bad2.velocity = 100;
    bad2.length = 1;
    s.notes[s.count++] = bad2; // invalid pitch: dropped
    CHECK(session.setNotes(0, 1, s.notes, s.count));
    CHECK(session.project().patterns[0].noteCount[1] == 3);
    CHECK(!session.setNotes(0, 1, s.notes, cfg::kMaxNotes + 1));
    Set big;
    for (int i = 0; i < cfg::kMaxNotes; ++i)
        addT(big, (i % cfg::kMaxSteps) * T + (i % 5) * 4, 30 + i % 40, (i % 3 == 0) ? 6 : (1 + i % 3) * T, 20 + i);
    CHECK(session.setNotes(1, 2, big.notes, big.count) && session.project().patterns[1].noteCount[2] == cfg::kMaxNotes);
    static uint8_t buf[projectio::kMaxFileBytes];
    const size_t n = projectio::save(session.project(), buf, sizeof(buf));
    Project p;
    char err[64];
    CHECK(n > 0 && projectio::load(buf, n, p, err, sizeof(err)));
    CHECK(p.patterns[1].noteCount[2] == cfg::kMaxNotes && memcmp(p.patterns[1].notes[2], big.notes, sizeof(PianoNote) * cfg::kMaxNotes) == 0);
    CHECK(p.patterns[0].noteCount[1] == 3 && p.patterns[0].notes[1][1].velocity == 90);
    const PianoNote& sub = p.patterns[0].notes[1][2];
    CHECK(sub.step == 5 && sub.tick == 9 && sub.lenTicks == 6 && sub.length == 1);
    // A file without the timing chunk (older build) loads the notes on whole steps.
    size_t at = 0;
    for (size_t i = 12; i + 4 < n; ++i)
        if (memcmp(buf + i, "NOTX", 4) == 0) {
            at = i;
            break;
        }
    CHECK(at > 0);
    memcpy(buf + at, "ZZZZ", 4); // renamed: an unknown chunk, skipped like any other (the checksum covers it, so redo it)
    const uint32_t crc = projectio::crc32(buf, n - 12);
    buf[n - 4] = (uint8_t)crc;
    buf[n - 3] = (uint8_t)(crc >> 8);
    buf[n - 2] = (uint8_t)(crc >> 16);
    buf[n - 1] = (uint8_t)(crc >> 24);
    Project old;
    CHECK(projectio::load(buf, n, old, err, sizeof(err)));
    CHECK(old.patterns[0].noteCount[1] == 3 && old.patterns[0].notes[1][2].tick == 0 && old.patterns[0].notes[1][2].lenTicks == 0 &&
          old.patterns[0].notes[1][2].length == 1);
}

// Sub-step notes reach the mixer at the right frame, with the right length.
static void testSubStepPlayback()
{
    SampleBank bank;
    AudioEngine engine;
    Session session(engine, bank, [] { return false; });
    engine.setSampleBank(&bank);
    int16_t* dc = (int16_t*)malloc(48000 * sizeof(int16_t));
    for (int i = 0; i < 48000; ++i)
        dc[i] = 20000;
    const int slot = bank.add("DC", dc, 48000, 48000, 1, true, "builtin:DC");
    session.setSample(0, slot);
    session.setVolume(0, 100);
    session.setMasterVolume(100);
    session.setBpmCenti(12000); // a step is 6000 frames, a tick 250
    session.setChannelGate(0, true);
    // quarter-step notes: starts at ticks 0, 6, 12, 18 of step 1, each 6 ticks (1500 frames) long
    Set s;
    for (int i = 0; i < 4; ++i)
        addT(s, T + i * 6, 60, 6);
    session.setNotes(0, 0, s.notes, s.count);
    session.play();
    std::vector<int16_t> L;
    int16_t buf[cfg::kMaxBlockFrames * 2];
    for (int done = 0; done < 24000; done += 250) {
        engine.render(buf, 250);
        for (int i = 0; i < 250; ++i)
            L.push_back(buf[i * 2]);
    }
    // the first note starts at step 1 (frame 6000); consecutive pieces of 1500 frames join into one
    // sound lasting about 4 x 1500 frames, with a declick tail of 256 frames.
    CHECK(L[5999] == 0 && L[6000] != 0);
    size_t end = 6000;
    while (end < L.size() && L[end] != 0)
        ++end;
    CHECK(end - 6000 >= 6000 && end - 6000 <= 6000 + 300);
    // a single sub-step note: start offset and length are sample-accurate
    Set one;
    addT(one, 2 * T + 12, 60, 3); // frame 12000 + 12*250 = 15000, 750 frames long
    session.stop();
    session.setNotes(0, 0, one.notes, one.count);
    session.play();
    std::vector<int16_t> M;
    for (int done = 0; done < 24000; done += 250) {
        engine.render(buf, 250);
        for (int i = 0; i < 250; ++i)
            M.push_back(buf[i * 2]);
    }
    CHECK(M[14999] == 0 && M[15000] != 0);
    size_t e2 = 15000;
    while (e2 < M.size() && M[e2] != 0)
        ++e2;
    CHECK(e2 - 15000 >= 750 && e2 - 15000 <= 750 + 300);
}

static void testWaveformCache()
{
    SampleBank bank;
    int16_t* d = (int16_t*)malloc(200000 * sizeof(int16_t));
    for (int i = 0; i < 200000; ++i)
        d[i] = (int16_t)(i < 100000 ? 4000 : 20000); // quiet first half, loud second half
    const int slot = bank.add("W", d, 200000, 48000, 1, false, "samples:W.WAV");
    CHECK(slot >= 0);
    WaveformCache cache;
    CHECK(!cache.ready(slot));
    int ticks = 0;
    while (!cache.ready(slot) && ticks < 100) {
        cache.tick(bank);
        ++ticks;
    }
    CHECK(cache.ready(slot) && ticks > 1 && ticks <= 8);
    const uint8_t* p = cache.peaks(slot);
    CHECK(p && p[10] == 4000 >> 7 && p[120] == 20000 >> 7);
    const int adp = bank.addHwOnly("A", "samples:A.ADP", 100);
    for (int i = 0; i < 4; ++i)
        cache.tick(bank);
    CHECK(cache.ready(adp) && cache.peaks(adp)[5] == 0);
    bank.requestRelease(slot);
    bank.acknowledgeRelease(slot);
    bank.reap();
    int16_t* d2 = (int16_t*)calloc(1000, sizeof(int16_t));
    const int slot2 = bank.add("X", d2, 1000, 48000, 1, false, "samples:X.WAV");
    CHECK(slot2 == slot);
    for (int i = 0; i < 6; ++i)
        cache.tick(bank);
    CHECK(cache.ready(slot2) && cache.peaks(slot2)[10] == 0);
}

int runNoteEditTests()
{
    printf("running piano roll edit tests\n");
    testNoteTiming();
    testSelectionAndMove();
    testTransposeQuantizeVelocity();
    testChop();
    testCopyPasteDuplicateDelete();
    testSessionNotesAndPersistence();
    testSubStepPlayback();
    testWaveformCache();
    return g_fail;
}
