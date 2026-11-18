#include <juce_audio_processors/juce_audio_processors.h>

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

void verify(int channels, int rate, bool silent)
{
    std::unique_ptr<juce::AudioProcessor> plugin(createPluginFilter());
    const auto layout = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    require(plugin->setBusesLayout({{layout}, {layout}}), "Layout rejected");
    const int held = (rate + 1) / 2;
    const int blocks[]{held, 1, 511, 128, 997, 4096, rate * 2 + 7};
    plugin->prepareToPlay(rate, blocks[6]);

    juce::MidiBuffer midi;
    int position = 0;
    int count = 0;
    for (int block = 0; position < rate * 6; ++block)
    {
        const int size = blocks[block % 7];
        juce::AudioBuffer<float> audio(channels, size);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < size; ++i)
                audio.setSample(ch, i, silent ? 0.0f : static_cast<float>((i + ch * 17) % 101 - 50) / 64.0f);

        const auto input = audio;
        plugin->processBlock(audio, midi);
        for (int ch = 0; ch < channels; ++ch)
            require(std::memcmp(audio.getReadPointer(ch), input.getReadPointer(ch),
                                static_cast<std::size_t>(size) * sizeof(float)) == 0, "Audio changed");

        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();
            const bool on = count % 2 == 0;
            const int expected = (count / 2) * rate + (on ? 0 : held);
            require(position + metadata.samplePosition == expected, "Wrong event sample");
            require(metadata.samplePosition >= 0 && metadata.samplePosition < size, "Invalid offset");
            require(message.getChannel() == 1 && message.getNoteNumber() == 60, "Wrong channel or pitch");
            require(on ? message.isNoteOn() && message.getVelocity() == 100 : message.isNoteOff(),
                    "Wrong event type or velocity");
            ++count;
        }

        position += size;
        require(count == 2 * (position / rate) + (position % rate > 0) + (position % rate > held),
                "Missing or extra event");
    }

    juce::AudioBuffer<float> audio(channels, 2);
    plugin->reset();
    plugin->processBlock(audio, midi);
    plugin->releaseResources();
    plugin->prepareToPlay(rate, 2);
    plugin->processBlock(audio, midi);
    require(midi.getNumEvents() == 2, "Expected cleanup and restart");
    auto event = midi.begin();
    require((*event).samplePosition == 0 && (*event).getMessage().isNoteOff(), "Missing reset Note Off");
    ++event;
    require((*event).samplePosition == 1 && (*event).getMessage().isNoteOn(), "Missing reset Note On");
    plugin->processBlockBypassed(audio, midi);
    require(midi.getNumEvents() == 1 && (*midi.begin()).getMessage().isNoteOff(), "Missing bypass Note Off");
    plugin->processBlockBypassed(audio, midi);
    require(midi.isEmpty(), "Bypass generated another event");
}
}

int main()
{
    try
    {
        for (int rate : {44100, 48000, 96000, 44101})
            for (int channels : {1, 2})
                for (bool silent : {false, true})
                    verify(channels, rate, silent);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
