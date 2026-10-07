// In-memory sample storage with a PS2-safe lifecycle.
//
// Threading contract:
//  * The UI thread adds, requests release of, and reaps slots.
//  * The audio thread only reads Ready slots (get()) and acknowledges
//    releases (acknowledgeRelease()). It never allocates or frees.
//
// A Ready sample is immutable: its fields are written completely, then the
// state is published with release semantics.
//
// Freeing an imported sample is a three-step handshake so memory is never
// released while a voice may still read it:
//   1. UI: requestRelease(slot)   Ready -> Releasing. get() now returns null,
//      so no new voice can start on it.
//   2. UI posts Command::ReleaseSample; the audio thread, between blocks,
//      kills every voice reading the slot and calls acknowledgeRelease().
//   3. UI: reap() frees the PCM of every acknowledged slot.
// Built-in samples are never released.
#pragma once

#include <stdint.h>

#include "audio/audio_config.hpp"

enum class SlotState : uint8_t { Empty = 0, Ready = 1, Releasing = 2, Acked = 3 };

struct Sample {
    char name[16];
    char ref[64];         // stable storage reference: "builtin:KICK" / "samples:DRUMS/KICK.WAV"
    const int16_t* data;  // interleaved when channels == 2; null for SPU2-only (.adp) samples
    uint32_t frames;
    uint32_t sampleRate;
    uint32_t pcmBytes;    // bytes of `data` owned by the bank
    uint32_t fileBytes;   // size of the source file (0 for generated sounds)
    uint16_t generation;  // bumps every time the slot is reused
    uint8_t channels;     // 1 or 2 (0 for SPU2-only)
    uint8_t builtin;      // generated at boot, not loaded from storage
    uint8_t hwOnly;       // lives only in SPU2 RAM (.adp): no software path
    volatile uint8_t state;
};

class SampleBank {
public:
    // PCM held by imported samples is bounded; built-ins are exempt.
    static constexpr uint32_t kMaxExternalBytes = 12u * 1024u * 1024u;

    SampleBank();
    ~SampleBank();
    SampleBank(const SampleBank&) = delete;
    SampleBank& operator=(const SampleBank&) = delete;

    // UI thread. Takes ownership of `data` (malloc/memalign) only on success.
    // `ref` defaults to "builtin:<name>". Returns the slot or -1 when there
    // is no free slot, the data is invalid, or an imported sample would
    // exceed kMaxExternalBytes (see lastFailure()).
    int add(const char* name, int16_t* data, uint32_t frames, uint32_t rate, uint8_t channels, bool builtin,
            const char* ref = nullptr, uint32_t fileBytes = 0);
    // SPU2-only sample (no PCM in EE RAM).
    int addHwOnly(const char* name, const char* ref, uint32_t fileBytes);

    enum class Failure : uint8_t { None, SlotsFull, BudgetFull, Invalid };
    Failure lastFailure() const { return failure_; }
    bool canFit(uint32_t pcmBytes) const { return externalBytes_ + pcmBytes <= kMaxExternalBytes; }
    bool hasFreeSlot() const;

    // Any thread: the sample if the slot is Ready, else null.
    const Sample* get(int index) const;
    // Any thread: the slot's storage regardless of state (pointer identity
    // only; used by the audio thread to find voices of a releasing sample).
    const Sample* peek(int index) const;
    int findByName(const char* name) const;
    int findByRef(const char* ref) const;

    // Highest used slot + 1. Slots below it may be empty; always use get().
    int count() const { return highWater_; }
    int liveCount() const;
    uint32_t bytesUsed() const { return totalBytes_; }       // all PCM
    uint32_t externalBytesUsed() const { return externalBytes_; }

    // UI thread. Ready non-builtin slot -> Releasing. False if not allowed.
    bool requestRelease(int index);
    // Audio thread (or the UI thread when no audio thread runs).
    void acknowledgeRelease(int index) const;
    // UI thread. Frees acknowledged slots; returns how many.
    int reap();
    // UI thread. Immediately frees a slot that was added and never published
    // to the engine (e.g. SPU2 upload failed right after add()).
    void discardNew(int index);
    bool isReleasing(int index) const;

private:
    void freeSlot(int index);

    Sample slots_[cfg::kMaxSamples];
    int highWater_;
    uint32_t totalBytes_;
    uint32_t externalBytes_;
    uint16_t nextGeneration_;
    Failure failure_;
};
