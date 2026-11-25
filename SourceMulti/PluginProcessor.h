#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "Biquad.h"
#include "Analyzer.h"
#include "Welch.h"

class FeedbackDestroyerAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kSlots = 48;        // notch bank
    static constexpr int kHeatBins = 64;     // resolution of the heat strip
    static constexpr double kMinHz = 30.0;
    static constexpr double kMaxHz = 40000.0;

    enum SlotState { Free = 0, Floating = 1, Latched = 2 };
    enum ProcState { Idle = 0, Running = 1, Done = 2, Listening = 3, Measuring = 4, Verifying = 5 };
    enum Command { CmdNone = 0, CmdReset = 1, CmdStart = 2, CmdStop = 3, CmdLockAll = 4 };
    enum EndReason { EndNone = 0, EndCeiling = 1, EndOutOfNotches = 2, EndTimeLimit = 3, EndStopped = 4 };

    struct SlotInfo
    {
        std::atomic<int>   state { 0 };
        std::atomic<float> freq { 0.f }, depth { 0.f }, q { 28.f };
        std::atomic<bool>  manual { false };
        std::atomic<bool>  engaged { true };
        std::atomic<float> manualDepth { 0.f };
        std::atomic<float> manualQ { 0.f };
    };

    FeedbackDestroyerAudioProcessor();
    ~FeedbackDestroyerAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Freqd Out Multi v5.0"; }
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

    juce::AudioProcessorValueTreeState apvts;
    std::array<SlotInfo, kSlots> info;
    std::array<std::atomic<float>, kHeatBins> heat;
    std::atomic<int>   uiCommand { 0 };
    std::atomic<int>   procState { Idle };
    std::atomic<float> gainDbOut { 0.f };
    std::atomic<float> elapsedSec { 0.f };
    std::atomic<float> highLimitHz { 20000.f };
    std::atomic<int>   endReason { EndNone };
    std::atomic<int>   tidiedAtEnd { 0 };   // overlapping notches merged when the last run ended

    // ---- staged pipeline readouts ----
    std::array<std::atomic<float>, kHeatBins> roomTone;   // 1 where the Listen stage found a steady room tone
    std::array<std::atomic<float>, kHeatBins> suspect;    // 0..1 strength of bands the burst flagged as likely to ring
    std::atomic<float> stageProgress { 0.f };             // 0..1 through the current stage
    std::atomic<int>   toneCount { 0 };                   // steady room tones learned
    std::atomic<int>   burstResult { 0 };                 // 0 not run, 1 measured, 2 too quiet to trust
    std::atomic<float> burstLevelUsedDb { -35.f };
    std::atomic<float> loopMarginDb { 0.f };              // estimated margin to feedback at 0 dB plugin gain
    std::atomic<int>   suspectCount { 0 };
    std::atomic<bool>  limitedByCut { false };            // test gain was held back by the Max cut limit
    std::atomic<int>   verifyResult { 0 };                // 0 not run, 1 clean, 2 feedback appeared
    std::atomic<float> verifiedExtraDb { 0.f };           // clean headroom found above the working gain

private:
    struct Notch
    {
        int    state = Free;
        double freq = 0.0;
        double q = 28.0;
        float  cur = 0.f;
        float  target = 0.f;
        int    sinceNeeded = 0;
        int    lastDeepen = 0;
        long   lastDeepenClk = 0;
        bool   releasing = false;
        bool   pinned = false;
        bool   secured = false;
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
    void beginListen();
    void finishListen();
    void beginBurst (bool firstAttempt);
    void runBurst (juce::AudioBuffer<float>& buffer, int numSamples, int numChannels);
    bool buildSuspectMap();
    void finishBurst();
    void beginRingOut();
    void resumeRingOut();
    float depthLimit (const Notch& n) const;
    void beginVerify();
    void endVerify (bool clean);
    void secureAll();
    void clearMaps();
    int  heatBinFor (double freq) const;
    void resetAll();
    void startProcess();
    void finishProcess (int reason);
    void lockAll();
    int  tidyOverlaps();
    void applyRestore();
    void applyManual();
    void updateFilters (int numSamples);
    void runAnalysis (Analyzer& a, double lowHz, double highHz, bool isMain, bool isFast = false);
    void handleCandidates (const Candidate* cands, int numCands, bool fast = false);
    bool mergeIntoExisting (double freq, double maxRatio);
    int  freeSlotCount() const;
    void housekeeping();
    void updateHeat (const Analyzer& a);
    int  findCovering (double freq) const;
    void publishInfo();
    double highLimit() const;

    std::array<Notch, kSlots> notches;

    Analyzer lowA, mainA, fastA;   // fastA: short window, reports in about 5 ms steps for quick reaction above 500 Hz
    long clockSamples = 0;
    std::array<float, kHeatBins> heatVals {};

    double sr = 48000.0;
    long mainFrames = 0;
    float gainDb = 0.f;
    float lastGainLin = 1.f;
    float elapsed = 0.f;
    int settleFrames = 0, creepFrames = 0, ceilFrames = 0;
    bool fullFlag = false;

    // staged pipeline
    Welch wBase, wIn, wNoise;
    juce::Random rng;
    std::vector<float> loopL;            // measured loop gain per bin (dB)
    std::array<float, 200> bandL {};     // loop gain per 1/12-octave band
    int  stageMode = 1;                  // 0 quick, 1 standard, 2 thorough
    float listenSec = 30.f, verifyHoldSec = 5.f, verifyMaxExtraDb = 6.f;
    bool verifyOn = true;
    long burstPos = 0;
    int  burstAttempts = 0;
    float burstDbNow = -35.f;
    float stageTime = 0.f;
    float verifyGain = 0.f, verifyClean = 0.f;
    float safetyDbNow = 3.f;
    float ceilingDb = 12.f, maxCutDb = 12.f;
    int   verifyCycles = 0;
    std::array<float, kHeatBins> roomVals {}, suspectVals {};

    Saved savedPending;
    juce::SpinLock restoreLock;
    std::atomic<bool> restoreFlag { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FeedbackDestroyerAudioProcessor)
};
