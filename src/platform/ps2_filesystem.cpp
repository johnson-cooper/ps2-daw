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
    if (!driversOk_ || readyRoot_ >= 0)
        return;
    if (firstProbeMs_ == 0)
        firstProbeMs_ = nowMs ? nowMs : 1;
    // USB enumeration takes a second or two; probe every 500 ms for the first
    // 10 s, then every 3 s so a drive plugged in later is still found.
    const uint32_t interval = (nowMs - firstProbeMs_ < 10000) ? 500 : 3000;
    if (probes_ && nowMs - lastProbeMs_ < interval)
        return;
    lastProbeMs_ = nowMs;
    ++probes_;
    for (int i = 0; i < kMaxRoots; ++i) {
        char path[16];
        snprintf(path, sizeof(path), "%s/", kRoots[i]);
        if (dirExists(path)) {
            readyRoot_ = i;
            log.set(Subsystem::Storage, Health::Ok, "%s ready (FAT via BDM)", kRoots[i]);
            return;
        }
    }
    if (probes_ == 20)
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
        close(fd);
        return -1;
    }
    if ((size_t)size > cap) {
        close(fd);
        return -2;
    }
    size_t done = 0;
    while (done < (size_t)size) {
        const size_t want = (size_t)size - done;
        const ssize_t n = read(fd, buf + done, want > 32768 ? 32768 : want);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    close(fd);
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
    const int rc = close(fd);
    return done == len && rc >= 0;
}

bool Storage::ensureDir(const char* path) const
{
    if (!driversOk_)
        return false;
    return dirExists(path) || mkdir(path, 0777) == 0;
}

int Storage::listDir(const char* path, DirEntry* out, int max) const
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
