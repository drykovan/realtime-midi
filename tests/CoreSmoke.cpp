#include "NoteSequence.h"

#include <iostream>
#include <stdexcept>

using realtime_midi::NoteEvent;
using realtime_midi::NoteSequence;

void require(bool condition)
{
    if (!condition)
        throw std::runtime_error("Unexpected note sequence");
}

void verifyTiming(int rate)
{
    NoteSequence sequence;
    sequence.prepare(rate);
    const int held = (rate + 1) / 2;
    const int blocks[]{held, 1, 511, 997, 128, 2 * rate + 7};
    int position = 0;
    int count = 0;
    for (int block = 0; position < 8 * rate; ++block)
    {
        const int size = blocks[block % 6];
        sequence.process(size, [&](NoteEvent event)
        {
            const bool on = count % 2 == 0;
            const int expected = (count / 2) * rate + (on ? 0 : held);
            require(event.offset >= 0 && event.offset < size);
            require(position + event.offset == expected && event.noteOn == on);
            ++count;
            return true;
        });
        position += size;
        require(count == 2 * (position / rate) + (position % rate > 0) + (position % rate > held));
    }
}

void verifyReset()
{
    NoteSequence sequence;
    sequence.prepare(48000);
    sequence.process(1, [](NoteEvent e) { require(e.noteOn && e.offset == 0); return true; });
    sequence.reset();
    sequence.reset();
    sequence.process(2, [count = 0](NoteEvent e) mutable
    {
        require(e.offset == count && e.noteOn == (count == 1));
        return ++count <= 2;
    });
    sequence.reset();
    require(!sequence.flush([](NoteEvent e) { require(!e.noteOn && e.offset == 0); return false; }));
    require(sequence.flush([](NoteEvent e) { require(!e.noteOn && e.offset == 0); return true; }));
}

int main()
{
    try
    {
        for (int rate : {44100, 48000, 96000, 44101})
            verifyTiming(rate);
        verifyReset();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
