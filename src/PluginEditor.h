#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class RealtimeMidiEditor final : public juce::AudioProcessorEditor
{
public:
    RealtimeMidiEditor(juce::AudioProcessor&, juce::RangedAudioParameter& attack,
                       juce::RangedAudioParameter& window);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::Label title, versionLabel, versionValue, buildLabel, buildValue;
    juce::Label attackLabel, windowLabel;
    juce::Slider attackSlider, windowSlider;
    juce::SliderParameterAttachment attackAttachment, windowAttachment;
};
