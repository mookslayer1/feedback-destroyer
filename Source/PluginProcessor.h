#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "Biquad.h"

class FeedbackDestroyerAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kLatched = 12;
    static constexpr int kFloating = 12;
    static constexpr int kSlots = kLatched + kFloating;   // slots 0-11 latched, 12-23 floating

    enum SlotState { Free = 0, Floating = 1, Latched = 2 };

    // What the editor is allowed to look at (written by the audio thread).
    struct SlotInfo
    {
        std::atomic<int>   state   { 0 };
        std::atomic<float> freq    { 0.f };
        std::atomic<float> depth   { 0.f };   // current cut in dB (positive number)
        std::atomic<bool>  engaged { true };  // the editor can switch latched notches on and off
    };

    FeedbackDestroyerAudioProcessor();
    ~FeedbackDestroyerAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Freq'd Out"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Shared with the editor
    juce::AudioProcessorValueTreeState apvts;
    std::array<SlotInfo, kSlots> info;
    std::atomic<int>   pendingSlot { -1 };   // floating notch waiting for "lock it?" (-1 = none)
    std::atomic<float> gainDbOut { 0.f };    // automatic output gain currently applied
    std::atomic<int>   uiCommand { 0 };      // 1 = reset, 2 = lock pending, 3 = keep floating

private:
    struct Notch
    {
        int    state = Free;
        double freq = 0.0;
        float  cur = 0.f;          // depth being applied right now (dB)
        float  target = 0.f;       // depth we want (dB)
        int    sinceNeeded = 0;    // analysis frames since this notch last had work to do
        int    lastDeepen = 0;
        bool   releasing = false;
        bool   dismissed = false;  // user chose "keep floating"
        double coefFreq = 0.0;
        float  coefDepth = -1.f;
        Biquad bq[2];
    };

    struct SavedLatched
    {
        bool  valid[kLatched] {};
        float freq[kLatched] {};
        float depth[kLatched] {};
        bool  on[kLatched] {};
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void handleCommands();
    void resetAll();
    void lockPending();
    void applyRestore();
    void updateFilters (int numSamples, float maxDepth);
    void analyse();
    int  findCovering (double freq) const;
    void publishInfo();
    float currentMaxDepth() const;

    std::array<Notch, kSlots> notches;

    double sr = 48000.0;
    static constexpr int fftOrder = 13;
    static constexpr int fftSize = 1 << fftOrder;   // 8192 samples, about 6 Hz per bin at 48 kHz
    static constexpr int hop = 2048;                // analysis runs about every 43 ms at 48 kHz

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, ring, fftData, level, hist;
    std::vector<int> persist, persistNew;
    int ringPos = 0, hopCount = 0, histFilled = 0;
    long frameCount = 0;

    float gainDb = 0.f;
    float lastGainLin = 1.f;
    int settleFrames = 0;
    int creepFrames = 0;

    SavedLatched savedPending;
    juce::SpinLock restoreLock;
    std::atomic<bool> restoreFlag { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessor)
};
