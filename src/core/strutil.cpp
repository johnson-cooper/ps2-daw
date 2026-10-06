#include "core/strutil.hpp"

#include <string.h>

namespace str {

void copy(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0)
        return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; ++i)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool equalsNoCase(const char* a, const char* b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (lower(*a) != lower(*b))
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool endsWithNoCase(const char* s, const char* suffix)
{
    if (!s || !suffix)
        return false;
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n)
        return false;
    return equalsNoCase(s + (n - m), suffix);
}

void append(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0)
        return;
    size_t n = strnlen(dst, cap);
    if (n >= cap)
        return;
    copy(dst + n, cap - n, src);
}

} // namespace str
