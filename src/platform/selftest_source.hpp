// Layered sample file source: names under "__SELFTEST/" are synthesized in
// RAM (so the whole load path can be exercised without a USB stick, e.g. in
// an emulator); everything else goes to the real storage.
#pragma once

#include "audio/sample_io.hpp"

class SelfTestSource : public SampleFileSource {
public:
    static constexpr const char* kPrefix = "__SELFTEST/";

    explicit SelfTestSource(SampleFileSource& real) : real_(real) {}

    int openSample(const char* rel, uint32_t* size) override;
    int read(int handle, uint8_t* buf, uint32_t n) override;
    void close(int handle) override;

private:
    static constexpr int kMaxHandles = 4;
    static constexpr int kBase = 1000; // handles >= kBase are ours

    struct Item {
        uint8_t* data = nullptr;
        uint32_t size = 0, pos = 0;
    };
    SampleFileSource& real_;
    Item items_[kMaxHandles];
};
