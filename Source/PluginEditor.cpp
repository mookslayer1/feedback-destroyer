#include "PluginEditor.h"

namespace
{
    const juce::Colour kBackground (0xff1b1d21);
    const juce::Colour kStripFill  (0xff23262b);
    const juce::Colour kGrid       (0xff3a3e45);
    const juce::Colour kLatched    (0xff5aa9ff);
    const juce::Colour kFloating   (0xffe0b050);
    const juce::Colour kOff        (0xff7d8189);
    const juce::Colour kHot        (0xffff5a36);

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
    setSize (760, 530);

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

    modeBox.addItem ("Pre-show", 1);
    modeBox.addItem ("Live", 2);
    addAndMakeVisible (modeBox);
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "mode", modeBox);

    addAndMakeVisible (startButton);
    addAndMakeVisible (lockAllButton);
    addAndMakeVisible (resetButton);

    startButton.onClick = [this]
    {
        const bool running = proc.procState.load() == FeedbackDestroyerAudioProcessor::Running;
        proc.uiCommand.store (running ? FeedbackDestroyerAudioProcessor::CmdStop
                                      : FeedbackDestroyerAudioProcessor::CmdStart);
    };
    lockAllButton.onClick = [this] { proc.uiCommand.store (FeedbackDestroyerAudioProcessor::CmdLockAll); };
    resetButton.onClick   = [this] { proc.uiCommand.store (FeedbackDestroyerAudioProcessor::CmdReset); };

    startTimerHz (30);
}

FeedbackDestroyerAudioProcessorEditor::~FeedbackDestroyerAudioProcessorEditor()
{
    stopTimer();
}

float FeedbackDestroyerAudioProcessorEditor::xForFreq (double f) const
{
    const double hi = juce::jmax (1000.0, (double) proc.highLimitHz.load());
    f = juce::jlimit (30.0, hi, f);
    return (float) strip.getX() + (float) (std::log (f / 30.0) / std::log (hi / 30.0)) * (float) strip.getWidth();
}

float FeedbackDestroyerAudioProcessorEditor::yForDb (float db) const
{
    return (float) strip.getY() + juce::jlimit (0.f, 24.f, db) / 24.f * (float) strip.getHeight();
}

int FeedbackDestroyerAudioProcessorEditor::nearestNotch (int x) const
{
    int best = -1;
    float bestDist = 14.f;

    for (int i = 0; i < FeedbackDestroyerAudioProcessor::kSlots; ++i)
    {
        if (proc.info[(size_t) i].state.load() == FeedbackDestroyerAudioProcessor::Free)
            continue;

        const float d = std::abs (xForFreq (proc.info[(size_t) i].freq.load()) - (float) x);
        if (d < bestDist) { bestDist = d; best = i; }
    }

    return best;
}

void FeedbackDestroyerAudioProcessorEditor::resized()
{
    heatStrip = { 56, 58, 650, 36 };
    strip     = { 56, 104, 650, 190 };

    modeBox.setBounds (330, 14, 120, 28);
    startButton.setBounds (462, 14, 80, 28);
    lockAllButton.setBounds (552, 14, 80, 28);
    resetButton.setBounds (642, 14, 64, 28);

    threshold.setBounds (70, 385, 110, 110);
    amount.setBounds (210, 385, 110, 110);
}

void FeedbackDestroyerAudioProcessorEditor::timerCallback()
{
    const bool live = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;
    const bool running = proc.procState.load() == FeedbackDestroyerAudioProcessor::Running;

    startButton.setButtonText (running ? "Stop" : "Start");
    startButton.setEnabled (! live);
    repaint();
}

void FeedbackDestroyerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    g.setColour (juce::Colours::white);
    g.setFont (20.0f);
    g.drawText ("Freq'd Out", 20, 14, 300, 28, juce::Justification::centredLeft);

    const float promThr = juce::jmap (proc.apvts.getRawParameterValue ("threshold")->load(), 0.f, 100.f, 8.f, 30.f);
    const float maxDepth = juce::jmap (proc.apvts.getRawParameterValue ("amount")->load(), 0.f, 100.f, 6.f, 24.f);

    // --- heat strip: how feedback-like each part of the spectrum looks ---------------
    g.setColour (kStripFill);
    g.fillRect (heatStrip);

    const float bw = (float) heatStrip.getWidth() / (float) FeedbackDestroyerAudioProcessor::kHeatBins;
    for (int b = 0; b < FeedbackDestroyerAudioProcessor::kHeatBins; ++b)
    {
        const float v = proc.heat[(size_t) b].load();
        const float h = juce::jlimit (0.f, 1.f, v / 40.f) * (float) heatStrip.getHeight();

        g.setColour (v >= promThr + 8.f ? kHot : (v >= promThr ? kFloating : kLatched.withAlpha (0.6f)));
        g.fillRect ((float) heatStrip.getX() + (float) b * bw, (float) heatStrip.getBottom() - h, juce::jmax (1.f, bw - 1.f), h);
    }

    const float ty = (float) heatStrip.getBottom() - promThr / 40.f * (float) heatStrip.getHeight();
    const float dashes[] = { 4.f, 3.f };
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.drawDashedLine (juce::Line<float> ((float) heatStrip.getX(), ty, (float) heatStrip.getRight(), ty), dashes, 2, 1.f);
    g.setFont (10.0f);
    g.setColour (juce::Colours::grey);
    g.drawText ("Threshold", heatStrip.getX() - 54, (int) ty - 6, 50, 12, juce::Justification::centredRight);

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
    const double hi = juce::jmax (1000.0, (double) proc.highLimitHz.load());
    const double marks[] = { 30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 40000 };
    for (double f : marks)
    {
        if (f > hi)
            break;

        const float x = xForFreq (f);
        g.setColour (kGrid);
        g.drawVerticalLine ((int) x, (float) strip.getY(), (float) strip.getBottom());
        g.setColour (juce::Colours::grey);
        g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                    (int) x - 20, strip.getBottom() + 3, 40, 16, juce::Justification::centred);
    }

    // --- Amount: the zone beyond the automatic depth limit -----------------------------
    const float yLimit = yForDb (maxDepth);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect ((float) strip.getX(), yLimit, (float) strip.getWidth(), (float) strip.getBottom() - yLimit);
    g.setColour (kLatched.withAlpha (0.8f));
    g.drawDashedLine (juce::Line<float> ((float) strip.getX(), yLimit, (float) strip.getRight(), yLimit), dashes, 2, 1.f);

    // --- the notches, drawn as Graphic EQ faders --------------------------------------
    int active = 0;
    for (int i = 0; i < FeedbackDestroyerAudioProcessor::kSlots; ++i)
    {
        const int state = proc.info[(size_t) i].state.load();
        if (state == FeedbackDestroyerAudioProcessor::Free)
            continue;

        ++active;

        const double f  = proc.info[(size_t) i].freq.load();
        const float cut = proc.info[(size_t) i].depth.load();
        const bool latched = state == FeedbackDestroyerAudioProcessor::Latched;
        const bool engaged = proc.info[(size_t) i].engaged.load();
        const bool manual  = proc.info[(size_t) i].manual.load();

        const auto colour = ! engaged ? kOff : (latched ? kLatched : kFloating);

        const float x  = xForFreq (f);
        const float y0 = yForDb (0.f);
        const float y1 = yForDb (cut);

        g.setColour (colour.withAlpha (0.35f));
        g.fillRect (x - 3.f, y0, 6.f, juce::jmax (1.f, y1 - y0));

        g.setColour (colour);
        g.fillRoundedRectangle (x - 8.f, y1 - 3.f, 16.f, 7.f, 2.f);

        if (manual)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (x - 9.f, y1 - 4.f, 18.f, 9.f, 2.f, 1.f);
        }

        if (i == selected)
        {
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, (float) strip.getY(), (float) strip.getBottom());
        }

        g.setFont (10.0f);
        g.setColour (colour);
        g.drawText (formatFreq (f), (int) x - 22, (int) y1 + 6, 44, 12, juce::Justification::centred);
    }

    g.setColour (juce::Colours::grey);
    g.setFont (12.0f);
    g.drawText ("Blue: locked   Amber: found / live   Grey: switched off   White outline: manual cut",
                strip.getX(), strip.getBottom() + 22, strip.getWidth(), 16, juce::Justification::centredLeft);
    g.drawText ("Drag a fader to cut deeper or shallower   |   Wheel: Q   |   Double-click: on / off",
                strip.getX(), strip.getBottom() + 40, strip.getWidth(), 16, juce::Justification::centredLeft);

    // --- status ----------------------------------------------------------------------------
    const bool live = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;
    const int ps = proc.procState.load();
    const int secs = (int) proc.elapsedSec.load();

    juce::String status;
    if (live)
        status = "LIVE  -  tracking in quarter-octave notches";
    else if (ps == FeedbackDestroyerAudioProcessor::Running)
        status = "RINGING OUT  " + juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
    else if (ps == FeedbackDestroyerAudioProcessor::Done)
        status = "DONE  -  notches locked";
    else
        status = "PRE-SHOW  -  press Start";

    g.setColour (juce::Colours::white);
    g.setFont (16.0f);
    g.drawText (status, 360, 395, 380, 22, juce::Justification::centredLeft);

    g.setFont (14.0f);
    g.drawText ("Output gain: " + juce::String (proc.gainDbOut.load(), 1) + " dB     Notches: "
                    + juce::String (active) + " / " + juce::String (FeedbackDestroyerAudioProcessor::kSlots),
                360, 422, 380, 20, juce::Justification::centredLeft);

    if (selected >= 0 && proc.info[(size_t) selected].state.load() != FeedbackDestroyerAudioProcessor::Free)
    {
        g.drawText ("Selected: " + formatFreq (proc.info[(size_t) selected].freq.load()) + " Hz   cut "
                        + juce::String (proc.info[(size_t) selected].depth.load(), 1) + " dB   Q "
                        + juce::String (proc.info[(size_t) selected].q.load(), 1),
                    360, 448, 380, 20, juce::Justification::centredLeft);
    }
}

void FeedbackDestroyerAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    if (strip.expanded (0, 20).contains (e.getPosition()))
        selected = nearestNotch (e.x);
}

void FeedbackDestroyerAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (selected < 0 || proc.info[(size_t) selected].state.load() == FeedbackDestroyerAudioProcessor::Free)
        return;

    const float depth = (float) (e.y - strip.getY()) / (float) strip.getHeight() * 24.f;
    proc.info[(size_t) selected].manualDepth.store (juce::jlimit (0.5f, 24.f, depth));
}

void FeedbackDestroyerAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! strip.expanded (0, 20).contains (e.getPosition()))
        return;

    const int i = nearestNotch (e.x);
    if (i >= 0)
        proc.info[(size_t) i].engaged.store (! proc.info[(size_t) i].engaged.load());
}

void FeedbackDestroyerAudioProcessorEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! strip.expanded (0, 20).contains (e.getPosition()))
        return;

    const int i = nearestNotch (e.x);
    if (i < 0)
        return;

    selected = i;
    const float q = proc.info[(size_t) i].q.load();
    const float factor = wheel.deltaY > 0.f ? 1.12f : 1.f / 1.12f;
    proc.info[(size_t) i].manualQ.store (juce::jlimit (1.f, 60.f, q * factor));
}
