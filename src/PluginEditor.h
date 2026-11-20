#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <deque>
#include <optional>
#include <atomic>
#include <cstdint>

class DetectedNotes
{
public:
    struct Event
    {
        std::uint32_t time;
        int pitch;
        std::size_t voice;
        bool on;
    };
    void push(Event event) noexcept;
    std::uint64_t latest() const noexcept { return published.load(std::memory_order_acquire); }
    std::optional<Event> read(std::uint64_t sequence) const noexcept;
    static constexpr std::size_t capacity = 512;

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
        int pitch;
        std::uint32_t start;
        std::optional<std::uint32_t> end;
    };
    const DetectedNotes& detectedNotes;
    std::uint64_t lastRead;
    std::array<std::deque<RecentNote>, 2> recent;
    int hintRefresh = 0;
    juce::Label buildStatus;
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
