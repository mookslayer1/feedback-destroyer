#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/FeedbackEngine.h"
#include "Core/LinkRegistry.h"

class FeedbackDestroyerAudioProcessor : public juce::AudioProcessor,
                                        private juce::AudioProcessorValueTreeState::Listener
{
public:
    FeedbackDestroyerAudioProcessor();
    ~FeedbackDestroyerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // UI -> audio thread requests (consumed at the top of processBlock)
    void requestClear() { clearRequested.store (true); }
    void requestLock()  { lockRequested.store (true); }

    // For the editor
    void getBand (int i, float& freq, float& depth, bool& locked) const { engine.getBandDisplay (i, freq, depth, locked); }
    int  instancesInGroup() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& id, float newValue) override;
    void updateLinkMembership();

    fd::FeedbackEngine engine;

    std::atomic<float>* pStrength    = nullptr;
    std::atomic<float>* pShiftMax    = nullptr;
    std::atomic<float>* pSensitivity = nullptr;
    std::atomic<float>* pSpeed       = nullptr;
    std::atomic<float>* pMaxDepth    = nullptr;
    std::atomic<float>* pQ           = nullptr;
    std::atomic<float>* pBands       = nullptr;
    std::atomic<float>* pRelease     = nullptr;
    std::atomic<float>* pBypass      = nullptr;
    std::atomic<float>* pLinkGroup   = nullptr;
    std::atomic<float>* pRole        = nullptr;

    std::atomic<bool> clearRequested { false }, lockRequested { false };

    // Locked bands restored from a saved session, handed to the audio thread.
    std::array<std::atomic<float>, fd::FeedbackEngine::kMaxBands> pendingFreq, pendingDepth;
    std::atomic<int>  pendingCount { 0 };
    std::atomic<bool> pendingApply { false };

    std::atomic<int> joinedGroup { 0 }, joinedRole { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessor)
};
