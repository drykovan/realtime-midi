#pragma once

#include <cstdint>

namespace realtime_midi
{
struct NoteEvent
{
    int offset;
    bool noteOn;
    static constexpr int channel = 1;
    static constexpr int pitch = 60;
    static constexpr int velocity = 100;
};

class NoteSequence
{
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    // Call only when there is a non-empty block to carry the cleanup.
    template <typename Sink>
    bool flush(Sink&& send) noexcept
    {
        if (pendingOff)
            pendingOff = !send(NoteEvent{0, false});

        return !pendingOff;
    }

    // A rejected Note Off is retried before another attack can be delivered.
    template <typename Sink>
    void process(int samples, Sink&& send) noexcept
    {
        if (samples <= 0)
            return;

        const auto end = position + samples;
        if (pendingOff)
        {
            if (!flush(send))
                return;

            inGate = false;
            nextEvent = position + 1;
        }

        while (nextEvent < end)
        {
            const bool on = !inGate;
            const int offset = static_cast<int>(nextEvent - position);

            if (on)
                deliveredOn = send(NoteEvent{offset, true});
            else if (deliveredOn)
            {
                pendingOff = !send(NoteEvent{offset, false});
                deliveredOn = false;
            }

            inGate = on;
            nextEvent += on ? held : period - held;
            if (pendingOff)
                break;
        }

        position = end;
    }

private:
    std::int64_t period = 2;
    std::int64_t held = 1;
    std::int64_t position = 0;
    std::int64_t nextEvent = 0;
    bool inGate = false;
    bool deliveredOn = false;
    bool pendingOff = false;
};
}
