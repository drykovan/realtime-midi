#include "PluginEditor.h"
#include "DevBuild.h"
#include <algorithm>

void DetectedNotes::push(int fundamental, int overtone) noexcept
{
    const auto sequence = published.load(std::memory_order_relaxed) + 1;
    const auto payload = (sequence << 16)
                       | (static_cast<std::uint64_t>(fundamental + 1) << 8)
                       | static_cast<std::uint64_t>(overtone + 1);
    slots[sequence % capacity].store(payload, std::memory_order_relaxed);
    published.store(sequence, std::memory_order_release);
}

std::array<int, 2> DetectedNotes::read(std::uint64_t sequence) const noexcept
{
    const auto payload = slots[sequence % capacity].load(std::memory_order_relaxed);
    if ((payload >> 16) != sequence)
        return {-1, -1};
    return {static_cast<int>((payload >> 8) & 255) - 1, static_cast<int>(payload & 255) - 1};
}

namespace
{
const auto background = juce::Colour::fromRGB(23, 25, 28);
const auto panel = juce::Colour::fromRGB(32, 35, 39);
const auto border = juce::Colour::fromRGB(52, 57, 63);
const auto foreground = juce::Colour::fromRGB(238, 241, 242);
const auto secondary = juce::Colour::fromRGB(170, 181, 188);
const auto accent = juce::Colour::fromRGB(155, 217, 195);
constexpr double historyMs = 1200.0;
constexpr double holdMs = 350.0;

void styleLabel(juce::Label& label, const juce::String& text, float height,
                juce::Colour colour, bool bold = false)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
    label.setColour(juce::Label::textColourId, colour);
    label.setBorderSize(juce::BorderSize<int>(0));
    label.setMinimumHorizontalScale(1.0f);
    label.setJustificationType(juce::Justification::centredLeft);
}
void styleSlider(juce::Slider& slider, const juce::String& title, const juce::String& suffix)
{
    slider.setTitle(title);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.textFromValueFunction = [suffix](double value)
    {
        return juce::String(juce::roundToInt(value)) + suffix;
    };
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 26);
    slider.setColour(juce::Slider::backgroundColourId, border);
    slider.setColour(juce::Slider::trackColourId, accent);
    slider.setColour(juce::Slider::thumbColourId, foreground);
    slider.setColour(juce::Slider::textBoxTextColourId, foreground);
    slider.setColour(juce::Slider::textBoxOutlineColourId, border);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, panel);
}

}

RealtimeMidiEditor::RealtimeMidiEditor(juce::AudioProcessor& processor,
                                      juce::AudioProcessorValueTreeState& parameters,
                                      ReadHints read, WriteHints write, const DetectedNotes& notes)
    : AudioProcessorEditor(processor),
      detectedNotes(notes), lastRead(notes.latest()),
      attackAttachment(*parameters.getParameter("attackSensitivity"), attackSlider),
      windowAttachment(*parameters.getParameter("analysisWindow"), windowSlider),
      hintsAttachment(parameters, "pitchHints", hintsToggle),
      readHints(std::move(read)), writeHints(std::move(write))
{
    styleLabel(title, "Realtime MIDI", 25.0f, foreground, true);
    styleLabel(versionLabel, "Version", 14.0f, secondary);
    styleLabel(versionValue, JucePlugin_VersionString, 15.0f, foreground);
#if defined(REALTIME_MIDI_DEV_BUILD)
    styleLabel(buildLabel, "Build (UTC)", 14.0f, secondary);
    const auto buildTime = juce::String(REALTIME_MIDI_DEV_BUILD).replaceCharacter('T', ' ').dropLastCharacters(1);
    styleLabel(buildValue, buildTime, 15.0f, accent);
#else
    styleLabel(buildLabel, "Build", 14.0f, secondary);
    styleLabel(buildValue, "Release", 15.0f, foreground);
#endif
    versionValue.setTitle("Plugin version");
    buildValue.setTitle("Plugin build");
    for (auto* label : {&title, &versionLabel, &versionValue, &buildLabel, &buildValue})
        addAndMakeVisible(*label);

    styleLabel(attackLabel, "Attack Sensitivity", 14.0f, secondary);
    styleLabel(windowLabel, "Analysis Window", 14.0f, secondary);
    styleSlider(attackSlider, attackLabel.getText(), " %");
    styleSlider(windowSlider, windowLabel.getText(), " ms");
    addAndMakeVisible(attackLabel);
    addAndMakeVisible(windowLabel);
    addAndMakeVisible(attackSlider);
    addAndMakeVisible(windowSlider);

    styleLabel(fundamentalLabel, "Fundamental", 14.0f, secondary);
    styleLabel(overtoneLabel, "Overtones", 14.0f, secondary);
    styleLabel(hintStatus, "", 13.0f, secondary);
    hintsToggle.setColour(juce::ToggleButton::textColourId, foreground);
    addAndMakeVisible(hintsToggle);
    for (auto* label : {&fundamentalLabel, &overtoneLabel, &hintStatus})
        addAndMakeVisible(*label);
    for (int voice = 0; voice < 2; ++voice)
    {
        auto& field = hintFields[voice];
        field.setTitle(voice == 0 ? "Fundamental pitch hints" : "Overtone pitch hints");
        field.setTextToShowWhenEmpty("No notes selected", secondary);
        field.setColour(juce::TextEditor::backgroundColourId, background);
        field.setColour(juce::TextEditor::textColourId, foreground);
        field.setColour(juce::TextEditor::outlineColourId, border);
        field.setColour(juce::TextEditor::focusedOutlineColourId, accent);
        field.setText(readHints(voice), false);
        auto apply = [this, voice]
        {
            auto& editor = hintFields[voice];
            const bool valid = writeHints(voice, editor.getText());
            hintStatus.setText(valid ? "" : "Enter note names with octaves, separated by commas.",
                               juce::dontSendNotification);
            editor.setText(readHints(voice), false);
        };
        field.onReturnKey = apply;
        field.onFocusLost = apply;
        field.onEscapeKey = [this, voice] { hintFields[voice].setText(readHints(voice), false); };
        addAndMakeVisible(field);
    }
    startTimerHz(30);
    setOpaque(true);
    setSize(440, 522);
}

