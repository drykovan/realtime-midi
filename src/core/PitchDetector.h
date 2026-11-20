#pragma once

#include <array>
#include <bitset>

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

    static constexpr float defaultSensitivity = 50.0f;
    static constexpr float minWindowMs = 30.0f;
    static constexpr float defaultWindowMs = 45.0f;
    static constexpr float maxWindowMs = 90.0f;

    void setControls(float sensitivityPercent, float windowMs) noexcept;
    using NoteSet = std::bitset<128>;
    void setPitchHints(NoteSet fundamental, NoteSet overtones) noexcept
    {
        fundamentalHints = fundamental;
        overtoneHints = overtones;
    }
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    Change push(float input) noexcept;

private:
    void updateWindow() noexcept;
    Change estimate() noexcept;
    struct Harmonics
    {
        double onGrid = 0.0;
        double between = 0.0;
        double brightest = 0.0;
        int overtone = -1;
        int supported = 0;
    };
    Harmonics measureHarmonics(double root, bool countSupport) const noexcept;
    double harmonicPower(double frequency) const noexcept;

    std::array<float, 1536> frame{};
    std::array<float, 1536> tapered{};
    std::array<float, 1536> window{};
    std::array<double, 320> difference{};
    NoteSet fundamentalHints, overtoneHints;
    float attackFloor = 0.00065f, relativeRise = 1.62f;
    float requestedWindowMs = defaultWindowMs;
    double rate = 0.0, recentRoot = 0.0;
    float fast = 0.0f, slow = 0.0f, level = 0.0f, last = 0.0f, sum = 0.0f;
    float fastStep = 0.0f, slowStep = 0.0f, levelStep = 0.0f;
    int stride = 1, count = 0, size = 0, maxLag = 0, collected = 0;
    int cooldown = 0, quiet = 0, age = 0;
    int rootAge = 0;
    bool capturing = false, sounding = false, retried = false;
};
}
