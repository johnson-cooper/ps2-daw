#include "project/note_edit.hpp"

#include <string.h>

namespace noteedit {
namespace {

inline uint64_t bit(int i) { return (uint64_t)1 << i; }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool targeted(const Set& s, int i, bool all) { return all || (s.sel & bit(i)); }

constexpr int kMaxTicks = cfg::kMaxSteps * kTicks;

// Removes the notes in `mask` and compacts the selection to match.
int removeMask(Set& s, uint64_t mask)
{
    int out = 0, removed = 0;
    uint64_t sel = 0;
    for (int i = 0; i < s.count; ++i) {
        if (mask & bit(i)) {
            ++removed;
            continue;
        }
        if (s.sel & bit(i))
            sel |= bit(out);
        s.notes[out++] = s.notes[i];
    }
    for (int i = out; i < s.count; ++i)
        s.notes[i] = PianoNote();
    s.count = out;
    s.sel = sel;
    return removed;
}

// Two notes on the same (start, pitch) cannot exist: a selected one wins over an
// unselected one (it is the one that was just moved or pasted), otherwise the later wins.
void dedupe(Set& s)
{
    uint64_t drop = 0;
    for (int i = 0; i < s.count; ++i) {
        if (drop & bit(i))
            continue;
        for (int j = i + 1; j < s.count; ++j) {
            if (drop & bit(j) || s.notes[i].startTicks() != s.notes[j].startTicks() || s.notes[i].pitch != s.notes[j].pitch)
                continue;
            const bool si = (s.sel & bit(i)) != 0, sj = (s.sel & bit(j)) != 0;
            if (si && !sj)
                drop |= bit(j);
            else
                drop |= bit(i);
        }
    }
    if (drop)
        removeMask(s, drop);
}

} // namespace

void setStart(PianoNote& n, int ticks)
{
    ticks = clampi(ticks, 0, kMaxTicks - 1);
    n.step = (uint8_t)(ticks / kTicks);
    n.tick = (uint8_t)(ticks % kTicks);
}

void setDuration(PianoNote& n, int ticks)
{
    ticks = clampi(ticks, 1, kMaxTicks);
    if (ticks % kTicks == 0) {
        n.lenTicks = 0;
        n.length = (uint8_t)(ticks / kTicks);
    } else {
        n.lenTicks = (uint16_t)ticks;
        n.length = (uint8_t)((ticks + kTicks - 1) / kTicks);
    }
}

void load(Set& s, const PatternData& pattern, int channel)
{
    s = Set();
    const int n = pattern.noteCount[channel] > cfg::kMaxNotes ? cfg::kMaxNotes : pattern.noteCount[channel];
    for (int i = 0; i < n; ++i)
        s.notes[i] = pattern.notes[channel][i];
    s.count = n;
}

int indexAt(const Set& s, int startTicks, int pitch)
{
    for (int i = 0; i < s.count; ++i)
        if (s.notes[i].startTicks() == startTicks && s.notes[i].pitch == pitch)
            return i;
    return -1;
}

int indexCovering(const Set& s, int ticks, int pitch)
{
    for (int i = 0; i < s.count; ++i) {
        const int a = s.notes[i].startTicks();
        if (s.notes[i].pitch == pitch && ticks >= a && ticks < a + s.notes[i].durTicks())
            return i;
    }
    return -1;
}

int selectedCount(const Set& s)
{
    int n = 0;
    for (int i = 0; i < s.count; ++i)
        n += (s.sel & bit(i)) ? 1 : 0;
    return n;
}

bool isSelected(const Set& s, int i) { return i >= 0 && i < s.count && (s.sel & bit(i)); }
void toggleSelect(Set& s, int i)
{
    if (i >= 0 && i < s.count)
        s.sel ^= bit(i);
}
void selectNone(Set& s) { s.sel = 0; }
void selectAll(Set& s)
{
    s.sel = 0;
    for (int i = 0; i < s.count; ++i)
        s.sel |= bit(i);
}
void selectColumn(Set& s, int step)
{
    s.sel = 0;
    for (int i = 0; i < s.count; ++i)
        if (s.notes[i].step == step)
            s.sel |= bit(i);
}
void selectRow(Set& s, int pitch)
{
    s.sel = 0;
    for (int i = 0; i < s.count; ++i)
        if (s.notes[i].pitch == pitch)
            s.sel |= bit(i);
}
void selectFrom(Set& s, int ticks)
{
    s.sel = 0;
    for (int i = 0; i < s.count; ++i)
        if (s.notes[i].startTicks() >= ticks)
            s.sel |= bit(i);
}

bool move(Set& s, int dTicks, int dPitch, int patternTicks, bool all)
{
    for (int i = 0; i < s.count; ++i) {
        if (!targeted(s, i, all))
            continue;
        const int ns = s.notes[i].startTicks() + dTicks, np = s.notes[i].pitch + dPitch;
        if (ns < 0 || ns >= patternTicks || ns >= kMaxTicks || np < 0 || np > 127)
            return false;
    }
    for (int i = 0; i < s.count; ++i)
        if (targeted(s, i, all)) {
            setStart(s.notes[i], s.notes[i].startTicks() + dTicks);
            s.notes[i].pitch = (uint8_t)(s.notes[i].pitch + dPitch);
        }
    dedupe(s);
    return true;
}

bool transpose(Set& s, int semitones, bool all)
{
    return move(s, 0, semitones, kMaxTicks, all);
}

void quantize(Set& s, int snap, int patternTicks, bool all)
{
    if (snap <= 1)
        return;
    for (int i = 0; i < s.count; ++i) {
        if (!targeted(s, i, all))
            continue;
        PianoNote& n = s.notes[i];
        int st = ((n.startTicks() + snap / 2) / snap) * snap;
        if (st >= patternTicks)
            st -= snap;
        const int dur = n.durTicks();
        int len = ((dur + snap / 2) / snap) * snap;
        if (len < snap)
            len = snap;
        setStart(n, st < 0 ? 0 : st);
        setDuration(n, len);
    }
    dedupe(s);
}

void setLength(Set& s, int steps, bool all)
{
    setLengthTicks(s, steps * kTicks, all);
}

void setLengthTicks(Set& s, int ticks, bool all)
{
    for (int i = 0; i < s.count; ++i)
        if (targeted(s, i, all))
            setDuration(s.notes[i], ticks);
}

void addLength(Set& s, int deltaTicks, bool all)
{
    for (int i = 0; i < s.count; ++i)
        if (targeted(s, i, all))
            setDuration(s.notes[i], s.notes[i].durTicks() + deltaTicks);
}

void setVelocity(Set& s, int velocity, bool all)
{
    for (int i = 0; i < s.count; ++i)
        if (targeted(s, i, all))
            s.notes[i].velocity = (uint8_t)clampi(velocity, 1, 127);
}

void addVelocity(Set& s, int delta, bool all)
{
    for (int i = 0; i < s.count; ++i)
        if (targeted(s, i, all))
            s.notes[i].velocity = (uint8_t)clampi(s.notes[i].velocity + delta, 1, 127);
}

int removeSelected(Set& s)
{
    return removeMask(s, s.sel);
}

bool chop(Set& s, int parts, bool all)
{
    if (parts < 2)
        return false;
    // count first so that nothing changes when it would not fit
    int extra = 0;
    for (int i = 0; i < s.count; ++i)
        if (targeted(s, i, all) && s.notes[i].durTicks() >= 2 * parts)
            extra += parts - 1;
    if (extra == 0 || s.count + extra > cfg::kMaxNotes)
        return false;
    Set out;
    out.count = 0;
    uint64_t sel = 0;
    for (int i = 0; i < s.count; ++i) {
        const PianoNote n = s.notes[i];
        if (!targeted(s, i, all) || n.durTicks() < 2 * parts) {
            out.notes[out.count] = n;
            if (s.sel & bit(i))
                sel |= bit(out.count);
            ++out.count;
            continue;
        }
        const int start = n.startTicks(), dur = n.durTicks();
        const int piece = dur / parts;
        for (int k = 0; k < parts; ++k) {
            PianoNote q = n;
            setStart(q, start + k * piece);
            setDuration(q, k == parts - 1 ? dur - piece * (parts - 1) : piece);
            sel |= bit(out.count);
            out.notes[out.count++] = q;
        }
    }
    s.count = out.count;
    memcpy(s.notes, out.notes, sizeof(s.notes));
    s.sel = sel;
    dedupe(s);
    return true;
}

int copy(const Set& s, Clip& out)
{
    out = Clip();
    const bool any = selectedCount(s) > 0;
    int minStart = 1 << 30, minPitch = 255, maxEnd = 0;
    for (int i = 0; i < s.count; ++i) {
        if (any && !(s.sel & bit(i)))
            continue;
        const PianoNote& n = s.notes[i];
        minStart = n.startTicks() < minStart ? n.startTicks() : minStart;
        minPitch = n.pitch < minPitch ? n.pitch : minPitch;
        maxEnd = n.startTicks() + n.durTicks() > maxEnd ? n.startTicks() + n.durTicks() : maxEnd;
    }
    for (int i = 0; i < s.count; ++i) {
        if (any && !(s.sel & bit(i)))
            continue;
        PianoNote n = s.notes[i];
        setStart(n, n.startTicks() - minStart);
        n.pitch = (uint8_t)(n.pitch - minPitch);
        out.notes[out.count++] = n;
    }
    out.spanTicks = out.count ? maxEnd - minStart : 0;
    return out.count;
}

bool paste(Set& s, const Clip& clip, int ticks, int pitch, int patternTicks)
{
    if (clip.count == 0)
        return false;
    Set work = s;
    work.sel = 0;
    uint64_t pasted = 0;
    for (int i = 0; i < clip.count; ++i) {
        const int ns = ticks + clip.notes[i].startTicks(), np = pitch + clip.notes[i].pitch;
        if (ns < 0 || ns >= patternTicks || ns >= kMaxTicks || np < 0 || np > 127)
            continue;
        int idx = indexAt(work, ns, np);
        if (idx < 0) {
            if (work.count >= cfg::kMaxNotes)
                return false; // would not fit: change nothing
            idx = work.count++;
        }
        work.notes[idx] = clip.notes[i];
        setStart(work.notes[idx], ns);
        work.notes[idx].pitch = (uint8_t)np;
        pasted |= bit(idx);
    }
    if (!pasted)
        return false;
    work.sel = pasted;
    s = work;
    return true;
}

bool duplicate(Set& s, int patternTicks)
{
    Clip clip;
    if (copy(s, clip) == 0)
        return false;
    int minStart = 1 << 30, minPitch = 255;
    const bool any = selectedCount(s) > 0;
    for (int i = 0; i < s.count; ++i) {
        if (any && !(s.sel & bit(i)))
            continue;
        minStart = s.notes[i].startTicks() < minStart ? s.notes[i].startTicks() : minStart;
        minPitch = s.notes[i].pitch < minPitch ? s.notes[i].pitch : minPitch;
    }
    return paste(s, clip, minStart + (clip.spanTicks > 0 ? clip.spanTicks : kTicks), minPitch, patternTicks);
}

} // namespace noteedit
