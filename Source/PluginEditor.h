#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class FeedbackDestroyerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                              private juce::Timer
{
public:
    explicit FeedbackDestroyerAudioProcessorEditor (FeedbackDestroyerAudioProcessor&);
    ~FeedbackDestroyerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
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

    juce::TextButton startButton { "START" };
    juce::TextButton lockAllButton { "Lock all" };
    juce::TextButton resetButton { "Reset" };

    juce::Slider lowLimitSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lowLimitAttach;

    juce::Rectangle<int> strip, heatStrip;
    int selected = -1;
    int hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessorEditor)
};
