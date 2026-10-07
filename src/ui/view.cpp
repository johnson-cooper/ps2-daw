#include "ui/view.hpp"

#include <stdarg.h>
#include <stdio.h>

void UiContext::toast(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    messageUntilMs = nowMs + 3000;
}

uint32_t UiContext::heardFrame() const
{
    return audio.stats().streaming ? audio.stats().heardFrame : engine.status().renderedFrames;
}

int UiContext::heardStep() const
{
    const EngineStatus& st = engine.status();
    if (st.transport == (uint8_t)Transport::State::Stopped)
        return -1;
    const uint32_t heard = heardFrame();
    const uint32_t serial = __atomic_load_n(&st.markSerial, __ATOMIC_ACQUIRE);
    // Newest mark that has actually reached the speakers.
    for (uint32_t i = 0; i < EngineStatus::kMarks && i < serial; ++i) {
        const StepMark& m = st.marks[(serial - 1 - i) % EngineStatus::kMarks];
        if ((int32_t)(heard - m.frame) >= 0)
            return m.step;
    }
    return -1;
}

int UiContext::heardSongStep() const
{
    const EngineStatus& st = engine.status();
    if (st.transport == (uint8_t)Transport::State::Stopped || !st.songMode)
        return -1;
    const uint32_t heard = heardFrame();
    const uint32_t serial = __atomic_load_n(&st.markSerial, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < EngineStatus::kMarks && i < serial; ++i) {
        const StepMark& m = st.marks[(serial - 1 - i) % EngineStatus::kMarks];
        if ((int32_t)(heard - m.frame) >= 0)
            return (int)m.songStep;
    }
    return -1;
}

bool UiContext::channelActive(int ch) const
{
    const EngineStatus& st = engine.status();
    if (ch < 0 || ch >= cfg::kMaxChannels || st.triggerCount[ch] == 0)
        return false;
    const int32_t age = (int32_t)(heardFrame() - st.lastTriggerFrame[ch]);
    return age >= 0 && age < 4320; // 90 ms at 48 kHz
}
