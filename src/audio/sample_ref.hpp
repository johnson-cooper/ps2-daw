// Stable sample references stored in projects.
//
//   builtin:KICK                 a generated sound
//   samples:DRUMS/KICK.WAV       a file under <root>/PS2DAW/SAMPLES/
//
// External references are relative to the sample folder and never name a
// USB port, so a project keeps working when the stick moves between mass0:
// and mass1:. Every path that reaches the filesystem goes through
// validRelPath(): no "..", no absolute paths, no device prefixes, no control
// characters.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sampleref {

constexpr size_t kMaxRef = 64;                   // matches Sample::ref / ChannelData::sampleRef
constexpr const char* kSamplesPrefix = "samples:";
constexpr const char* kBuiltinPrefix = "builtin:";
constexpr const char* kSampleDir = "SAMPLES";    // under <root>/PS2DAW/

enum class Kind { None, Builtin, Samples, Invalid };
Kind classify(const char* ref);

// True if `rel` is a safe, non-empty relative path using '/' separators.
bool validRelPath(const char* rel);

// "samples:" + rel. Fails (false) if rel is invalid or the result would not
// fit in `cap` (including the NUL).
bool make(char* out, size_t cap, const char* rel);

// Returns the part after "samples:" if `ref` is a valid external reference.
const char* relPart(const char* ref);

// "<root>/PS2DAW/SAMPLES[/rel]". `root` is e.g. "mass0:". rel may be empty.
bool devicePath(char* out, size_t cap, const char* root, const char* rel);

enum class FileType : uint8_t { Dir, Wav, Adp, Unsupported };
FileType classifyFile(const char* name, bool isDir);
const char* typeName(FileType t);

// Last path component of `rel`.
const char* baseName(const char* rel);

// "A/B" + "C" -> "A/B/C"; false on overflow.
bool joinRel(char* out, size_t cap, const char* dir, const char* name);
// Removes the last component ("A/B" -> "A", "A" -> ""). Returns false if
// already empty.
bool parentRel(char* rel);

// Browser ordering: directories first, then case-insensitive by name.
int compareEntries(const char* nameA, bool dirA, const char* nameB, bool dirB);

} // namespace sampleref
