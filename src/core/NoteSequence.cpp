#include "NoteSequence.h"

#include <algorithm>
#include <cmath>

namespace realtime_midi
{
void NoteSequence::prepare(double sampleRate) noexcept
{
    period = std::max<std::int64_t>(2, std::llround(sampleRate));
    held = std::clamp<std::int64_t>(std::llround(sampleRate * 0.5), 1, period - 1);
    reset();
}

void NoteSequence::reset() noexcept
{
    pendingOff |= deliveredOn;
    deliveredOn = false;
    inGate = false;
    position = 0;
    nextEvent = 0;
}
}
