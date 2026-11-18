#include "PitchDetector.h"
#include "PluginEditor.h"
#include "DevBuild.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>

#if JUCE_MAC && defined(REALTIME_MIDI_DEV_BUILD)
#include <unistd.h>
#endif

#if defined(REALTIME_MIDI_ENABLE_IAC)
#include "IacProbe.h"
#endif

namespace
{
class RealtimeMIDI final : public juce::AudioProcessor
{
public:
    RealtimeMIDI()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
#if JUCE_MAC && defined(REALTIME_MIDI_DEV_BUILD)
        const auto process = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFileName();
        const auto log = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                             .getChildFile("Library/Logs/Realtime MIDI Dev.log");
        log.appendText(juce::Time::getCurrentTime().toISO8601(true) + " build "
                       + REALTIME_MIDI_DEV_BUILD + " pid " + juce::String(static_cast<int>(getpid()))
                       + " " + process + "\n");
#endif
    }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }

    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        const auto input = layout.getMainInputChannelSet();
        return input == layout.getMainOutputChannelSet()
            && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
    }

    void prepareToPlay(double sampleRate, int maxBlockSize) override
    {
        reset();
        detector.prepare(sampleRate);
#if defined(REALTIME_MIDI_ENABLE_IAC)
        iac.start();
#endif
        // Even with a release and retrigger, this leaves ample space for
        // four MIDI edges per 65 ms analysis window and transport cleanup.
        const auto bytes = 64 * (2 + static_cast<std::size_t>(maxBlockSize / (sampleRate * 0.060)));
        firstOutput.ensureSize(std::max<std::size_t>(2048, bytes));
        nextOutput.ensureSize(std::max<std::size_t>(2048, bytes));
        firstBlock = true;
    }

    void releaseResources() override
    {
        reset();
#if defined(REALTIME_MIDI_ENABLE_IAC)
        iac.stop();
#endif
    }
    void reset() override
    {
        detector.reset();
        pendingOff = true;
#if defined(REALTIME_MIDI_ENABLE_IAC)
        releaseIac();
#endif
    }

    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        // JUCE supplies the matching input and output channels in place.
        renderEvents(audio, midi, false);
    }

    void processBlockBypassed(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        reset();
        renderEvents(audio, midi, true);
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new RealtimeMidiEditor(*this); }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& state) override { state.reset(); }
    void setStateInformation(const void*, int) override {}

private:
#if defined(REALTIME_MIDI_ENABLE_IAC)
    void releaseIac()
    {
        for (std::size_t voice = 0; voice < iacHeld.size(); ++voice)
        {
            if (iacHeld[voice] >= 0)
                iac.enqueue(static_cast<int>(voice) + 1, iacHeld[voice], false);
            iacHeld[voice] = -1;
        }
    }
#endif

    void renderEvents(const juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi, bool bypassed)
    {
        const int samples = audio.getNumSamples();
        if (samples == 0)
        {
            midi.clear();
            return;
        }

        // Park the wrapper's initial MIDI buffer and exchange two stores
        // reserved during prepareToPlay for the rest of the session.
        auto& output = firstBlock ? firstOutput : nextOutput;
        firstBlock = false;
        output.clear();
#if defined(REALTIME_MIDI_ENABLE_IAC)
        const auto* playhead = getPlayHead();
        const auto transport = playhead ? playhead->getPosition() : juce::nullopt;
        const bool mirror = !isNonRealtime() && (!transport || transport->getIsPlaying());
        if (!mirror)
            releaseIac();
#endif
        const auto send = [&](std::size_t voice, int pitch, bool on, int offset)
        {
            const int channel = static_cast<int>(voice) + 1;
            output.addEvent(on ? juce::MidiMessage::noteOn(channel, pitch, juce::uint8{100})
                               : juce::MidiMessage::noteOff(channel, pitch), offset);
#if defined(REALTIME_MIDI_ENABLE_IAC)
            if (mirror && (on || iacHeld[voice] == pitch))
            {
                iac.enqueue(channel, pitch, on);
                iacHeld[voice] = on ? pitch : -1;
            }
#endif
        };
        const auto release = [&](int offset)
        {
            for (std::size_t voice = 0; voice < held.size(); ++voice)
            {
                if (held[voice] >= 0)
                    send(voice, held[voice], false, offset);
                held[voice] = -1;
            }
        };

        if (pendingOff)
        {
            release(0);
            pendingOff = false;
        }

        if (!bypassed && audio.getNumChannels() > 0)
        {
            const auto* left = audio.getReadPointer(0);
            const auto* right = audio.getNumChannels() > 1 ? audio.getReadPointer(1) : left;
            for (int i = 0; i < samples; ++i)
            {
                const auto change = detector.push(0.5f * (left[i] + right[i]));
                if (!change.emit)
                    continue;

                release(i);
                held = {change.root, change.overtone};
                for (std::size_t voice = 0; voice < held.size(); ++voice)
                    if (held[voice] >= 0)
                        send(voice, held[voice], true, i);
            }
        }

        midi.swapWith(output);
    }

#if defined(REALTIME_MIDI_ENABLE_IAC)
    IacProbe iac;
    std::array<int, 2> iacHeld{-1, -1};
#endif
    realtime_midi::PitchDetector detector;
    std::array<int, 2> held{-1, -1};
    bool pendingOff = false;
    juce::MidiBuffer firstOutput, nextOutput;
    bool firstBlock = true;
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RealtimeMIDI;
}
