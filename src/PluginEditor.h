#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <atomic>
#include <cstdint>

class DetectedNotes
{
public:
    void push(int fundamental, int overtone) noexcept;
    std::uint64_t latest() const noexcept { return published.load(std::memory_order_acquire); }
    std::array<int, 2> read(std::uint64_t sequence) const noexcept;
    static constexpr std::size_t capacity = 64;

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::array<std::atomic<std::uint64_t>, capacity> slots{};
    std::atomic<std::uint64_t> published{0};
};

class RealtimeMidiEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    using ReadHints = std::function<juce::String(int)>;
    using WriteHints = std::function<bool(int, const juce::String&)>;
    RealtimeMidiEditor(juce::AudioProcessor&, juce::AudioProcessorValueTreeState&,
                       ReadHints, WriteHints, const DetectedNotes&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setHintError(int voice, bool invalid);
    void openNotePicker(int voice);
    struct RecentNote
    {
        int pitch = -1;
        double seenAt = 0.0;
    };
    const DetectedNotes& detectedNotes;
    std::uint64_t lastRead;
    std::array<std::array<RecentNote, 8>, 2> recent{};
    int hintRefresh = 0;
    juce::Label title, versionLabel, versionValue, buildLabel, buildValue;
    juce::Label attackLabel, windowLabel;
    juce::Slider attackSlider, windowSlider;
    juce::SliderParameterAttachment attackAttachment, windowAttachment;
    juce::ToggleButton hintsToggle{"Pitch Hints"};
    juce::AudioProcessorValueTreeState::ButtonAttachment hintsAttachment;
    juce::Label fundamentalLabel, overtoneLabel, hintStatus;
    std::array<juce::TextEditor, 2> hintFields;
    juce::DrawableButton fundamentalPicker{"Pick fundamental notes", juce::DrawableButton::ImageFitted};
    juce::DrawableButton overtonePicker{"Pick overtone notes", juce::DrawableButton::ImageFitted};
    juce::TooltipWindow pickerTooltips{this};
    std::array<juce::String, 2> confirmedHints;
    std::array<bool, 2> hintErrors{};
    ReadHints readHints;
    WriteHints writeHints;
};
