#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace realtime_midi
{
namespace
{
constexpr double pi = 3.14159265358979323846;

int midi(double frequency)
{
    return std::clamp(static_cast<int>(std::lround(69.0 + 12.0 * std::log2(frequency / 440.0))), 0, 127);
}
}

void PitchDetector::prepare(double sampleRate) noexcept
{
    stride = std::max(1, static_cast<int>(std::lround(sampleRate / 10000.0)));
    rate = sampleRate / stride;
    size = std::clamp(static_cast<int>(rate * 0.065), 1, static_cast<int>(frame.size()));
    maxLag = std::min(static_cast<int>(rate / 45.0), static_cast<int>(difference.size()) - 1);
    fastStep = static_cast<float>(1.0 - std::exp(-1.0 / (rate * 0.002)));
    slowStep = static_cast<float>(1.0 - std::exp(-1.0 / (rate * 0.036)));
    levelStep = static_cast<float>(1.0 - std::exp(-1.0 / (rate * 0.018)));
    for (int i = 0; i < size; ++i)
        window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / size));
    reset();
}

void PitchDetector::reset() noexcept
{
    fast = slow = level = last = sum = 0.0f;
    count = collected = cooldown = quiet = age = 0;
    capturing = sounding = false;
}

PitchDetector::Change PitchDetector::push(float input) noexcept
{
    if (rate <= 0.0)
        return {};

    sum += input;
    if (++count < stride)
        return {};

    const float sample = sum / stride;
    sum = 0.0f;
    count = 0;
    const float edge = std::abs(sample - last);
    last = sample;
    fast += fastStep * (edge - fast);
    slow += slowStep * (edge - slow);
    level += levelStep * (std::abs(sample) - level);

    if (cooldown > 0)
        --cooldown;
    if (!capturing && cooldown == 0 && fast > std::max(0.00065f, slow * 1.62f))
    {
        collected = 0;
        capturing = true;
        cooldown = static_cast<int>(rate * 0.09);
    }

    if (capturing)
    {
        frame[collected++] = sample;
        if (collected == size)
        {
            capturing = false;
            auto change = estimate();
            if (change.emit)
            {
                sounding = true;
                quiet = age = 0;
                return change;
            }
        }
    }

    if (sounding)
    {
        quiet = level < 0.0014f ? quiet + 1 : 0;
        if (++age >= rate * 0.45 || quiet >= rate * 0.045)
        {
            sounding = false;
            return {true, -1, -1};
        }
    }
    return {};
}

double PitchDetector::harmonicPower(double frequency) const noexcept
{
    const double coefficient = 2.0 * std::cos(2.0 * pi * frequency / rate);
    double previous = 0.0, before = 0.0;
    for (int i = 0; i < size; ++i)
    {
        const double next = tapered[i] + coefficient * previous - before;
        before = previous;
        previous = next;
    }
    return std::max(0.0, previous * previous + before * before - coefficient * previous * before);
}

PitchDetector::Change PitchDetector::estimate() noexcept
{
    const int support = size - maxLag;
    if (support <= 0)
        return {};

    double cumulative = 0.0;
    difference[0] = 1.0;
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double energy = 0.0;
        for (int i = 0; i < support; ++i)
        {
            const double delta = frame[i] - frame[i + lag];
            energy += delta * delta;
        }
        cumulative += energy;
        difference[lag] = cumulative > 0.0 ? energy * lag / cumulative : 1.0;
    }

    double spectralEnergy = 0.0;
    for (int i = 0; i < size; ++i)
    {
        tapered[i] = frame[i] * window[i];
        spectralEnergy += tapered[i] * tapered[i];
    }
    spectralEnergy *= size / 3.0;

    const int minLag = std::max(2, static_cast<int>(rate / 1000.0));
    double bestScore = 0.0;
    Change best{};
    // Check multiple plausible periods. Comparing on-grid energy with the
    // halfway points helps reject octaves when odd harmonics are present.
    for (int lag = minLag; lag < maxLag; ++lag)
    {
        const double center = difference[lag];
        const double left = difference[lag - 1], right = difference[lag + 1];
        if (center >= 0.28 || center > left || center >= right)
            continue;
        const double period = lag + 0.5 * (left - right) / (left + right - 2.0 * center);
        const double root = rate / period;
        const int lastHarmonic = std::min(32, static_cast<int>(std::min(3600.0, rate * 0.44) / root));
        double onGrid = 0.0, between = 0.0, brightest = 0.0;
        int overtone = -1;
        for (int h = 1; h <= lastHarmonic; ++h)
        {
            const double hz = h * root;
            const double power = harmonicPower(hz);
            onGrid += power;
            between += harmonicPower((h - 0.5) * root);
            if (h >= 2 && power > brightest)
            {
                brightest = power;
                overtone = midi(hz);
            }
        }
        if (onGrid < 1e-8)
            continue;
        const double agreement = onGrid / (onGrid + between);
        const double coverage = std::min(1.0, onGrid / spectralEnergy);
        const double score = (1.0 - center) * agreement * coverage;
        if (agreement >= 0.68 && coverage >= 0.37 && score > bestScore * 1.015)
        {
            bestScore = score;
            best = {true, midi(root), brightest >= onGrid * 0.12 ? overtone : -1};
        }
    }
    return best;
}
}
