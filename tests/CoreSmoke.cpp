#include "NoteSequence.h"
#include "PitchDetector.h"

#include <cmath>
#include <cstdint>
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

void verifyNoisyPluck()
{
    constexpr int rate = 44100;
    constexpr double angular = 2.0 * 3.14159265358979323846 * 87.307;
    realtime_midi::PitchDetector detector;
    detector.prepare(rate);
    detector.setControls(50.0f, 65.0f);
    std::uint32_t random = 42;
    double filteredNoise = 0.0;
    int attacks = 0;
    for (int i = 0; i < rate * 2; ++i)
    {
        const double time = static_cast<double>(i) / rate;
        random = 1664525u * random + 1013904223u;
        const double whiteNoise = (random >> 8) / 8388608.0 - 1.0;
        filteredNoise += 0.075 * (whiteNoise - filteredNoise);
        double sample = 0.0;
        if (time >= 0.10 && time < 0.25)
            sample = 0.06 * std::sin(angular * (time - 0.10));
        if (time >= 0.60 && time < 0.78)
        {
            const double phase = angular * (time - 0.60);
            sample = 0.04 * std::sin(8 * phase) + 0.05 * std::sin(10 * phase)
                   + 0.04 * std::sin(12 * phase) + 0.60 * filteredNoise;
        }

        const auto change = detector.push(static_cast<float>(sample));
        if (change.emit && change.root >= 0)
        {
            ++attacks;
            if (change.root != 41 || (attacks == 2 && change.overtone != 81))
                throw std::runtime_error("Wrong pitch on clean or noisy pluck");
        }
    }
    if (attacks != 2)
        throw std::runtime_error("Expected the noisy pluck to retrigger");
}

int detectSineWithWindow(float ms)
{
    constexpr int rate = 48000;
    constexpr double phaseStep = 2.0 * 3.14159265358979323846 * 220.0 / rate;
    realtime_midi::PitchDetector detector;
    detector.prepare(rate);
    detector.setControls(50.0f, ms);
    for (int i = 0; i < rate / 4; ++i)
    {
        const auto change = detector.push(static_cast<float>(0.15 * std::sin(phaseStep * i)));
        if (change.emit && change.root >= 0)
        {
            if (change.root != 57)
                throw std::runtime_error("Incorrect note with selected analysis window");
            return i;
        }
    }
    throw std::runtime_error("No note with selected analysis window");
}

int main()
{
    try
    {
        for (int rate : {44100, 48000, 96000, 44101})
            verifyTiming(rate);
        verifyReset();
        verifyNoisyPluck();
        if (detectSineWithWindow(90.0f) - detectSineWithWindow(30.0f) != 2880)
            throw std::runtime_error("Analysis window did not change detection latency");
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
