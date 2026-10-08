#include "platform/ps2_export_file.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool UsbExportFile::open(const char* path)
{
    if (!storage_.available())
        return false;
    close();
    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return fd_ >= 0;
}

bool UsbExportFile::write(const void* data, uint32_t bytes)
{
    if (fd_ < 0)
        return false;
    const uint8_t* p = (const uint8_t*)data;
    uint32_t done = 0;
    while (done < bytes) {
        const uint32_t want = bytes - done;
        const ssize_t n = ::write(fd_, p + done, want > 32768 ? 32768 : want);
        if (n <= 0)
            return false; // disk full, drive removed, or a bus error
        done += (uint32_t)n;
    }
    return true;
}

bool UsbExportFile::rewriteHeader(const uint8_t* hdr, uint32_t bytes)
{
    if (fd_ < 0 || lseek(fd_, 0, SEEK_SET) < 0)
        return false;
    return write(hdr, bytes);
}

bool UsbExportFile::close()
{
    if (fd_ < 0)
        return true;
    const int rc = ::close(fd_);
    fd_ = -1;
    return rc >= 0;
}

bool UsbExportFile::readHeader(const char* path, uint8_t* out, uint32_t bytes, uint32_t* fileSize)
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0)
        return false;
    const off_t size = lseek(fd, 0, SEEK_END);
    bool ok = size >= (off_t)bytes && lseek(fd, 0, SEEK_SET) >= 0;
    uint32_t got = 0;
    while (ok && got < bytes) {
        const ssize_t n = ::read(fd, out + got, bytes - got);
        if (n <= 0)
            ok = false;
        else
            got += (uint32_t)n;
    }
    ::close(fd);
    if (ok && fileSize)
        *fileSize = (uint32_t)size;
    return ok;
}

bool UsbExportFile::rename(const char* from, const char* to)
{
    // FAT cannot rename over an existing file: park the old export aside first
    // and only delete it once the new one is in place.
    char old[200];
    snprintf(old, sizeof(old), "%s.OLD", to);
    struct stat st;
    const bool hadOld = stat(to, &st) == 0;
    if (hadOld) {
        unlink(old);
        if (::rename(to, old) != 0)
            return false;
    }
    if (::rename(from, to) != 0) {
        if (hadOld)
            ::rename(old, to);
        return false;
    }
    if (hadOld)
        unlink(old);
    return true;
}

void UsbExportFile::remove(const char* path)
{
    if (storage_.available())
        unlink(path);
}
