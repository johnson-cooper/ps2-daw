// Narrow interfaces between the platform-independent sample code and the
// platform (USB filesystem, SPU2). Host tests implement them with fakes.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Reads files from the sample folder (<root>/PS2DAW/SAMPLES/) of whichever
// USB root has them. All calls run on the UI thread, never the audio thread.
class SampleFileSource {
public:
    virtual ~SampleFileSource() {}
    // Opens `rel` (validated by the caller). Returns a handle >= 0 and the
    // file size, or a negative value if no root has the file.
    virtual int openSample(const char* rel, uint32_t* size) = 0;
    // Reads up to n bytes: count, 0 at end of file, negative on error.
    virtual int read(int handle, uint8_t* buf, uint32_t n) = 0;
    virtual void close(int handle) = 0;
};

struct Sample;

// The SPU2 side of the sample bank. Optional: samples always work in
// software; SPU2 copies are explicit.
class HwSink {
public:
    virtual ~HwSink() {}
    // Encodes a mono PCM sample to PS-ADPCM and uploads it. Fails cleanly
    // (false + reason) for stereo, bad rates or when SPU2 RAM is full.
    virtual bool uploadPcm(int slot, const Sample& s, char* err, size_t cap) = 0;
    // Uploads the image of an APCM (.adp) file.
    virtual bool uploadApcm(int slot, const uint8_t* file, uint32_t size, char* err, size_t cap) = 0;
    // Frees the slot's SPU2 copy, if any.
    virtual void unload(int slot) = 0;
    virtual bool resident(int slot) const = 0;
    virtual uint32_t bytesUsed() const = 0;
};
