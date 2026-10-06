// Small bounded string helpers. Everything here truncates rather than
// overflowing, so it is safe to use on names read from user files.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace str {

// Copies at most cap-1 characters and always NUL-terminates (cap > 0).
void copy(char* dst, size_t cap, const char* src);

// Case-insensitive ASCII comparison; returns true if equal.
bool equalsNoCase(const char* a, const char* b);

// True if `s` ends with `suffix`, ignoring ASCII case.
bool endsWithNoCase(const char* s, const char* suffix);

// Bounded append; keeps dst NUL-terminated.
void append(char* dst, size_t cap, const char* src);

} // namespace str
