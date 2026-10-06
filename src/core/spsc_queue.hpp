// Fixed-capacity single-producer / single-consumer ring buffer.
//
// Used to pass commands from the UI thread to the audio thread without locks
// or allocation. The EE is a single core, so the only hazards are compiler
// reordering and preemption between the payload write and the index publish;
// acquire/release atomics on the 32-bit indices cover both. (The R5900 has no
// LL/SC, so this deliberately uses plain atomic loads/stores and never a
// read-modify-write atomic.)
#pragma once

#include <stdint.h>

template <typename T, uint32_t CapacityPow2>
class SpscQueue {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacity must be a power of two");

public:
    SpscQueue() : head_(0), tail_(0) {}

    // Producer side. Returns false (and drops nothing) when full.
    bool push(const T& item)
    {
        const uint32_t head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        if (head - tail >= CapacityPow2)
            return false;
        items_[head & (CapacityPow2 - 1)] = item;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out)
    {
        const uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const uint32_t head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (head == tail)
            return false;
        out = items_[tail & (CapacityPow2 - 1)];
        __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Approximate fill level; safe to call from either side for statistics.
    uint32_t size() const
    {
        return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    }

    static constexpr uint32_t capacity() { return CapacityPow2; }

private:
    T items_[CapacityPow2];
    uint32_t head_; // written by producer only
    uint32_t tail_; // written by consumer only
};
