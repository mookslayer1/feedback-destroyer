#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "Biquad.h"
#include "Analyzer.h"

class FeedbackDestroyerAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kSlots = 24;        // 24 notches; any of them can float or be locked
    static constexpr int kHeatBins = 64;     // resolution of the heat strip
    static constexpr double kMinHz = 30.0;
    static constexpr double kMaxHz = 40000.0;

    enum SlotState { Free = 0, Floating = 1, Latched = 2 };
    enum ProcState { Idle = 0, Running = 1, Done = 2 };
    enum Command { CmdNone = 0, CmdReset = 1, CmdStart = 2, CmdStop = 3, CmdLockAll = 4 };

    // What the editor is allowed to look at or change.
    struct SlotInfo
    {
        std::atomic<int>   state { 0 };
        std::atomic<float> freq { 0.f }, depth { 0.f }, q { 28.f };   // written by the audio thread
        std::atomic<bool>  manual { false };                          // pinned by a manual cut
        std::atomic<bool>  engaged { true };                          // switched on or off from the editor
        std::atomic<float> manualDepth { 0.f };                       // set from the editor (0 = none)
        std::atomic<float> manualQ { 0.f };                           // set from the editor (0 = none)
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
    std::array<std::atomic<float>, kHeatBins> heat;   // how feedback-like each part of the spectrum looks (dB)
    std::atomic<int>   uiCommand { 0 };
    std::atomic<int>   procState { Idle };
    std::atomic<float> gainDbOut { 0.f };
    std::atomic<float> elapsedSec { 0.f };
    std::atomic<float> highLimitHz { 20000.f };

private:
    struct Notch
    {
        int    state = Free;
        double freq = 0.0;
        double q = 28.0;
        float  cur = 0.f;          // depth being applied right now (dB)
        float  target = 0.f;       // depth we want (dB)
        int    sinceNeeded = 0;
        int    lastDeepen = 0;
        bool   releasing = false;
        bool   pinned = false;     // a manual cut: never changed automatically
        bool   secured = false;    // has had its extra safety cut
        double coefFreq = 0.0, coefQ = 0.0;
        float  coefDepth = -1.f;
        Biquad bq[2];
    };

    struct Saved
    {
        std::array<bool, kSlots>  valid {}, on {}, pinned {};
        std::array<float, kSlots> freq {}, depth {}, q {};
        float gain = 0.f;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void handleCommands();
    void resetAll();
    void startProcess();
    void finishProcess();
    void lockAll();
    void applyRestore();
    void applyManual();
    void updateFilters (int numSamples, float maxDepth);
    void runAnalysis (Analyzer& a, double lowHz, double highHz, bool isMain, bool live);
    void handleCandidates (const Candidate* cands, int numCands, bool live);
    void housekeeping (bool live);
    void updateHeat (const Analyzer& a);
    int  findCovering (double freq) const;
    void publishInfo();
    float currentMaxDepth() const;
    float currentPromThr() const;
    double highLimit() const;

    std::array<Notch, kSlots> notches;

    Analyzer lowA, mainA;
    std::array<float, kHeatBins> heatVals {};

    double sr = 48000.0;
    long mainFrames = 0;
    float gainDb = 0.f;
    float lastGainLin = 1.f;
    float elapsed = 0.f;
    int settleFrames = 0, creepFrames = 0, ceilFrames = 0;
    bool fullFlag = false;

    Saved savedPending;
    juce::SpinLock restoreLock;
    std::atomic<bool> restoreFlag { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessor)
};
