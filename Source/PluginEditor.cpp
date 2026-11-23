#include "PluginEditor.h"

namespace
{
    const juce::Colour kBackground (0xff1b1d21);
    const juce::Colour kStripFill  (0xff23262b);
    const juce::Colour kGrid       (0xff3a3e45);
    const juce::Colour kLatched    (0xff5aa9ff);
    const juce::Colour kFloating   (0xffe0b050);
    const juce::Colour kOff        (0xff7d8189);

    juce::String formatFreq (double f)
    {
        if (f >= 1000.0)
            return juce::String (f / 1000.0, 2) + "k";
        return juce::String ((int) std::round (f));
    }
}

FeedbackDestroyerAudioProcessorEditor::FeedbackDestroyerAudioProcessorEditor (FeedbackDestroyerAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (680, 450);

    for (auto* s : { &threshold, &amount })
    {
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
        s->setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        s->setColour (juce::Slider::rotarySliderFillColourId, kLatched);
        addAndMakeVisible (*s);
    }

    thresholdLabel.setText ("Threshold", juce::dontSendNotification);
    amountLabel.setText ("Amount", juce::dontSendNotification);

    for (auto* l : { &thresholdLabel, &amountLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (*l);
    }

    thresholdLabel.attachToComponent (&threshold, false);
    amountLabel.attachToComponent (&amount, false);

    thresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "threshold", threshold);
    amountAttachment    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "amount", amount);

    ringOut.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible (ringOut);
    ringOutAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "ringout", ringOut);

    addAndMakeVisible (resetButton);
    resetButton.onClick = [this] { proc.uiCommand.store (1); };

    addChildComponent (lockButton);
    addChildComponent (keepButton);
    lockButton.onClick = [this] { proc.uiCommand.store (2); };
    keepButton.onClick = [this] { proc.uiCommand.store (3); };

    startTimerHz (30);
}

FeedbackDestroyerAudioProcessorEditor::~FeedbackDestroyerAudioProcessorEditor()
{
    stopTimer();
}

float FeedbackDestroyerAudioProcessorEditor::xForFreq (double f) const
{
    f = juce::jlimit (80.0, 8000.0, f);
    return (float) strip.getX() + (float) (std::log (f / 80.0) / std::log (100.0)) * (float) strip.getWidth();
}

float FeedbackDestroyerAudioProcessorEditor::yForDb (float db) const
{
    return (float) strip.getY() + juce::jlimit (0.f, 24.f, db) / 24.f * (float) strip.getHeight();
}

void FeedbackDestroyerAudioProcessorEditor::resized()
{
    strip = { 56, 64, 580, 190 };

    threshold.setBounds (70, 305, 110, 110);
    amount.setBounds (210, 305, 110, 110);

    ringOut.setBounds (380, 310, 110, 28);
    resetButton.setBounds (380, 350, 100, 28);

    lockButton.setBounds (440, 410, 100, 28);
    keepButton.setBounds (548, 410, 100, 28);
}

void FeedbackDestroyerAudioProcessorEditor::timerCallback()
{
    const bool pending = proc.pendingSlot.load() >= 0;
    lockButton.setVisible (pending);
    keepButton.setVisible (pending);
    repaint();
}

void FeedbackDestroyerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    g.setColour (juce::Colours::white);
    g.setFont (20.0f);
    g.drawText ("Freq'd Out", 20, 14, 300, 28, juce::Justification::centredLeft);

    // --- strip: dB grid ------------------------------------------------------
    g.setColour (kStripFill);
    g.fillRect (strip);

    g.setFont (11.0f);
    for (int db = 0; db <= 24; db += 6)
    {
        const float y = yForDb ((float) db);
        g.setColour (kGrid);
        g.drawHorizontalLine ((int) y, (float) strip.getX(), (float) strip.getRight());
        g.setColour (juce::Colours::grey);
        g.drawText (db == 0 ? "0 dB" : "-" + juce::String (db), 8, (int) y - 8, 44, 16, juce::Justification::centredRight);
    }

    // --- strip: frequency grid ---------------------------------------------------
    const double marks[] = { 100, 200, 500, 1000, 2000, 5000, 8000 };
    for (double f : marks)
    {
        const float x = xForFreq (f);
        g.setColour (kGrid);
        g.drawVerticalLine ((int) x, (float) strip.getY(), (float) strip.getBottom());
        g.setColour (juce::Colours::grey);
        g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                    (int) x - 20, strip.getBottom() + 3, 40, 16, juce::Justification::centred);
    }

    // --- the notches, drawn as Graphic EQ faders --------------------------------------
    for (int i = 0; i < FeedbackDestroyerAudioProcessor::kSlots; ++i)
    {
        const int state = proc.info[(size_t) i].state.load();
        if (state == FeedbackDestroyerAudioProcessor::Free)
            continue;

        const double f  = proc.info[(size_t) i].freq.load();
        const float cut = proc.info[(size_t) i].depth.load();
        const bool latched = state == FeedbackDestroyerAudioProcessor::Latched;
        const bool engaged = proc.info[(size_t) i].engaged.load();

        const auto colour = ! latched ? kFloating : (engaged ? kLatched : kOff);

        const float x  = xForFreq (f);
        const float y0 = yForDb (0.f);
        const float y1 = yForDb (cut);

        g.setColour (colour.withAlpha (0.35f));
        g.fillRect (x - 3.f, y0, 6.f, juce::jmax (1.f, y1 - y0));

        g.setColour (colour);
        g.fillRoundedRectangle (x - 8.f, y1 - 3.f, 16.f, 7.f, 2.f);

        g.setFont (10.0f);
        g.drawText (formatFreq (f), (int) x - 22, (int) y1 + 6, 44, 12, juce::Justification::centred);
    }

    g.setColour (juce::Colours::grey);
    g.setFont (12.0f);
    g.drawText ("Blue: latched   Amber: floating   Grey: switched off (click a latched fader)",
                strip.getX(), strip.getBottom() + 22, strip.getWidth(), 16, juce::Justification::centredLeft);

    // --- gain readout --------------------------------------------------------------
    const float gain = proc.gainDbOut.load();
    g.setColour (juce::Colours::white);
    g.setFont (15.0f);
    g.drawText ("Output gain: " + juce::String (gain, 1) + " dB", 500, 314, 170, 20, juce::Justification::centredLeft);

    // --- lock prompt ------------------------------------------------------------------
    const int pending = proc.pendingSlot.load();
    if (pending >= 0 && pending < FeedbackDestroyerAudioProcessor::kSlots)
    {
        g.setColour (kLatched.withAlpha (0.18f));
        g.fillRoundedRectangle (20.f, 402.f, 640.f, 44.f, 8.f);
        g.setColour (juce::Colours::white);
        g.setFont (14.0f);
        g.drawText ("Notch at " + formatFreq (proc.info[(size_t) pending].freq.load()) + " is holding. Lock it?",
                    32, 410, 390, 28, juce::Justification::centredLeft);
    }
}

void FeedbackDestroyerAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! strip.expanded (0, 20).contains (e.getPosition()))
        return;

    int best = -1;
    float bestDist = 12.f;

    for (int i = 0; i < FeedbackDestroyerAudioProcessor::kLatched; ++i)
    {
        if (proc.info[(size_t) i].state.load() != FeedbackDestroyerAudioProcessor::Latched)
            continue;

        const float d = std::abs (xForFreq (proc.info[(size_t) i].freq.load()) - (float) e.x);
        if (d < bestDist) { bestDist = d; best = i; }
    }

    if (best >= 0)
        proc.info[(size_t) best].engaged.store (! proc.info[(size_t) best].engaged.load());
}
