// ExportFile that writes into a caller-provided memory block. Used by the host
// tests and by the PCSX2 debug command that exercises the exporter without USB.
#pragma once

#include <stdint.h>
#include <string.h>

#include "audio/wav_export.hpp"

class RamExportFile : public ExportFile {
public:
    RamExportFile(uint8_t* buf, uint32_t cap) : buf_(buf), cap_(cap) {}

    uint32_t size() const { return size_; }
    uint8_t* data() { return buf_; }
    bool overflowed() const { return overflow_; }

    bool open(const char* path) override
    {
        size_ = 0;
        overflow_ = false;
        strncpy(name_, path, sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
        return buf_ != nullptr;
    }
    bool write(const void* d, uint32_t n) override
    {
        if (size_ + n > cap_) {
            overflow_ = true;
            return false;
        }
        memcpy(buf_ + size_, d, n);
        size_ += n;
        return true;
    }
    bool rewriteHeader(const uint8_t* h, uint32_t n) override
    {
        if (size_ < n)
            return false;
        memcpy(buf_, h, n);
        return true;
    }
    bool close() override { return true; }
    bool readHeader(const char* path, uint8_t* out, uint32_t n, uint32_t* fileSize) override
    {
        if (strcmp(path, name_) != 0 || size_ < n)
            return false;
        memcpy(out, buf_, n);
        *fileSize = size_;
        return true;
    }
    bool rename(const char*, const char* to) override
    {
        strncpy(name_, to, sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
        return true;
    }
    void remove(const char*) override {}

private:
    uint8_t* buf_;
    uint32_t cap_, size_ = 0;
    bool overflow_ = false;
    char name_[200] = "";
};
