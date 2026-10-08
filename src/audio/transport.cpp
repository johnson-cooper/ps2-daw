#include "audio/transport.hpp"

Transport::Transport()
    : state_(State::Stopped), bpmCenti_(0), increment_(0), acc_(0), nextStepTick_(0), songFrames_(0), loops_(0)
{
    setBpmCenti(cfg::kDefaultBpmCenti);
}

void Transport::setBpmCenti(uint32_t bpmCenti)
{
    if (bpmCenti < cfg::kMinBpmCenti)
        bpmCenti = cfg::kMinBpmCenti;
    if (bpmCenti > cfg::kMaxBpmCenti)
        bpmCenti = cfg::kMaxBpmCenti;
    bpmCenti_ = bpmCenti;
    increment_ = bpmCenti * (uint32_t)cfg::kPpq;
}

void Transport::setSwingTicks(int ticks)
{
    swing_ = ticks < 0 ? 0 : (ticks > cfg::kTicksPerStep / 2 ? cfg::kTicksPerStep / 2 : ticks);
    // The boundary that is already scheduled moves with it.
    nextStepTick_ = tickOfStep(nextStepTick_ / (uint32_t)cfg::kTicksPerStep);
}

void Transport::play()
{
    if (state_ == State::Stopped) {
        acc_ = 0;
        nextStepTick_ = 0;
        songFrames_ = 0;
        loops_ = 0;
    }
    state_ = State::Playing;
}

void Transport::startAtStep(int step)
{
    if (step < 0)
        step = 0;
    nextStepTick_ = tickOfStep((uint32_t)step);
    acc_ = (uint64_t)nextStepTick_ * kDenominator;
    songFrames_ = 0;
    loops_ = 0;
    state_ = State::Playing;
}

void Transport::pause()
{
    if (state_ == State::Playing)
        state_ = State::Paused;
}

void Transport::stop()
{
    state_ = State::Stopped;
    acc_ = 0;
    nextStepTick_ = 0;
    songFrames_ = 0;
    loops_ = 0;
}

uint32_t Transport::framesUntilNextStep(uint32_t limit) const
{
    const uint64_t target = (uint64_t)nextStepTick_ * kDenominator;
    if (acc_ >= target)
        return 0;
    const uint64_t frames = (target - acc_ + increment_ - 1) / increment_;
    return frames > limit ? limit : (uint32_t)frames;
}

void Transport::advance(uint32_t frames)
{
    acc_ += (uint64_t)increment_ * frames;
    songFrames_ += frames;
}

int Transport::consumeStep(int patternSteps)
{
    int step = (int)(nextStepTick_ / (uint32_t)cfg::kTicksPerStep);
    if (step >= patternSteps) {
        // Loop: re-base so this boundary becomes tick 0. Subtracting the whole
        // boundary keeps the sub-tick remainder, so loops never drift.
        acc_ -= (uint64_t)nextStepTick_ * kDenominator;
        nextStepTick_ = 0;
        step = 0;
        ++loops_;
    }
    nextStepTick_ = tickOfStep((uint32_t)step + 1);
    return step;
}
