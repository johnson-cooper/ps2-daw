// Storage devices and file access.
//
// USB mass storage uses the current block-device stack: usbd -> usbmass_bd
// -> bdm -> bdmfs_fatfs (FAT12/16/32), registered with iomanX and reached
// from the EE through fileXio. Devices appear as mass0:, mass1:, ...
//
// No path is hard-coded: the app asks for "the first ready root" and builds
// paths under it (PS2DAW/ on the device). Device enumeration takes a moment
// after the drivers load, so poll() probes without ever blocking the UI.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/sample_io.hpp"
#include "core/status_log.hpp"

class Storage : public SampleFileSource {
public:
    static constexpr int kMaxRoots = 2;
    static constexpr const char* kAppDir = "PS2DAW";

    struct DirEntry {
        char name[64];
        uint32_t size;
        bool isDir;
    };

    // Loads the embedded USB/FAT stack. With `skip` set (user held SELECT at
    // boot) nothing is loaded, as a recovery path for problem devices.
    void init(StatusLog& log, bool skip, const char* elfPath);
    // Probes for devices; call once per frame (rate limited internally).
    void poll(uint32_t nowMs, StatusLog& log);

    bool available() const { return driversOk_; }
    bool ready() const { return readyRoot_ >= 0; }
    bool rootReady(int i) const { return i >= 0 && i < kMaxRoots && (rootMask_ & (1u << i)); }
    const char* rootName() const; // "mass0:" or "none"
    const char* elfPath() const { return elfPath_; }

    // Builds "<root>/PS2DAW/<rel>" into out. Returns false if no device.
    bool appPath(char* out, size_t cap, const char* rel) const;

    // Whole-file helpers with explicit size limits (never trust lengths).
    // readFile returns bytes read, or -1 (error) / -2 (larger than cap).
    int readFile(const char* path, uint8_t* buf, size_t cap) const;
    bool writeFile(const char* path, const uint8_t* data, size_t len) const;
    bool ensureDir(const char* path) const;
    bool fileExists(const char* path) const;
    bool removeFile(const char* path) const;
    bool renameFile(const char* from, const char* to) const;
    // Lists up to `max` entries; returns count or -1.
    int listDir(const char* path, DirEntry* out, int max, bool* truncated = nullptr) const;

    // ---- Sample folder: <root>/PS2DAW/SAMPLES, on whichever root has it ----
    // Lists a sub-directory of the sample folder (rel may be ""). Returns the
    // entry count, -1 if no ready root has the folder (also: no USB drive).
    // At most `max` entries are returned; *truncated is set if there were more.
    int listSamples(const char* rel, DirEntry* out, int max, bool* truncated) const;
    // Creates <root>/PS2DAW/SAMPLES on the first ready root.
    bool createSampleDir() const;

    // SampleFileSource
    int openSample(const char* rel, uint32_t* size) override;
    int read(int handle, uint8_t* buf, uint32_t n) override;
    void close(int handle) override;

private:
    bool driversOk_ = false;
    int readyRoot_ = -1;
    uint32_t rootMask_ = 0;
    uint32_t lastProbeMs_ = 0;
    uint32_t firstProbeMs_ = 0;
    uint32_t probes_ = 0;
    char elfPath_[64] = "";
};
