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
    constexpr float kDetectionThreshold = 18.f;

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

    for (auto* button : { &preShowButton, &liveButton, &startButton, &lockAllButton, &resetButton })
    {
        button->setColour (juce::TextButton::buttonColourId, kStripFill);
        button->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        addAndMakeVisible (*button);
    }

    preShowButton.setColour (juce::TextButton::buttonOnColourId, kLatched);
    liveButton.setColour (juce::TextButton::buttonOnColourId, kFloating);
    startButton.setColour (juce::TextButton::buttonOnColourId, kLatched);

    preShowButton.onClick = [this]
    {
        if (proc.apvts.getRawParameterValue ("mode")->load() > 0.5f)
            proc.apvts.getParameter ("mode")->setValueNotifyingHost (0.0f);
    };

    liveButton.onClick = [this]
    {
        if (proc.apvts.getRawParameterValue ("mode")->load() < 0.5f)
            proc.apvts.getParameter ("mode")->setValueNotifyingHost (1.0f);
    };

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

        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }

    return best;
}

void FeedbackDestroyerAudioProcessorEditor::resized()
{
    heatStrip = { 56, 58, 650, 36 };
    strip     = { 56, 104, 650, 190 };

    preShowButton.setBounds (330, 14, 100, 30);
    liveButton.setBounds (434, 14, 76, 30);
    startButton.setBounds (522, 14, 88, 30);
    lockAllButton.setBounds (618, 14, 82, 30);
    resetButton.setBounds (706, 14, 0, 0);
}

void FeedbackDestroyerAudioProcessorEditor::timerCallback()
{
    const bool live = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;
    const bool running = proc.procState.load() == FeedbackDestroyerAudioProcessor::Running;

    preShowButton.setToggleState (! live, juce::dontSendNotification);
    liveButton.setToggleState (live, juce::dontSendNotification);

    startButton.setButtonText (running ? "STOP" : "START");
    startButton.setToggleState (running, juce::dontSendNotification);

    repaint();
}

void FeedbackDestroyerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    g.setColour (juce::Colours::white);
    g.setFont (20.0f);
    g.drawText ("Freq'd Out", 20, 14, 300, 28, juce::Justification::centredLeft);

    g.setColour (kStripFill);
    g.fillRect (heatStrip);

    const float bw = (float) heatStrip.getWidth() / (float) FeedbackDestroyerAudioProcessor::kHeatBins;

    for (int b = 0; b < FeedbackDestroyerAudioProcessor::kHeatBins; ++b)
    {
        const float v = proc.heat[(size_t) b].load();
        const float h = juce::jlimit (0.f, 1.f, v / 40.f) * (float) heatStrip.getHeight();

        g.setColour (v >= kDetectionThreshold + 8.f ? kHot
                                                    : (v >= kDetectionThreshold ? kFloating
                                                                                : kLatched.withAlpha (0.6f)));
        g.fillRect ((float) heatStrip.getX() + (float) b * bw,
                    (float) heatStrip.getBottom() - h,
                    juce::jmax (1.f, bw - 1.f),
                    h);
    }

    const float ty = (float) heatStrip.getBottom()
                   - kDetectionThreshold / 40.f * (float) heatStrip.getHeight();
    const float dashes[] = { 4.f, 3.f };

    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.drawDashedLine (juce::Line<float> ((float) heatStrip.getX(), ty,
                                          (float) heatStrip.getRight(), ty),
                      dashes, 2, 1.f);

    g.setFont (10.0f);
    g.setColour (juce::Colours::grey);
    g.drawText ("Detection", heatStrip.getX() - 54, (int) ty - 6, 50, 12,
                juce::Justification::centredRight);

    g.setColour (kStripFill);
    g.fillRect (strip);

    g.setFont (11.0f);

    for (int db = 0; db <= 24; db += 6)
    {
        const float y = yForDb ((float) db);
        g.setColour (kGrid);
        g.drawHorizontalLine ((int) y, (float) strip.getX(), (float) strip.getRight());
        g.setColour (juce::Colours::grey);
        g.drawText (db == 0 ? "0 dB" : "-" + juce::String (db),
                    8, (int) y - 8, 44, 16, juce::Justification::centredRight);
    }

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
        g.drawText (formatFreq (f), (int) x - 22, (int) y1 + 6, 44, 12,
                    juce::Justification::centred);
    }

    g.setColour (juce::Colours::grey);
    g.setFont (12.0f);
    g.drawText ("Blue: locked   Amber: live adaptive notch   Grey: switched off   White outline: manual cut",
                strip.getX(), strip.getBottom() + 22, strip.getWidth(), 16,
                juce::Justification::centredLeft);
    g.drawText ("Drag a fader to cut deeper or shallower   |   Wheel: Q   |   Double-click: on / off",
                strip.getX(), strip.getBottom() + 40, strip.getWidth(), 16,
                juce::Justification::centredLeft);

    const bool live = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;
    const int ps = proc.procState.load();
    const int secs = (int) proc.elapsedSec.load();

    juce::String status;

    if (live)
        status = proc.procState.load() == FeedbackDestroyerAudioProcessor::Running
                     ? "LIVE  -  one adaptive notch active"
                     : "LIVE  -  press START to arm protection";
    else if (ps == FeedbackDestroyerAudioProcessor::Running)
        status = "PRE-SHOW  -  ringing out  "
               + juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
    else if (ps == FeedbackDestroyerAudioProcessor::Done)
        status = "PRE-SHOW COMPLETE  -  safety notches locked";
    else
        status = "PRE-SHOW  -  press START to ring out";

    g.setColour (juce::Colours::white);
    g.setFont (16.0f);
    g.drawText (status, 56, 395, 650, 22, juce::Justification::centredLeft);

    g.setFont (14.0f);
    g.drawText ("Output gain: " + juce::String (proc.gainDbOut.load(), 1) + " dB     Notches: "
                    + juce::String (active) + " / "
                    + juce::String (FeedbackDestroyerAudioProcessor::kSlots),
                56, 422, 650, 20, juce::Justification::centredLeft);

    if (selected >= 0 && proc.info[(size_t) selected].state.load()
                           != FeedbackDestroyerAudioProcessor::Free)
    {
        g.drawText ("Selected: " + formatFreq (proc.info[(size_t) selected].freq.load()) + " Hz   cut "
                        + juce::String (proc.info[(size_t) selected].depth.load(), 1) + " dB   Q "
                        + juce::String (proc.info[(size_t) selected].q.load(), 1),
                    56, 448, 650, 20, juce::Justification::centredLeft);
    }
}

void FeedbackDestroyerAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    if (strip.expanded (0, 20).contains (e.getPosition()))
        selected = nearestNotch (e.x);
}

void FeedbackDestroyerAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (selected < 0 || proc.info[(size_t) selected].state.load()
                         == FeedbackDestroyerAudioProcessor::Free)
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

void FeedbackDestroyerAudioProcessorEditor::mouseWheelMove (const juce::MouseEvent& e,
                                                            const juce::MouseWheelDetails& wheel)
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
