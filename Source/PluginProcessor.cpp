#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout FeedbackDestroyerAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "strength", 1 }, "Strength",
                                                        NormalisableRange<float> (0.f, 1.f, 0.01f), 0.7f,
                                                        AudioParameterFloatAttributes().withStringFromValueFunction (
                                                            [] (float v, int) { return juce::String (juce::roundToInt (v * 100.f)) + " %"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "shiftMax", 1 }, "Max Shift",
                                                        NormalisableRange<float> (0.f, 6.f, 0.1f), 2.f,
                                                        AudioParameterFloatAttributes().withLabel ("Hz")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sensitivity", 1 }, "Sensitivity",
                                                        NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 }, "Speed",
                                                         StringArray { "Slow", "Medium", "Fast" }, 1));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "maxDepth", 1 }, "Max Depth",
                                                        NormalisableRange<float> (6.f, 30.f, 0.5f), 18.f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "q", 1 }, "Notch Q",
                                                        NormalisableRange<float> (10.f, 100.f, 0.1f, 0.5f), 40.f));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "bands", 1 }, "Bands", 1, 24, 24));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release",
                                                        NormalisableRange<float> (2.f, 120.f, 1.f, 0.5f), 30.f,
                                                        AudioParameterFloatAttributes().withLabel ("s")));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "linkGroup", 1 }, "Link Group",
                                                         StringArray { "Off", "1", "2", "3", "4", "5", "6", "7", "8" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "role", 1 }, "Link Role",
                                                         StringArray { "Channel", "Master" }, 0));

    return { p.begin(), p.end() };
}

FeedbackDestroyerAudioProcessor::FeedbackDestroyerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pStrength    = apvts.getRawParameterValue ("strength");
    pShiftMax    = apvts.getRawParameterValue ("shiftMax");
    pSensitivity = apvts.getRawParameterValue ("sensitivity");
    pSpeed       = apvts.getRawParameterValue ("speed");
    pMaxDepth    = apvts.getRawParameterValue ("maxDepth");
    pQ           = apvts.getRawParameterValue ("q");
    pBands       = apvts.getRawParameterValue ("bands");
    pRelease     = apvts.getRawParameterValue ("release");
    pBypass      = apvts.getRawParameterValue ("bypass");
    pLinkGroup   = apvts.getRawParameterValue ("linkGroup");
    pRole        = apvts.getRawParameterValue ("role");

    for (auto& a : pendingFreq)  a.store (0.f);
    for (auto& a : pendingDepth) a.store (0.f);

    apvts.addParameterListener ("linkGroup", this);
    apvts.addParameterListener ("role", this);
    updateLinkMembership();
}

FeedbackDestroyerAudioProcessor::~FeedbackDestroyerAudioProcessor()
{
    apvts.removeParameterListener ("linkGroup", this);
    apvts.removeParameterListener ("role", this);

    if (joinedGroup.load() > 0)
        fd::LinkRegistry::get().leave (joinedGroup.load(), (fd::LinkRole) joinedRole.load());
}

void FeedbackDestroyerAudioProcessor::parameterChanged (const juce::String&, float)
{
    updateLinkMembership(); // atomics only, safe on any thread
}

void FeedbackDestroyerAudioProcessor::updateLinkMembership()
{
    const int group = (int) pLinkGroup->load();
    const int role  = (int) pRole->load();
    const int oldGroup = joinedGroup.load(), oldRole = joinedRole.load();
    if (group == oldGroup && role == oldRole) return;

    if (oldGroup > 0) fd::LinkRegistry::get().leave (oldGroup, (fd::LinkRole) oldRole);
    if (group > 0)    fd::LinkRegistry::get().join (group, (fd::LinkRole) role);
    joinedGroup.store (group);
    joinedRole.store (role);
}

int FeedbackDestroyerAudioProcessor::instancesInGroup() const
{
    const int g = joinedGroup.load();
    if (g <= 0) return 0;
    return fd::LinkRegistry::get().numChannels (g) + fd::LinkRegistry::get().numMasters (g);
}

bool FeedbackDestroyerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void FeedbackDestroyerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, juce::jmax (samplesPerBlock, 1));
    setLatencySamples (0); // analysis-only detector, minimum-phase notches: no added latency
}

void FeedbackDestroyerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    fd::EngineParams ep;
    ep.sensitivity = pSensitivity->load();
    ep.speed       = (int) pSpeed->load();
    // Strength is the one-knob master: 0 % = fully bypassed, 100 % = full depth and full shift.
    const float strength = pStrength->load();
    ep.maxDepthDb  = 6.f + (pMaxDepth->load() - 6.f) * strength;
    ep.shiftHz     = pShiftMax->load() * strength;
    ep.q           = pQ->load();
    ep.numBands    = (int) pBands->load();
    ep.releaseSec  = pRelease->load();
    ep.bypass      = pBypass->load() > 0.5f || strength <= 0.001f;
    engine.setParams (ep);

    if (clearRequested.exchange (false)) engine.clearAll();
    if (lockRequested.exchange (false))  engine.lockAll();
    if (pendingApply.exchange (false))
    {
        float f[fd::FeedbackEngine::kMaxBands], d[fd::FeedbackEngine::kMaxBands];
        const int n = juce::jlimit (0, fd::FeedbackEngine::kMaxBands, pendingCount.load());
        for (int i = 0; i < n; ++i) { f[i] = pendingFreq[(size_t) i].load(); d[i] = pendingDepth[(size_t) i].load(); }
        engine.loadLocked (f, d, n);
    }

    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

juce::AudioProcessorEditor* FeedbackDestroyerAudioProcessor::createEditor()
{
    return new FeedbackDestroyerAudioProcessorEditor (*this);
}

void FeedbackDestroyerAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();

    auto locked = state.getOrCreateChildWithName ("lockedBands", nullptr);
    locked.removeAllChildren (nullptr);
    for (int i = 0; i < fd::FeedbackEngine::kMaxBands; ++i)
    {
        float f = 0.f, d = 0.f;
        bool l = false;
        engine.getBandDisplay (i, f, d, l);
        if (l && f > 0.f)
        {
            juce::ValueTree band ("band");
            band.setProperty ("freq", f, nullptr);
            band.setProperty ("depth", d, nullptr);
            locked.appendChild (band, nullptr);
        }
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void FeedbackDestroyerAudioProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;

    auto tree = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (tree);

    auto locked = tree.getChildWithName ("lockedBands");
    const int n = juce::jmin (locked.getNumChildren(), fd::FeedbackEngine::kMaxBands);
    for (int i = 0; i < n; ++i)
    {
        pendingFreq[(size_t) i].store ((float) (double) locked.getChild (i).getProperty ("freq"));
        pendingDepth[(size_t) i].store ((float) (double) locked.getChild (i).getProperty ("depth"));
    }
    pendingCount.store (n);
    pendingApply.store (true);
    updateLinkMembership();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FeedbackDestroyerAudioProcessor();
}
