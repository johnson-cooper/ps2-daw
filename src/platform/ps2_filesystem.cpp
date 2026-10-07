#include "platform/ps2_filesystem.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bdm_irx.h"
#include "bdmfs_fatfs_irx.h"
#include "filexio_irx.h"
#include "iomanx_irx.h"
#include "usbd_irx.h"
#include "usbmass_bd_irx.h"

#include "audio/sample_ref.hpp"
#include "core/strutil.hpp"
#include "platform/ps2_system.hpp"

// The SDK wants newlib programs to use POSIX calls rather than the fileXio
// API: fileXioInit() binds the RPC server and then routes open/read/
// opendir/mkdir/... through it. That one call is all we need, so declare it
// here instead of including fileXio_rpc.h (which rejects newlib builds).
extern "C" int fileXioInit(void);

namespace {

const char* const kRoots[Storage::kMaxRoots] = {"mass0:", "mass1:"};

bool dirExists(const char* path)
{
    DIR* d = opendir(path);
    if (!d)
        return false;
    closedir(d);
    return true;
}

} // namespace

void Storage::init(StatusLog& log, bool skip, const char* elfPath)
{
    str::copy(elfPath_, sizeof(elfPath_), elfPath ? elfPath : "");
    if (skip) {
        log.set(Subsystem::Storage, Health::Skipped, "skipped (SELECT held at boot)");
        return;
    }
    // Order matters: iomanX before anything that registers devices, fileXio
    // as the EE's RPC door, then the USB host and block-device layers.
    struct Mod {
        const char* name;
        const void* image;
        unsigned size;
    };
    const Mod mods[] = {
        {"iomanX", iomanx_irx, size_iomanx_irx},
        {"fileXio", filexio_irx, size_filexio_irx},
        {"usbd", usbd_irx, size_usbd_irx},
        {"bdm", bdm_irx, size_bdm_irx},
        {"bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx},
        {"usbmass_bd", usbmass_bd_irx, size_usbmass_bd_irx},
    };
    for (const Mod& m : mods) {
        if (!ps2sys::loadModule(m.name, m.image, m.size)) {
            log.fail(Subsystem::Storage, "%s.irx failed to load", m.name);
            return;
        }
    }
    // Only bind the fileXio RPC once its server is known to be resident:
    // fileXioInit() spins until the server answers.
    if (fileXioInit() < 0) {
        log.fail(Subsystem::Storage, "fileXioInit failed");
        return;
    }
    driversOk_ = true;
    log.set(Subsystem::Storage, Health::Warning, "USB drivers loaded, looking for a drive");
}

void Storage::poll(uint32_t nowMs, StatusLog& log)
{
    // Keep watching until both roots have been seen, at a slow rate once
    // one is up, so a second stick plugged in later is still found.
    if (!driversOk_ || rootMask_ == (1u << kMaxRoots) - 1)
        return;
    if (firstProbeMs_ == 0)
        firstProbeMs_ = nowMs ? nowMs : 1;
    // USB enumeration takes a second or two; probe every 500 ms for the first
    // 10 s, then every 3 s.
    const uint32_t interval = (readyRoot_ < 0 && nowMs - firstProbeMs_ < 10000) ? 500 : 3000;
    if (probes_ && nowMs - lastProbeMs_ < interval)
        return;
    lastProbeMs_ = nowMs;
    ++probes_;
    for (int i = 0; i < kMaxRoots; ++i) {
        if (rootMask_ & (1u << i))
            continue;
        char path[16];
        snprintf(path, sizeof(path), "%s/", kRoots[i]);
        if (dirExists(path)) {
            rootMask_ |= 1u << i;
            if (readyRoot_ < 0)
                readyRoot_ = i;
            log.set(Subsystem::Storage, Health::Ok, "%s ready (FAT via BDM)%s", kRoots[readyRoot_],
                    rootMask_ == (1u << kMaxRoots) - 1 ? " +2nd drive" : "");
        }
    }
    if (probes_ == 20 && readyRoot_ < 0)
        log.set(Subsystem::Storage, Health::Warning, "no USB drive found (still watching)");
}

const char* Storage::rootName() const
{
    return readyRoot_ >= 0 ? kRoots[readyRoot_] : "none";
}

bool Storage::appPath(char* out, size_t cap, const char* rel) const
{
    if (readyRoot_ < 0)
        return false;
    const int n = snprintf(out, cap, "%s/%s%s%s", kRoots[readyRoot_], kAppDir, rel && *rel ? "/" : "", rel ? rel : "");
    return n > 0 && (size_t)n < cap;
}

int Storage::readFile(const char* path, uint8_t* buf, size_t cap) const
{
    if (!driversOk_)
        return -1;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    // Check the real length before reading anything.
    const off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        return -1;
    }
    if ((size_t)size > cap) {
        ::close(fd);
        return -2;
    }
    size_t done = 0;
    while (done < (size_t)size) {
        const size_t want = (size_t)size - done;
        const ssize_t n = ::read(fd, buf + done, want > 32768 ? 32768 : want);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    ::close(fd);
    return done == (size_t)size ? (int)size : -1;
}

bool Storage::writeFile(const char* path, const uint8_t* data, size_t len) const
{
    if (!driversOk_)
        return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < len) {
        const size_t want = len - done;
        const ssize_t n = write(fd, data + done, want > 32768 ? 32768 : want);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    const int rc = ::close(fd);
    return done == len && rc >= 0;
}

bool Storage::ensureDir(const char* path) const
{
    if (!driversOk_)
        return false;
    return dirExists(path) || mkdir(path, 0777) == 0;
}

bool Storage::fileExists(const char* path) const
{
    struct stat st;
    return driversOk_ && stat(path, &st) == 0;
}

bool Storage::removeFile(const char* path) const
{
    return driversOk_ && unlink(path) == 0;
}

bool Storage::renameFile(const char* from, const char* to) const
{
    return driversOk_ && rename(from, to) == 0;
}

int Storage::listDir(const char* path, DirEntry* out, int max, bool* truncated) const
{
    if (!driversOk_)
        return -1;
    DIR* d = opendir(path);
    if (!d)
        return -1;
    int n = 0;
    while (n < max) {
        const struct dirent* e = readdir(d);
        if (!e)
            break;
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        str::copy(out[n].name, sizeof(out[n].name), e->d_name);
        char full[320];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            out[n].size = (uint32_t)st.st_size;
            out[n].isDir = S_ISDIR(st.st_mode);
        } else {
            out[n].size = 0;
            out[n].isDir = false;
        }
        ++n;
    }
    closedir(d);
    return n;
}

