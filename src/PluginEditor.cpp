#include "PluginEditor.h"
#include "DevBuild.h"

namespace
{
const auto background = juce::Colour::fromRGB(23, 25, 28);
const auto panel = juce::Colour::fromRGB(32, 35, 39);
const auto border = juce::Colour::fromRGB(52, 57, 63);
const auto foreground = juce::Colour::fromRGB(238, 241, 242);
const auto secondary = juce::Colour::fromRGB(170, 181, 188);
const auto accent = juce::Colour::fromRGB(155, 217, 195);

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
                                       juce::RangedAudioParameter& attack,
                                       juce::RangedAudioParameter& window)
    : AudioProcessorEditor(processor),
      attackAttachment(attack, attackSlider),
      windowAttachment(window, windowSlider)
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

    setOpaque(true);
    setSize(440, 255);
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
}
