// ExportFile on USB mass storage (POSIX calls routed through fileXio), plus the
// RenderHold that parks the realtime render thread for the duration of a render.
#pragma once

#include "audio/wav_export.hpp"
#include "platform/ps2_audio.hpp"
#include "platform/ps2_filesystem.hpp"

class UsbExportFile : public ExportFile {
public:
    explicit UsbExportFile(const Storage& storage) : storage_(storage) {}
    bool open(const char* path) override;
    bool write(const void* data, uint32_t bytes) override;
    bool rewriteHeader(const uint8_t* hdr, uint32_t bytes) override;
    bool close() override;
    bool readHeader(const char* path, uint8_t* out, uint32_t bytes, uint32_t* fileSize) override;
    bool rename(const char* from, const char* to) override;
    void remove(const char* path) override;

private:
    const Storage& storage_;
    int fd_ = -1;
};

class Ps2AudioHold : public RenderHold {
public:
    explicit Ps2AudioHold(Ps2Audio& audio) : audio_(audio) {}
    bool hold() override { return audio_.hold(500); }
    void release() override { audio_.release(); }

private:
    Ps2Audio& audio_;
};
