#include "PluginEditor.h"

void NotchMapComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff15181d));
    g.fillRoundedRectangle (r, 6.f);

    constexpr float fLo = 20.f, fHi = 20000.f;
    auto xFor = [&] (float f)
    {
        const float t = (std::log10 (juce::jlimit (fLo, fHi, f)) - std::log10 (fLo)) / (std::log10 (fHi) - std::log10 (fLo));
        return r.getX() + t * r.getWidth();
    };

    g.setFont (11.f);
    for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
    {
        const float x = xFor (f);
        g.setColour (juce::Colour (0xff2a2f38));
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
        g.setColour (juce::Colour (0xff6b7480));
        g.drawText (f >= 1000.f ? juce::String (f / 1000.f) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> (x + 2.f, r.getBottom() - 16.f, 40.f, 14.f), juce::Justification::left);
    }

    for (int i = 0; i < fd::FeedbackEngine::kMaxBands; ++i)
    {
        float f = 0.f, d = 0.f;
        bool locked = false;
        proc.getBand (i, f, d, locked);
        if (f <= 0.f) continue;

        const float x = xFor (f);
        const float h = juce::jlimit (0.f, 1.f, d / 30.f) * (r.getHeight() - 24.f);
        g.setColour (locked ? juce::Colour (0xff4da3ff) : juce::Colour (0xffff9a3c));
        g.fillRect (juce::Rectangle<float> (x - 1.5f, r.getY() + 4.f, 3.f, h));
        g.setFont (10.f);
        g.drawText (juce::String ((int) std::round (f)), juce::Rectangle<float> (x - 22.f, r.getY() + 6.f + h, 44.f, 12.f),
                    juce::Justification::centred);
    }
}

void FeedbackDestroyerAudioProcessorEditor::addKnob (Knob& k, const juce::String& paramId, const juce::String& text)
{
    addAndMakeVisible (k.slider);
    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.attachToComponent (&k.slider, false);
    addAndMakeVisible (k.label);
    k.att = std::make_unique<SliderAtt> (proc.apvts, paramId, k.slider);
}

FeedbackDestroyerAudioProcessorEditor::FeedbackDestroyerAudioProcessorEditor (FeedbackDestroyerAudioProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p), map (p)
{
    setSize (640, 400);
    addAndMakeVisible (map);

    addKnob (strength,    "strength",    "Strength");
    addKnob (shiftMax,    "shiftMax",    "Max Shift");
    addKnob (sensitivity, "sensitivity", "Sensitivity");
    addKnob (maxDepth,    "maxDepth",    "Max Depth");
    addKnob (q,           "q",           "Notch Q");
    addKnob (release,     "release",     "Release");
    addKnob (bands,       "bands",       "Bands");

    speed.addItemList ({ "Slow", "Medium", "Fast" }, 1);
    linkGroup.addItemList ({ "Off", "1", "2", "3", "4", "5", "6", "7", "8" }, 1);
    role.addItemList ({ "Channel", "Master" }, 1);
    for (auto* c : { &speed, &linkGroup, &role }) addAndMakeVisible (c);

    auto setup = [this] (juce::Label& l, const juce::String& t, juce::Component& c)
    {
        l.setText (t, juce::dontSendNotification);
        l.attachToComponent (&c, true);
        addAndMakeVisible (l);
    };
    setup (speedLabel, "Speed", speed);
    setup (linkLabel,  "Link group", linkGroup);
    setup (roleLabel,  "Role", role);

    speedAtt     = std::make_unique<ComboAtt> (proc.apvts, "speed", speed);
    linkGroupAtt = std::make_unique<ComboAtt> (proc.apvts, "linkGroup", linkGroup);
    roleAtt      = std::make_unique<ComboAtt> (proc.apvts, "role", role);

    addAndMakeVisible (bypass);
    bypassAtt = std::make_unique<ButtonAtt> (proc.apvts, "bypass", bypass);

    addAndMakeVisible (lockButton);
    addAndMakeVisible (clearButton);
    lockButton.onClick  = [this] { proc.requestLock(); };
    clearButton.onClick = [this] { proc.requestClear(); };

    linkStatus.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (linkStatus);

    startTimerHz (4);
    timerCallback();
}

void FeedbackDestroyerAudioProcessorEditor::timerCallback()
{
    const int n = proc.instancesInGroup();
    linkStatus.setText (n > 0 ? juce::String (n) + " instance(s) in this group (same process)" : "Link off",
                        juce::dontSendNotification);
}

void FeedbackDestroyerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1d2128));
}

void FeedbackDestroyerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    map.setBounds (area.removeFromTop (170));
    area.removeFromTop (28); // room for knob labels

    auto knobRow = area.removeFromTop (96);
    const int w = knobRow.getWidth() / 7;
    for (auto* k : { &strength, &shiftMax, &sensitivity, &maxDepth, &q, &release, &bands })
        k->slider.setBounds (knobRow.removeFromLeft (w).reduced (8, 0));

    area.removeFromTop (12);
    auto row1 = area.removeFromTop (28);
    speed.setBounds (row1.removeFromLeft (200).withTrimmedLeft (70));
    row1.removeFromLeft (16);
    bypass.setBounds (row1.removeFromLeft (90));
    row1.removeFromLeft (16);
    lockButton.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (8);
    clearButton.setBounds (row1.removeFromLeft (100));

    area.removeFromTop (10);
    auto row2 = area.removeFromTop (28);
    linkGroup.setBounds (row2.removeFromLeft (170).withTrimmedLeft (90));
    row2.removeFromLeft (12);
    role.setBounds (row2.removeFromLeft (150).withTrimmedLeft (45));
    row2.removeFromLeft (12);
    linkStatus.setBounds (row2);
}
