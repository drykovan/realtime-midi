#include "NoteSequence.h"
#include "DevBuild.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

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
        sequence.prepare(sampleRate);
#if defined(REALTIME_MIDI_ENABLE_IAC)
        iac.start();
#endif
        // Budget two events per second, plus a boundary event and reset cleanup.
        // JUCE 8 stores each three-byte MIDI message in nine bytes.
        const double half = std::max(1.0, std::floor(sampleRate * 0.5));
        const auto bytes = 16 * (4 + static_cast<std::size_t>(maxBlockSize / half));
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
        sequence.reset();
#if defined(REALTIME_MIDI_ENABLE_IAC)
        if (iacNoteActive)
            iac.enqueue(false);
        iacNoteActive = false;
#endif
    }

    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        // JUCE supplies the matching input and output channels in place.
        renderEvents(audio.getNumSamples(), midi, false);
    }

    void processBlockBypassed(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        reset();
        renderEvents(audio.getNumSamples(), midi, true);
    }

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& state) override { state.reset(); }
    void setStateInformation(const void*, int) override {}

private:
    void renderEvents(int samples, juce::MidiBuffer& midi, bool bypassed)
    {
        if (samples == 0)
        {
            midi.clear();
            return;
        }

        // The VST3 wrapper reuses its MIDI buffer. Park its original backing
        // store, then exchange the two stores reserved during prepareToPlay.
        auto& output = firstBlock ? firstOutput : nextOutput;
        firstBlock = false;
        output.clear();
#if defined(REALTIME_MIDI_ENABLE_IAC)
        const auto* playhead = getPlayHead();
        const auto transport = playhead ? playhead->getPosition() : juce::nullopt;
        const bool mirror = !isNonRealtime() && (!transport || transport->getIsPlaying());
        if (!mirror && iacNoteActive)
        {
            iac.enqueue(false);
            iacNoteActive = false;
        }
        const auto send = [this, &output, mirror](realtime_midi::NoteEvent note)
#else
        const auto send = [&output](realtime_midi::NoteEvent note)
#endif
        {
#if defined(REALTIME_MIDI_ENABLE_IAC)
            if (mirror && iacNoteActive != note.noteOn)
            {
                iac.enqueue(note.noteOn);
                iacNoteActive = note.noteOn;
            }
#endif
            return output.addEvent(note.noteOn
                ? juce::MidiMessage::noteOn(note.channel, note.pitch, juce::uint8{note.velocity})
                : juce::MidiMessage::noteOff(note.channel, note.pitch), note.offset);
        };

        if (bypassed)
            sequence.flush(send);
        else
            sequence.process(samples, send);

        midi.swapWith(output);
    }

#if defined(REALTIME_MIDI_ENABLE_IAC)
    IacProbe iac;
    bool iacNoteActive = false;
#endif
    realtime_midi::NoteSequence sequence;
    juce::MidiBuffer firstOutput, nextOutput;
    bool firstBlock = true;
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RealtimeMIDI;
}
