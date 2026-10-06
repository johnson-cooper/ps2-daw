#include "core/status_log.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

StatusLog::StatusLog() : errorSerial_(0)
{
    for (auto& e : entries_) {
        e.health = Health::Pending;
        e.detail[0] = '\0';
    }
    lastError_[0] = '\0';
}

void StatusLog::set(Subsystem s, Health h, const char* fmt, ...)
{
    Entry& e = entries_[(int)s];
    e.health = h;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.detail, sizeof(e.detail), fmt, ap);
    va_end(ap);
}

void StatusLog::fail(Subsystem s, const char* fmt, ...)
{
    Entry& e = entries_[(int)s];
    e.health = Health::Failed;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.detail, sizeof(e.detail), fmt, ap);
    va_end(ap);
    snprintf(lastError_, sizeof(lastError_), "%s: %s", name(s), e.detail);
    ++errorSerial_;
}

void StatusLog::error(Subsystem s, const char* fmt, ...)
{
    char msg[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(lastError_, sizeof(lastError_), "%s: %s", name(s), msg);
    ++errorSerial_;
}

const char* StatusLog::name(Subsystem s)
{
    switch (s) {
    case Subsystem::Iop: return "IOP";
    case Subsystem::Graphics: return "GS";
    case Subsystem::Controller: return "PAD";
    case Subsystem::AudioIrx: return "AUDIO IRX";
    case Subsystem::AudioInit: return "AUDIO_INIT";
    case Subsystem::AudioStream: return "PCM STREAM";
    case Subsystem::Spu2: return "SPU2 VOICES";
    case Subsystem::Samples: return "SAMPLES";
    case Subsystem::Storage: return "STORAGE";
    case Subsystem::Project: return "PROJECT";
    default: return "?";
    }
}

const char* StatusLog::healthName(Health h)
{
    switch (h) {
    case Health::Pending: return "....";
    case Health::Ok: return " OK ";
    case Health::Warning: return "WARN";
    case Health::Failed: return "FAIL";
    case Health::Skipped: return "SKIP";
    default: return "?";
    }
}
