#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class RealtimeMidiEditor final : public juce::AudioProcessorEditor
{
public:
    explicit RealtimeMidiEditor(juce::AudioProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::Label title, versionLabel, versionValue, buildLabel, buildValue;
};