void RealtimeMidiEditor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto latest = detectedNotes.latest();
    if (latest - lastRead > DetectedNotes::capacity)
        lastRead = latest - DetectedNotes::capacity;
    while (lastRead < latest)
    {
        const auto pitches = detectedNotes.read(++lastRead);
        for (std::size_t voice = 0; voice < recent.size(); ++voice)
            if (pitches[voice] >= 0)
            {
                auto& row = recent[voice];
                std::move_backward(row.begin(), row.end() - 1, row.end());
                row.front() = {pitches[voice], now};
            }
    }
    bool refresh = false;
    for (auto& row : recent)
        for (auto& note : row)
            if (note.pitch >= 0)
            {
                refresh = true;
                if (now - note.seenAt >= historyMs)
                    note.pitch = -1;
            }
    if (refresh)
        repaint(20, 395, getWidth() - 40, 107);

    if (++hintRefresh < 8)
        return;
    hintRefresh = 0;
    for (int voice = 0; voice < 2; ++voice)
        if (!hintFields[voice].hasKeyboardFocus(false))
        {
            const auto saved = readHints(voice);
            if (hintFields[voice].getText() != saved)
                hintFields[voice].setText(saved, false);
        }
}

void RealtimeMidiEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(background);
    const auto card = juce::Rectangle<float>(20.0f, 66.0f, static_cast<float>(getWidth() - 40), 72.0f);
    graphics.setColour(panel);
    graphics.fillRoundedRectangle(card, 9.0f);
    graphics.setColour(border);
    graphics.drawRoundedRectangle(card.reduced(0.5f), 9.0f, 1.0f);
    graphics.drawHorizontalLine(102, card.getX() + 12.0f, card.getRight() - 12.0f);
    const auto controls = card.withY(154.0f).withHeight(81.0f);
    graphics.setColour(panel);
    graphics.fillRoundedRectangle(controls, 9.0f);
    graphics.setColour(border);
    graphics.drawRoundedRectangle(controls.reduced(0.5f), 9.0f, 1.0f);
    const auto hints = card.withY(250.0f).withHeight(131.0f);
    graphics.setColour(panel);
    graphics.fillRoundedRectangle(hints, 9.0f);
    graphics.setColour(border);
    graphics.drawRoundedRectangle(hints.reduced(0.5f), 9.0f, 1.0f);

    const auto history = card.withY(395.0f).withHeight(107.0f);
    graphics.setColour(panel);
    graphics.fillRoundedRectangle(history, 9.0f);
    graphics.setColour(border);
    graphics.drawRoundedRectangle(history.reduced(0.5f), 9.0f, 1.0f);
    graphics.setFont(juce::FontOptions(14.0f));
    graphics.setColour(foreground);
    graphics.drawText("Recent Notes", 34, 402, 120, 24, juce::Justification::centredLeft);
    const auto now = juce::Time::getMillisecondCounterHiRes();
    for (std::size_t voice = 0; voice < recent.size(); ++voice)
    {
        const int y = 430 + 31 * static_cast<int>(voice);
        graphics.setColour(secondary);
        graphics.drawText(voice == 0 ? "Fundamental" : "Overtones",
                          34, y, 110, 24, juce::Justification::centredLeft);
        for (std::size_t i = 0; i < recent[voice].size(); ++i)
        {
            const auto note = recent[voice][i];
            if (note.pitch < 0)
                continue;
            const auto alpha = static_cast<float>(std::clamp((historyMs - (now - note.seenAt))
                                                             / (historyMs - holdMs), 0.0, 1.0));
            const int x = getWidth() - 34 - 29 - 31 * static_cast<int>(i);
            const juce::Rectangle<int> chip(x, y, 29, 24);
            const auto colour = voice == 0 ? accent : juce::Colour::fromRGB(184, 196, 244);
            graphics.setColour(colour.withAlpha(alpha * 0.19f));
            graphics.fillRoundedRectangle(chip.toFloat(), 4.0f);
            graphics.setColour(colour.withAlpha(alpha));
            graphics.drawFittedText(juce::MidiMessage::getMidiNoteName(note.pitch, true, true, 4),
                                    chip.reduced(1, 0), juce::Justification::centred, 1, 0.75f);
        }
    }
}

void RealtimeMidiEditor::resized()
{
    title.setBounds(20, 17, getWidth() - 40, 36);
    versionLabel.setBounds(34, 67, 84, 35);
    versionValue.setBounds(126, 67, getWidth() - 160, 35);
    buildLabel.setBounds(34, 103, 84, 35);
    buildValue.setBounds(126, 103, getWidth() - 160, 35);
    attackLabel.setBounds(34, 156, 122, 38);
    windowLabel.setBounds(34, 195, 122, 38);
    attackSlider.setBounds(158, 156, getWidth() - 192, 38);
    windowSlider.setBounds(158, 195, getWidth() - 192, 38);
    hintsToggle.setBounds(32, 255, 140, 28);
    fundamentalLabel.setBounds(34, 286, 110, 26);
    overtoneLabel.setBounds(34, 320, 110, 26);
    hintFields[0].setBounds(158, 286, getWidth() - 192, 26);
    hintFields[1].setBounds(158, 320, getWidth() - 192, 26);
    hintStatus.setBounds(34, 349, getWidth() - 68, 26);
}
