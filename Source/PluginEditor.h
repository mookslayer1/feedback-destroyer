#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// Graphic EQ style display. Every notch is a vertical fader on a log frequency axis
// (30 Hz up to the sample-rate limit, 40 kHz at most) that drops from 0 dB to its cut.
//   - drag a fader up or down to set its depth by hand (this pins it)
//   - turn the mouse wheel over a fader to change its Q
//   - double-click a fader to switch it off or on
// The heat strip above shows how feedback-like each part of the spectrum is, with a
// dashed line at the Threshold. The shaded zone on the faders is beyond Amount.
class FeedbackDestroyerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                              private juce::Timer
{
public:
    explicit FeedbackDestroyerAudioProcessorEditor (FeedbackDestroyerAudioProcessor&);
    ~FeedbackDestroyerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    float xForFreq (double freq) const;
    float yForDb (float db) const;
    int nearestNotch (int x) const;

    FeedbackDestroyerAudioProcessor& proc;

    juce::Slider threshold, amount;
    juce::Label thresholdLabel, amountLabel;
    juce::ComboBox modeBox;
    juce::TextButton startButton { "Start" }, lockAllButton { "Lock all" }, resetButton { "Reset" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> thresholdAttachment, amountAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;

    juce::Rectangle<int> strip, heatStrip;
    int selected = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessorEditor)
};
