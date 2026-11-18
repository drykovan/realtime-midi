#pragma once

#include <array>

namespace realtime_midi
{
class PitchDetector
{
public:
    struct Change
    {
        bool emit = false;
        int root = -1;
        int overtone = -1;
    };

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    Change push(float input) noexcept;

private:
    Change estimate() noexcept;
    double harmonicPower(double frequency) const noexcept;

    std::array<float, 1024> frame{};
    std::array<float, 1024> tapered{};
    std::array<float, 1024> window{};
    std::array<double, 320> difference{};
    double rate = 0.0;
    float fast = 0.0f, slow = 0.0f, level = 0.0f, last = 0.0f, sum = 0.0f;
    float fastStep = 0.0f, slowStep = 0.0f, levelStep = 0.0f;
    int stride = 1, count = 0, size = 0, maxLag = 0, collected = 0;
    int cooldown = 0, quiet = 0, age = 0;
    bool capturing = false, sounding = false;
};
}
