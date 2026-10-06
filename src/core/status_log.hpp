// Per-subsystem health table plus a sticky "last error" line.
//
// On real hardware there is no console, so every subsystem reports here and
// the boot screen, status bar and debug overlay all render from this table.
// Errors stay until replaced; nothing flashes for a single frame.
#pragma once

#include <stdint.h>

enum class Subsystem : uint8_t {
    Iop,
    Graphics,
    Controller,
    AudioIrx,
    AudioInit,
    AudioStream,
    Spu2,
    Samples,
    Storage,
    Project,
    Count
};

enum class Health : uint8_t {
    Pending,
    Ok,
    Warning,
    Failed,
    Skipped,
};

class StatusLog {
public:
    static constexpr int kDetailLen = 56;

    struct Entry {
        Health health;
        char detail[kDetailLen];
    };

    StatusLog();

    void set(Subsystem s, Health h, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

    // Records a failure for the subsystem and makes it the sticky last error.
    void fail(Subsystem s, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    // Sticky error line without changing subsystem health (e.g. a rejected file).
    void error(Subsystem s, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    const Entry& entry(Subsystem s) const { return entries_[(int)s]; }
    bool ok(Subsystem s) const { return entries_[(int)s].health == Health::Ok; }

    const char* lastError() const { return lastError_; }
    bool hasError() const { return lastError_[0] != '\0'; }
    uint32_t errorSerial() const { return errorSerial_; }
    void clearLastError() { lastError_[0] = '\0'; }

    static const char* name(Subsystem s);
    static const char* healthName(Health h);

private:
    Entry entries_[(int)Subsystem::Count];
    char lastError_[96];
    uint32_t errorSerial_;
};
