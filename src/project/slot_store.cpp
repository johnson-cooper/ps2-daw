#include "project/slot_store.hpp"

#include <stdio.h>
#include <string.h>

#include "audio/sample_ref.hpp"
#include "project/project_io.hpp"

namespace slotstore {

static bool fail(char* err, size_t cap, const char* msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
    return false;
}

bool slotPath(char* out, size_t cap, const char* dir, int slot, const char* ext)
{
    if (slot < 1 || slot > kSlots)
        return false;
    const int n = snprintf(out, cap, "%s/SLOT%d.%s", dir, slot, ext);
    return n > 0 && (size_t)n < cap;
}

bool save(ProjectFiles& fs, const char* dir, int slot, const Project& p, uint8_t* scratch, uint8_t* verify, size_t cap,
          char* err, size_t errCap)
{
    char main[96], tmp[96], bak[96];
    if (!slotPath(main, sizeof(main), dir, slot, "ps2daw") || !slotPath(tmp, sizeof(tmp), dir, slot, "TMP") ||
        !slotPath(bak, sizeof(bak), dir, slot, "BAK"))
        return fail(err, errCap, "bad slot or path");

    const size_t n = projectio::save(p, scratch, cap);
    if (n == 0)
        return fail(err, errCap, "project too large");

    if (!fs.writeAll(tmp, scratch, n)) {
        fs.removeFile(tmp);
        return fail(err, errCap, "write failed (drive full or read-only?)");
    }
    // Read back and validate the temp file completely before touching the old one.
    static Project check; // ~6 KiB; kept off the stack
    const int got = fs.readAll(tmp, verify, cap);
    char why[48];
    if (got != (int)n || memcmp(verify, scratch, n) != 0 || !projectio::load(verify, (size_t)got, check, why, sizeof(why))) {
        fs.removeFile(tmp);
        return fail(err, errCap, "verify failed, old save kept");
    }

    const bool hadMain = fs.exists(main);
    if (hadMain) {
        fs.removeFile(bak);
        if (!fs.renameTo(main, bak)) {
            fs.removeFile(tmp);
            return fail(err, errCap, "cannot rotate backup, old save kept");
        }
    }
    if (!fs.renameTo(tmp, main)) {
        // Put the previous file back so the slot is not left empty.
        if (hadMain)
            fs.renameTo(bak, main);
        fs.removeFile(tmp);
        return fail(err, errCap, "cannot finalize save, old save kept");
    }
    return true;
}

static bool loadOne(ProjectFiles& fs, const char* path, Project& out, uint8_t* scratch, size_t cap, char* err, size_t errCap)
{
    const int n = fs.readAll(path, scratch, cap);
    if (n == -2)
        return fail(err, errCap, "file too large");
    if (n < 0)
        return fail(err, errCap, "cannot read file");
    return projectio::load(scratch, (size_t)n, out, err, errCap);
}

bool load(ProjectFiles& fs, const char* dir, int slot, Project& out, uint8_t* scratch, size_t cap, bool* usedBackup,
          char* err, size_t errCap)
{
    if (usedBackup)
        *usedBackup = false;
    char main[96], bak[96];
    if (!slotPath(main, sizeof(main), dir, slot, "ps2daw") || !slotPath(bak, sizeof(bak), dir, slot, "BAK"))
        return fail(err, errCap, "bad slot or path");
    char mainErr[64] = "";
    if (loadOne(fs, main, out, scratch, cap, mainErr, sizeof(mainErr)))
        return true;
    if (fs.exists(bak) && loadOne(fs, bak, out, scratch, cap, nullptr, 0)) {
        if (usedBackup)
            *usedBackup = true;
        return true;
    }
    return fail(err, errCap, mainErr[0] ? mainErr : "cannot load");
}

Info peek(ProjectFiles& fs, const char* dir, int slot, uint8_t* scratch, size_t cap)
{
    Info info;
    memset(&info, 0, sizeof(info));
    char main[96], bak[96];
    if (!slotPath(main, sizeof(main), dir, slot, "ps2daw") || !slotPath(bak, sizeof(bak), dir, slot, "BAK"))
        return info;
    const bool haveMain = fs.exists(main), haveBak = fs.exists(bak);
    if (!haveMain && !haveBak)
        return info;
    info.exists = true;
    static Project p; // ~5 KiB; kept off the stack
    bool backup = false;
    if (!load(fs, dir, slot, p, scratch, cap, &backup, nullptr, 0)) {
        info.corrupt = true;
        return info;
    }
    info.fromBackup = backup;
    memcpy(info.name, p.name, sizeof(info.name));
    info.name[sizeof(info.name) - 1] = '\0';
    info.bpmCenti = p.bpmCenti;
    for (int ch = 0; ch < p.channelCount; ++ch)
        if (sampleref::classify(p.channels[ch].sampleRef) == sampleref::Kind::Samples)
            ++info.externalSamples;
    return info;
}

} // namespace slotstore
