#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Log-frequency map of the 24 notches: bar height = cut depth, blue = locked, orange = dynamic.
class NotchMapComponent : public juce::Component, private juce::Timer
{
public:
    explicit NotchMapComponent (FeedbackDestroyerAudioProcessor& p) : proc (p) { startTimerHz (30); }
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override { repaint(); }
    FeedbackDestroyerAudioProcessor& proc;
};

class FeedbackDestroyerAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit FeedbackDestroyerAudioProcessorEditor (FeedbackDestroyerAudioProcessor&);
    ~FeedbackDestroyerAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<SliderAtt> att;
    };

    void addKnob (Knob& k, const juce::String& paramId, const juce::String& text);

    FeedbackDestroyerAudioProcessor& proc;
    NotchMapComponent map;

    Knob strength, shiftMax, sensitivity, maxDepth, q, release, bands;

    juce::ComboBox speed, linkGroup, role;
    juce::Label speedLabel, linkLabel, roleLabel, linkStatus;
    std::unique_ptr<ComboAtt> speedAtt, linkGroupAtt, roleAtt;

    juce::ToggleButton bypass { "Bypass" };
    std::unique_ptr<ButtonAtt> bypassAtt;
    juce::TextButton lockButton { "Lock current" }, clearButton { "Clear all" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessorEditor)
};
