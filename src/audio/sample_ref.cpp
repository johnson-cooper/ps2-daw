#include "audio/sample_ref.hpp"

#include <stdio.h>
#include <string.h>

#include "core/strutil.hpp"

namespace sampleref {

static bool startsWith(const char* s, const char* prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

Kind classify(const char* ref)
{
    if (!ref || !*ref)
        return Kind::None;
    if (startsWith(ref, kBuiltinPrefix))
        return ref[strlen(kBuiltinPrefix)] ? Kind::Builtin : Kind::Invalid;
    if (startsWith(ref, kSamplesPrefix))
        return validRelPath(ref + strlen(kSamplesPrefix)) ? Kind::Samples : Kind::Invalid;
    return Kind::Invalid;
}

bool validRelPath(const char* rel)
{
    if (!rel || !*rel)
        return false;
    const size_t len = strlen(rel);
    if (len >= kMaxRef - strlen(kSamplesPrefix))
        return false;
    size_t compStart = 0;
    for (size_t i = 0; i <= len; ++i) {
        const char c = rel[i];
        if (c == '/' || c == '\0') {
            const size_t n = i - compStart;
            if (n == 0)
                return false; // leading '/', "//" or trailing '/'
            const char* comp = rel + compStart;
            if ((n == 1 && comp[0] == '.') || (n == 2 && comp[0] == '.' && comp[1] == '.'))
                return false;
            if (comp[n - 1] == ' ' || comp[n - 1] == '.')
                return false; // FAT silently strips these: avoid aliasing
            compStart = i + 1;
            continue;
        }
        const unsigned char u = (unsigned char)c;
        if (u < 0x20 || u > 0x7e)
            return false;
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

bool make(char* out, size_t cap, const char* rel)
{
    if (!out || cap == 0)
        return false;
    out[0] = '\0';
    if (!validRelPath(rel))
        return false;
    const int n = snprintf(out, cap, "%s%s", kSamplesPrefix, rel);
    if (n < 0 || (size_t)n >= cap || (size_t)n >= kMaxRef) {
        out[0] = '\0';
        return false;
    }
    return true;
}

const char* relPart(const char* ref)
{
    if (classify(ref) != Kind::Samples)
        return nullptr;
    return ref + strlen(kSamplesPrefix);
}

bool devicePath(char* out, size_t cap, const char* root, const char* rel)
{
    if (!out || !cap || !root || !*root)
        return false;
    int n;
    if (rel && *rel) {
        if (!validRelPath(rel))
            return false;
        n = snprintf(out, cap, "%s/PS2DAW/%s/%s", root, kSampleDir, rel);
    } else {
        n = snprintf(out, cap, "%s/PS2DAW/%s", root, kSampleDir);
    }
    return n > 0 && (size_t)n < cap;
}

FileType classifyFile(const char* name, bool isDir)
{
    if (isDir)
        return FileType::Dir;
    if (str::endsWithNoCase(name, ".wav"))
        return FileType::Wav;
    if (str::endsWithNoCase(name, ".adp"))
        return FileType::Adp;
    return FileType::Unsupported;
}

const char* typeName(FileType t)
{
    switch (t) {
    case FileType::Dir: return "DIR";
    case FileType::Wav: return "WAV";
    case FileType::Adp: return "ADP";
    default: return "---";
    }
}

const char* baseName(const char* rel)
{
    const char* slash = strrchr(rel, '/');
    return slash ? slash + 1 : rel;
}

bool joinRel(char* out, size_t cap, const char* dir, const char* name)
{
    const int n = (dir && *dir) ? snprintf(out, cap, "%s/%s", dir, name) : snprintf(out, cap, "%s", name);
    return n > 0 && (size_t)n < cap;
}

bool parentRel(char* rel)
{
    if (!rel[0])
        return false;
    char* slash = strrchr(rel, '/');
    if (slash)
        *slash = '\0';
    else
        rel[0] = '\0';
    return true;
}

int compareEntries(const char* a, bool dirA, const char* b, bool dirB)
{
    if (dirA != dirB)
        return dirA ? -1 : 1;
    for (;; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z')
            ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z')
            cb = (char)(cb - 32);
        if (ca != cb)
            return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
        if (!ca)
            return 0;
    }
}

} // namespace sampleref
