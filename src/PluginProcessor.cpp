#include "PitchDetector.h"
#include "PluginEditor.h"
#include "DevBuild.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cstdint>

#if JUCE_MAC && defined(REALTIME_MIDI_DEV_BUILD)
#include <unistd.h>
#endif

#if defined(REALTIME_MIDI_ENABLE_IAC)
#include "IacProbe.h"
#endif

namespace
{
constexpr auto attackId = "attackSensitivity";
constexpr auto windowId = "analysisWindow";
constexpr auto hintsId = "pitchHints";
constexpr std::array<const char*, 2> hintKeys{"fundamentalHints", "overtoneHints"};
constexpr std::array<const char*, 12> noteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

class StoredHints
{
public:
    using Notes = realtime_midi::PitchDetector::NoteSet;

    Notes load() const noexcept
    {
        return Notes(low.load()) | (Notes(high.load()) << 64);
    }

    juce::String text() const
    {
        const auto notes = load();
        juce::StringArray names;
        for (int note = 0; note < 128; ++note)
            if (notes[note])
                names.add(juce::String(noteNames[note % 12]) + juce::String(note / 12 - 1));
        return names.joinIntoString(", ");
    }

    bool set(const juce::String& input)
    {
        Notes notes;
        for (auto token : juce::StringArray::fromTokens(input.replace("♯", "#").replace("♭", "b"),
                                                       ", \t\r\n", ""))
        {
            if (token.isEmpty())
                continue;
            constexpr std::array<int, 7> naturalNotes{0, 2, 4, 5, 7, 9, 11};
            const int letter = juce::String("CDEFGAB").indexOfChar(juce::CharacterFunctions::toUpperCase(token[0]));
            if (letter < 0)
                return false;
            const auto symbol = juce::CharacterFunctions::toLowerCase(token[1]);
            const int accidental = symbol == '#' ? 1 : symbol == 'b' ? -1 : 0;
            const auto octaveText = token.substring(accidental == 0 ? 1 : 2);
            const int octave = octaveText.getIntValue();
            if (octave < -2 || octave > 10 || octaveText != juce::String(octave))
                return false;
            const int note = (octave + 1) * 12 + naturalNotes[letter] + accidental;
            if (note < 0 || note >= 128)
                return false;
            notes.set(note);
        }
        low.store((notes & Notes(UINT64_MAX)).to_ullong());
        high.store((notes >> 64).to_ullong());
        return true;
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> low{0}, high{0};
};

juce::AudioProcessorValueTreeState::ParameterLayout makeParameterLayout()
{
    using Detector = realtime_midi::PitchDetector;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{attackId, 1}, "Attack Sensitivity",
            juce::NormalisableRange<float>{0.0f, 100.0f, 1.0f}, Detector::defaultSensitivity,
            juce::AudioParameterFloatAttributes().withLabel("%")),
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{windowId, 1}, "Analysis Window",
            juce::NormalisableRange<float>{Detector::minWindowMs, Detector::maxWindowMs, 1.0f},
            Detector::defaultWindowMs, juce::AudioParameterFloatAttributes().withLabel("ms")),
        std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{hintsId, 1}, "Pitch Hints", false));
    return layout;
}

class RealtimeMIDI final : public juce::AudioProcessor
{
public:
    RealtimeMIDI()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          parameters(*this, nullptr, "RealtimeMIDIParameters", makeParameterLayout())
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
        updateDetectorControls();
        detector.prepare(sampleRate);
#if defined(REALTIME_MIDI_ENABLE_IAC)
        iac.start();
#endif
        // Even with a release and retrigger, this leaves ample space for
        // four MIDI edges per minimum analysis window and transport cleanup.
        const auto minimumWindow = sampleRate * realtime_midi::PitchDetector::minWindowMs / 1000.0;
        const auto bytes = 64 * (2 + static_cast<std::size_t>(maxBlockSize / minimumWindow));
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
    juce::AudioProcessorEditor* createEditor() override
    {
        return new RealtimeMidiEditor(*this, parameters,
            [this](int voice) { return hints[voice].text(); },
            [this](int voice, const juce::String& list) { return hints[voice].set(list); },
            detectedNotes);
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& state) override
    {
        auto snapshot = parameters.copyState();
        for (std::size_t voice = 0; voice < hints.size(); ++voice)
            snapshot.setProperty(hintKeys[voice], hints[voice].text(), nullptr);
        if (auto xml = snapshot.createXml())
            copyXmlToBinary(*xml, state);
    }
    void setStateInformation(const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary(data, size); xml && xml->hasTagName(parameters.state.getType()))
        {
            auto snapshot = juce::ValueTree::fromXml(*xml);
            for (std::size_t voice = 0; voice < hints.size(); ++voice)
                if (!hints[voice].set(snapshot.getProperty(hintKeys[voice]).toString()))
                    hints[voice].set({});
            // Projects saved before Pitch Hints contain no toggle parameter.
            if (!snapshot.getChildWithProperty("id", hintsId).isValid())
            {
                juce::ValueTree toggle{"PARAM"};
                toggle.setProperty("id", hintsId, nullptr);
                toggle.setProperty("value", 0.0f, nullptr);
                snapshot.addChild(toggle, -1, nullptr);
            }
            parameters.replaceState(snapshot);
        }
    }

private:
    void updateDetectorControls()
    {
        detector.setControls(attack->load(std::memory_order_relaxed),
                             window->load(std::memory_order_relaxed));
        const bool enabled = hintsEnabled->load(std::memory_order_relaxed) > 0.5f;
        detector.setPitchHints(enabled ? hints[0].load() : StoredHints::Notes{},
                               enabled ? hints[1].load() : StoredHints::Notes{});
    }

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
        // Input monitoring can feed audio with a stopped transport.
        const bool mirror = !isNonRealtime();
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
            updateDetectorControls();
            const auto* left = audio.getReadPointer(0);
            const auto* right = audio.getNumChannels() > 1 ? audio.getReadPointer(1) : left;
            for (int i = 0; i < samples; ++i)
            {
                const auto change = detector.push(0.5f * (left[i] + right[i]));
                if (!change.emit)
                    continue;

                release(i);
                held = {change.root, change.overtone};
                if (change.root >= 0 || change.overtone >= 0)
                    detectedNotes.push(change.root, change.overtone);
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
    juce::AudioProcessorValueTreeState parameters;
    const std::atomic<float>* attack = parameters.getRawParameterValue(attackId);
    const std::atomic<float>* window = parameters.getRawParameterValue(windowId);
    const std::atomic<float>* hintsEnabled = parameters.getRawParameterValue(hintsId);
    std::array<StoredHints, 2> hints;
    DetectedNotes detectedNotes;
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