int Storage::listSamples(const char* rel, DirEntry* out, int max, bool* truncated) const
{
    if (truncated)
        *truncated = false;
    if (!driversOk_ || max <= 0)
        return -1;
    if (rel && *rel && !sampleref::validRelPath(rel))
        return -1;
    for (int i = 0; i < kMaxRoots; ++i) {
        if (!rootReady(i))
            continue;
        char path[160];
        if (!sampleref::devicePath(path, sizeof(path), kRoots[i], rel))
            return -1;
        const int n = listDir(path, out, max, truncated);
        if (n >= 0)
            return n;
    }
    return -1;
}

bool Storage::createSampleDir() const
{
    if (readyRoot_ < 0)
        return false;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", kRoots[readyRoot_], kAppDir);
    if (!ensureDir(path))
        return false;
    if (!sampleref::devicePath(path, sizeof(path), kRoots[readyRoot_], ""))
        return false;
    return ensureDir(path);
}

int Storage::openSample(const char* rel, uint32_t* size)
{
    if (!driversOk_ || !rel || !sampleref::validRelPath(rel))
        return -1;
    // Try the root that came up first, then the other: a project saved with
    // the drive in one port still finds its samples in the other.
    for (int k = 0; k < kMaxRoots; ++k) {
        const int i = (readyRoot_ + k) % kMaxRoots;
        if (readyRoot_ < 0 || !rootReady(i))
            continue;
        char path[192];
        if (!sampleref::devicePath(path, sizeof(path), kRoots[i], rel))
            return -1;
        const int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        const off_t len = lseek(fd, 0, SEEK_END);
        if (len < 0 || lseek(fd, 0, SEEK_SET) < 0 || (uint64_t)len > 0xffffffffu) {
            ::close(fd);
            return -1;
        }
        if (size)
            *size = (uint32_t)len;
        return fd;
    }
    return -1;
}

int Storage::read(int handle, uint8_t* buf, uint32_t n)
{
    const ssize_t r = ::read(handle, buf, n);
    return r < 0 ? -1 : (int)r;
}

void Storage::close(int handle)
{
    if (handle >= 0)
        ::close(handle);
}
