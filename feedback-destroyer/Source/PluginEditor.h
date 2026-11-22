#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// Graphic EQ style display: every notch is a vertical fader on a log frequency
// axis (80 Hz to 8 kHz) that drops down from 0 dB to its current cut depth.
// The faders move by themselves; click a latched fader to switch it on or off.
class FeedbackDestroyerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                              private juce::Timer
{
public:
    explicit FeedbackDestroyerAudioProcessorEditor (FeedbackDestroyerAudioProcessor&);
    ~FeedbackDestroyerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    float xForFreq (double freq) const;
    float yForDb (float db) const;

    FeedbackDestroyerAudioProcessor& proc;

    juce::Slider threshold, amount;
    juce::Label thresholdLabel, amountLabel;
    juce::ToggleButton ringOut { "Ring-out" };
    juce::TextButton resetButton { "Reset" }, lockButton { "Lock notch" }, keepButton { "Keep floating" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thresholdAttachment, amountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ringOutAttachment;

    juce::Rectangle<int> strip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessorEditor)
};
