// Versioned, portable .ps2daw project format (see docs/PROJECT_FORMAT.md).
//
// Explicit little-endian fields in tagged chunks; unknown chunks are skipped
// so newer files stay loadable by older builds where possible. Nothing is
// written as a raw struct, so compiler layout never leaks into the file.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "project/project.hpp"

namespace projectio {

constexpr uint16_t kFormatVersion = 2;
constexpr size_t kMaxFileBytes = 64 * 1024;

// Serialises into `buf`; returns bytes written, or 0 if `cap` is too small.
size_t save(const Project& p, uint8_t* buf, size_t cap);

// Parses a file image into `out`. On failure `out` is left as an empty
// project and a reason is written to `err`.
bool load(const uint8_t* data, size_t size, Project& out, char* err, size_t errCap);

uint32_t crc32(const uint8_t* data, size_t size);

} // namespace projectio
