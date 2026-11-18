#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void verifyParameterState()
{
    const std::unique_ptr<juce::AudioProcessor> source(createPluginFilter());
    const std::unique_ptr<juce::AudioProcessor> restored(createPluginFilter());
    const auto& parameters = source->getParameters();
    const auto& copies = restored->getParameters();
    require(parameters.size() == 2 && copies.size() == 2, "Expected two host parameters");
    require(parameters[0]->getName(64) == "Attack Sensitivity"
            && parameters[1]->getName(64) == "Analysis Window", "Incorrect host parameter names");
    parameters[0]->setValueNotifyingHost(0.8f);
    parameters[1]->setValueNotifyingHost(1.0f);

    juce::MemoryBlock state;
    source->getStateInformation(state);
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    require(std::abs(copies[0]->getValue() - 0.8f) < 0.001f && copies[1]->getValue() == 1.0f,
            "Parameter state did not survive loading");
}

void verify(int channels, int rate, bool withOvertone)
{
    std::unique_ptr<juce::AudioProcessor> plugin(createPluginFilter());
    const auto layout = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    require(plugin->setBusesLayout({{layout}, {layout}}), "Layout rejected");
    constexpr std::array blocks{63, 256, 511, 128, 997, 4096};
    plugin->prepareToPlay(rate, blocks.back());
    plugin->setNonRealtime(true);

    juce::MidiBuffer midi;
    std::array<int, 2> noteOn{0, 0}, noteOff{0, 0};
    constexpr int pitches[]{41, 81};
    const double root = 440.0 * std::pow(2.0, (41.0 - 69.0) / 12.0);
    int position = 0;
    for (std::size_t block = 0; position < rate * 2; ++block)
    {
        const int size = blocks[block % blocks.size()];
        juce::AudioBuffer<float> audio(channels, size);
        for (int i = 0; i < size; ++i)
        {
            const double time = static_cast<double>(position + i) / rate;
            const double phase = 2.0 * 3.14159265358979323846 * root * (time - 0.25);
            const auto sample = static_cast<float>(time >= 0.25 && time < 0.42
                ? 0.05 * std::sin(phase) + (withOvertone ? 0.17 * std::sin(phase * 10.0) : 0.0)
                : 0.0);
            for (int ch = 0; ch < channels; ++ch)
                audio.setSample(ch, i, sample);
        }

        const auto input = audio;
        plugin->processBlock(audio, midi);
        for (int ch = 0; ch < channels; ++ch)
            require(std::memcmp(audio.getReadPointer(ch), input.getReadPointer(ch),
                                static_cast<std::size_t>(size) * sizeof(float)) == 0, "Audio changed");

        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();
            const int voice = message.getChannel() - 1;
            require(voice >= 0 && voice < 2 && message.getNoteNumber() == pitches[voice],
                    "Unexpected pitch or channel");
            require(metadata.samplePosition >= 0 && metadata.samplePosition < size, "Invalid offset");
            require(position + metadata.samplePosition >= rate / 4, "Note emitted on leading silence");
            if (message.isNoteOn())
                ++noteOn[voice];
            else if (message.isNoteOff())
                ++noteOff[voice];
        }

        position += size;
    }
    require(noteOn[0] == 1 && noteOff[0] == 1, "Expected one root Note On and Off");
    require(noteOn[1] == withOvertone && noteOff[1] == withOvertone,
            "Unexpected overtone Note On or Off");
}
}

int main()
{
    try
    {
        verifyParameterState();
        for (int rate : {44100, 48000, 96000})
            for (int channels : {1, 2})
                for (bool overtone : {false, true})
                    verify(channels, rate, overtone);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
