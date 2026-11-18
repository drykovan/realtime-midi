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
    capturing = sounding = retried = false;
    recentRoot = 0.0;
    rootAge = 0;
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

    if (rootAge > 0)
        --rootAge;
    if (cooldown > 0)
        --cooldown;
    if (!capturing && cooldown == 0 && fast > std::max(0.00065f, slow * 1.62f))
    {
        collected = 0;
        capturing = true;
        retried = false;
        cooldown = static_cast<int>(rate * 0.09);
    }

    if (capturing)
    {
        frame[collected++] = sample;
        if (collected == size)
        {
            capturing = false;
            const auto change = estimate();
            if (!change.emit && !retried)
            {
                // Give the pitched tail one more look after the noisy attack.
                const int shift = std::max(1, size / 3);
                std::move(frame.begin() + shift, frame.begin() + size, frame.begin());
                collected = size - shift;
                capturing = retried = true;
            }
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

PitchDetector::Harmonics PitchDetector::measureHarmonics(double root, bool countSupport) const noexcept
{
    Harmonics result;
    const int last = std::min(32, static_cast<int>(std::min(3600.0, rate * 0.44) / root));
    std::array<double, 32> power{};
    std::array<double, 33> gap{};
    gap[0] = harmonicPower(root * 0.5);
    for (int h = 1; h <= last; ++h)
    {
        const double hz = h * root;
        const auto index = static_cast<std::size_t>(h - 1);
        const double value = harmonicPower(hz);
        power[index] = value;
        result.onGrid += value;
        result.between += gap[index];
        if (h >= 2 && value > result.brightest)
        {
            result.brightest = value;
            result.overtone = midi(hz);
        }
        if (h < last || countSupport)
            gap[index + 1] = harmonicPower(hz + root * 0.5);
    }
    if (countSupport)
    {
        const auto dominant = *std::max_element(power.begin(), power.end());
        // An occupied bin must stand above its local noise floor on both sides.
        for (int index = 0; index < last; ++index)
            if (power[index] >= dominant * 0.06
                && power[index] > 3.0 * std::max(gap[index], gap[index + 1]))
                ++result.supported;
    }
    return result;
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
    double bestScore = 0.0, bestRoot = 0.0;
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
        const auto harmonics = measureHarmonics(root, false);
        if (harmonics.onGrid < 1e-8)
            continue;
        const double agreement = harmonics.onGrid / (harmonics.onGrid + harmonics.between);
        const double coverage = std::min(1.0, harmonics.onGrid / spectralEnergy);
        const double score = (1.0 - center) * agreement * coverage;
        if (agreement >= 0.68 && coverage >= 0.37 && score > bestScore * 1.015)
        {
            bestScore = score;
            bestRoot = root;
            best = {true, midi(root), harmonics.brightest >= harmonics.onGrid * 0.12
                                          ? harmonics.overtone : -1};
        }
    }
    if (best.emit)
    {
        recentRoot = bestRoot;
        rootAge = static_cast<int>(2.5 * rate);
    }
    else if (rootAge > 0 && spectralEnergy > 1e-8)
    {
        // A weak pluck can retain a harmonic comb even if period confidence
        // dropped. Reuse a recent root only with fresh spectral evidence.
        const auto harmonics = measureHarmonics(recentRoot, true);
        if (harmonics.supported >= 3 && harmonics.onGrid >= spectralEnergy * 0.38
            && harmonics.onGrid >= harmonics.between * 2.8)
        {
            best = {true, midi(recentRoot), harmonics.brightest >= harmonics.onGrid * 0.12
                                               ? harmonics.overtone : -1};
        }
    }
    return best;
}
}
