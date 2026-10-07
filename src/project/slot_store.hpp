// Crash-safe project slots on removable storage.
//
// A save never overwrites the only good copy:
//   1. serialise, write SLOTn.TMP, read it back and validate it completely
//   2. move the current SLOTn.ps2daw to SLOTn.BAK (one generation kept)
//   3. move SLOTn.TMP into place
// Pulling the stick or hitting a write error at any point leaves either the
// old file, the old file as .BAK, or the new file, never a torn project.
// Loading falls back to the .BAK when the main file is missing or corrupt.
//
// The filesystem is reached through ProjectFiles so the same logic runs
// against an in-memory fake in the host tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "project/project.hpp"

class ProjectFiles {
public:
    virtual ~ProjectFiles() {}
    // Whole-file read with a size cap: bytes read, -1 on error/missing, -2 if larger than cap.
    virtual int readAll(const char* path, uint8_t* buf, size_t cap) = 0;
    virtual bool writeAll(const char* path, const uint8_t* data, size_t len) = 0;
    virtual bool renameTo(const char* from, const char* to) = 0;
    virtual bool removeFile(const char* path) = 0;
    virtual bool exists(const char* path) = 0;
};

namespace slotstore {

constexpr int kSlots = 8;

struct Info {
    bool exists;          // a main or backup file was found
    bool fromBackup;      // the main file is missing/corrupt; this is the .BAK
    bool corrupt;         // neither file could be loaded
    char name[24];
    uint32_t bpmCenti;
    int externalSamples;  // channels referencing samples on storage
};

// `dir` is the app directory ("mass0:/PS2DAW"). `scratch` and `verify` are
// caller-provided buffers of `cap` bytes (projects are small; they stay off
// the stack).
bool save(ProjectFiles& fs, const char* dir, int slot, const Project& p, uint8_t* scratch, uint8_t* verify, size_t cap,
          char* err, size_t errCap);

bool load(ProjectFiles& fs, const char* dir, int slot, Project& out, uint8_t* scratch, size_t cap, bool* usedBackup,
          char* err, size_t errCap);

// Reads slot metadata for display (full parse; slots are a few KB).
Info peek(ProjectFiles& fs, const char* dir, int slot, uint8_t* scratch, size_t cap);

bool slotPath(char* out, size_t cap, const char* dir, int slot, const char* ext);

} // namespace slotstore
