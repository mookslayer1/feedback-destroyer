#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double kQNarrow         = 28.0;   // narrow, surgical static notch
    constexpr double kQWidest         = 6.0;    // widest a merged notch may become (kept narrower to protect tone)
    constexpr float  kStartDepthDb    = 6.f;    // initial Pre-Show cut
    constexpr float  kDepthStepDb     = 3.f;    // additional cut while a notch is still needed
    constexpr float  kHardMaxDepthDb  = 24.f;   // absolute ceiling (manual faders); automatic cuts use the Max cut control
    constexpr float  kWatchBonusDb    = 6.f;    // extra sensitivity inside bands the burst flagged
    constexpr float  kBurstMaxDb      = -20.f;  // loudest the burst may auto-raise to
    constexpr float  kPromThr         = 18.f;   // fixed internal detection prominence
    constexpr int    kMaxVerifyCycles = 4;      // verify -> ring-out repeats before giving up
    constexpr float  kGainStepDb      = 0.5f;
    constexpr float  kProcessMaxSec   = 600.f;   // backstop only; a run normally ends when quiet at the gain ceiling
    constexpr float  kAttackDbPerSec  = 240.f;
    constexpr float  kReleaseDbPerSec = 12.f;
    constexpr double kMergeThirdOctave = 0.26;  // ratio of 2^(1/3) - 1
}

//==============================================================================
FeedbackDestroyerAudioProcessor::FeedbackDestroyerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& h : heat)
        h.store (0.f);
    for (auto& v : roomTone)
        v.store (0.f);
    for (auto& v : suspect)
        v.store (0.f);
}

juce::AudioProcessorValueTreeState::ParameterLayout FeedbackDestroyerAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "lowLimit", 1 }, "Low limit",
        juce::NormalisableRange<float> (30.f, 400.f, 1.f, 0.5f), 80.f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "mode", 1 }, "Stages",
        juce::StringArray { "Quick", "Standard", "Thorough" }, 1));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "safety", 1 }, "Safety cut",
        juce::NormalisableRange<float> (0.f, 6.f, 0.5f), 3.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "push", 1 }, "Test gain",
        juce::NormalisableRange<float> (6.f, 18.f, 1.f), 12.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "maxCut", 1 }, "Max cut",
        juce::NormalisableRange<float> (6.f, 24.f, 1.f), 12.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "burstLevel", 1 }, "Burst level",
        juce::NormalisableRange<float> (-50.f, -20.f, 1.f), -35.f));

    return layout;
}

bool FeedbackDestroyerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

juce::AudioProcessorEditor* FeedbackDestroyerAudioProcessor::createEditor()
{
    return new FeedbackDestroyerAudioProcessorEditor (*this);
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;

    lowA.prepare (15, 8192);
    mainA.prepare (13, 2048);
    wBase.prepare (13);
    wIn.prepare (13);
    wNoise.prepare (13);
    loopL.assign ((size_t) wIn.size / 2 + 1, -200.f);
    clearMaps();

    heatVals.fill (0.f);
    for (auto& h : heat)
        h.store (0.f);

    highLimitHz.store ((float) highLimit());
    lastGainLin = juce::Decibels::decibelsToGain (gainDb);
    mainFrames = 0;
    settleFrames = creepFrames = ceilFrames = 0;

    for (auto& n : notches)
    {
        n.coefDepth = -1.f;
        n.bq[0].reset();
        n.bq[1].reset();
    }
}

double FeedbackDestroyerAudioProcessor::highLimit() const
{
    return juce::jmin (kMaxHz, 0.45 * sr);
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    if (! mainA.isReady() || ! lowA.isReady() || buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
        return;

    handleCommands();
    maxCutDb = apvts.getRawParameterValue ("maxCut")->load();

    const int n   = buffer.getNumSamples();
    const int nch = juce::jmin (buffer.getNumChannels(), 2);

    applyManual();
    updateFilters (n);

    for (int ch = 0; ch < nch; ++ch)
    {
        float* data = buffer.getWritePointer (ch);

        for (auto& notch : notches)
        {
            if (notch.cur <= 0.01f)
                continue;

            for (int s = 0; s < n; ++s)
                data[s] = notch.bq[ch].process (data[s]);
        }
    }

    const int stage = procState.load();
    const bool analyse = stage == Running || stage == Listening || stage == Verifying;

    if (analyse)
    {
        const float* l = buffer.getReadPointer (0);
        const float* r = nch > 1 ? buffer.getReadPointer (1) : l;
        const double hl = highLimit();
        const double lim = juce::jlimit (kMinHz, 400.0, (double) apvts.getRawParameterValue ("lowLimit")->load());

        for (int s = 0; s < n; ++s)
        {
            const float m = 0.5f * (l[s] + r[s]);

            if (stage == Listening)
                wBase.push (m);

            if (lowA.push (m) && lim < 399.0)
                runAnalysis (lowA, lim, juce::jmin (400.0, hl), false);

            if (mainA.push (m))
                runAnalysis (mainA, juce::jmax (400.0, lim), hl, true);
        }
    }

    const float g = juce::Decibels::decibelsToGain (gainDb);
    buffer.applyGainRamp (0, n, lastGainLin, g);
    lastGainLin = g;

    if (stage == Measuring)
        runBurst (buffer, n, nch);

    publishInfo();
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::applyManual()
{
    for (int i = 0; i < kSlots; ++i)
    {
        auto& n = notches[(size_t) i];
        if (n.state == Free)
            continue;

        const float md = info[(size_t) i].manualDepth.load();
        if (md > 0.f)
        {
            n.target = juce::jmin (md, kHardMaxDepthDb);
            n.pinned = true;
            n.secured = true;
        }

        const float mq = info[(size_t) i].manualQ.load();
        if (mq > 0.f)
            n.q = juce::jlimit (1.0, 60.0, (double) mq);
    }
}

void FeedbackDestroyerAudioProcessor::updateFilters (int numSamples)
{
    const float atk = kAttackDbPerSec * (float) numSamples / (float) sr;
    const float rel = kReleaseDbPerSec * (float) numSamples / (float) sr;

    for (int i = 0; i < kSlots; ++i)
    {
        auto& n = notches[(size_t) i];

        float want = 0.f;

        if (n.state != Free && ! n.releasing && info[(size_t) i].engaged.load())
            want = juce::jmin (n.target, kHardMaxDepthDb);

        if (n.cur < want)
            n.cur = juce::jmin (want, n.cur + atk);
        else if (n.cur > want)
            n.cur = juce::jmax (want, n.cur - rel);

        if (n.state != Free && n.releasing && n.cur <= 0.01f)
        {
            n = Notch();
            continue;
        }

        if (n.cur > 0.01f)
        {
            if (n.cur != n.coefDepth || n.freq != n.coefFreq || n.q != n.coefQ)
            {
                for (auto& b : n.bq)
                    b.setPeak (sr, n.freq, n.q, -(double) n.cur);

                n.coefDepth = n.cur;
                n.coefFreq  = n.freq;
                n.coefQ     = n.q;
            }
        }
        else
        {
            n.bq[0].reset();
            n.bq[1].reset();
            n.coefDepth = -1.f;
        }
    }
}

//==============================================================================
int FeedbackDestroyerAudioProcessor::findCovering (double f) const
{
    for (int i = 0; i < kSlots; ++i)
    {
        const auto& n = notches[(size_t) i];

        if (n.state != Free && ! n.releasing && std::abs (f / n.freq - 1.0) < juce::jmax (0.03, 0.45 / n.q))
            return i;
    }

    return -1;
}

void FeedbackDestroyerAudioProcessor::updateHeat (const Analyzer& a)
{
    if (a.lastHi < a.lastLo)
        return;

    const double lo = kMinHz;
    const double span = std::log (highLimit() / lo);
    const double binHz = sr / a.size;

    float local[kHeatBins] = {};
    int b0 = kHeatBins, b1 = -1;

    for (int k = a.lastLo; k <= a.lastHi; ++k)
    {
        const double f = (double) k * binHz;
        const int b = juce::jlimit (0, kHeatBins - 1, (int) std::floor (std::log (f / lo) / span * kHeatBins));
        b0 = juce::jmin (b0, b);
        b1 = juce::jmax (b1, b);
        local[b] = juce::jmax (local[b], a.peakProm[(size_t) k]);
    }

    for (int b = b0; b <= b1; ++b)
        heatVals[(size_t) b] = juce::jmax (heatVals[(size_t) b] * 0.8f, local[b]);
}

int FeedbackDestroyerAudioProcessor::freeSlotCount() const
{
    int c = 0;
    for (const auto& n : notches)
        if (n.state == Free)
            ++c;
    return c;
}

// Folds a new frequency into the nearest unpinned notch (within maxRatio) by moving that notch to the
// geometric centre and widening it so both frequencies sit inside it. Frees a slot for something else.
bool FeedbackDestroyerAudioProcessor::mergeIntoExisting (double f, double maxRatio)
{
    int best = -1;
    double bestRatio = maxRatio;

    for (int i = 0; i < kSlots; ++i)
    {
        const auto& n = notches[(size_t) i];

        if (n.state == Free || n.releasing || n.pinned || ! info[(size_t) i].engaged.load())
            continue;

        const double ratio = juce::jmax (f / n.freq, n.freq / f) - 1.0;

        if (ratio < bestRatio)
        {
            bestRatio = ratio;
            best = i;
        }
    }

    if (best < 0)
        return false;

    auto& n = notches[(size_t) best];
    const double centre = std::sqrt (n.freq * f);
    const double reqQ = centre / (std::abs (f - n.freq) * 1.6 + centre * 0.02);

    n.freq = centre;
    n.q = juce::jmin (n.q, juce::jlimit (kQWidest, kQNarrow, reqQ));
    n.target = juce::jmin (depthLimit (n), juce::jmax (n.target, kStartDepthDb));
    n.sinceNeeded = 0;
    settleFrames = 35;
    return true;
}

void FeedbackDestroyerAudioProcessor::runAnalysis (Analyzer& a, double lowHz, double highHz, bool isMain)
{
    Candidate cands[8];
    const int st = procState.load();

    if (st == Listening)
    {
        // Listening only: learn what the room does on its own, never act on it.
        a.analyse (sr, lowHz, highHz, 999.f, 1.0, cands, 8);
        updateHeat (a);
        a.accumulateBaseline();

        if (isMain)
            housekeeping();

        return;
    }

    const int n = a.analyse (sr, lowHz, highHz, kPromThr, 1.0, cands, 8);

    updateHeat (a);

    if (n > 0)
    {
        handleCandidates (cands, n);

        if (st == Verifying)
        {
            if (++verifyCycles < kMaxVerifyCycles)
                resumeRingOut();     // not clean yet: keep ringing out with the new notches
            else
                endVerify (false);
        }
    }

    if (isMain)
        housekeeping();
}

void FeedbackDestroyerAudioProcessor::handleCandidates (const Candidate* cands, int numCands)
{
    struct Target { double f; double q; };
    Target tg[3];
    int nt = 0;

    double takenF[3] {};
    int taken = 0;

    for (int c = 0; c < numCands && taken < 3; ++c)
    {
        const double f = cands[c].freq;

        bool dup = false;
        for (int t = 0; t < taken; ++t)
            if (std::abs (f / takenF[t] - 1.0) < 0.02)
                dup = true;
        if (dup)
            continue;

        takenF[taken++] = f;

        const int idx = findCovering (f);

        if (idx >= 0)
        {
            auto& n = notches[(size_t) idx];

            if (! info[(size_t) idx].engaged.load())
                continue;

            n.sinceNeeded = 0;
            settleFrames = 35;

            if (! n.pinned && (int) (mainFrames - n.lastDeepen) > 8)
            {
                if (n.target + kDepthStepDb <= depthLimit (n) + 0.01f)
                {
                    n.target += kDepthStepDb;
                    n.lastDeepen = (int) mainFrames;
                }
                else
                {
                    // At the cut limit and still ringing: back the test gain off instead of cutting more tone.
                    gainDb = juce::jmax (0.f, gainDb - 1.f);
                    ceilingDb = juce::jmin (ceilingDb, gainDb);
                    limitedByCut.store (true);
                    n.lastDeepen = (int) mainFrames;
                }
            }

            continue;
        }

        // As the bank fills, close neighbours are folded into one wider notch to keep slots spare.
        const int freeSlots = freeSlotCount();
        const double mergeRatio = freeSlots > 24 ? 0.06 : (freeSlots > 12 ? 0.12 : kMergeThirdOctave);

        if (mergeIntoExisting (f, mergeRatio))
            continue;

        bool merged = false;

        for (int t = 0; t < nt; ++t)
        {
            if (std::abs (f / tg[t].f - 1.0) < 0.12)
            {
                const double f1 = tg[t].f, f2 = f;
                const double centre = std::sqrt (f1 * f2);
                tg[t].f = centre;
                tg[t].q = juce::jlimit (6.0, kQNarrow,
                                         centre / (std::abs (f2 - f1) * 1.6 + centre * 0.02));
                merged = true;
                break;
            }
        }

        if (! merged && nt < 3)
        {
            tg[nt].f = f;
            tg[nt].q = kQNarrow;
            ++nt;
        }
    }

    int newCount = 0;

    for (int t = 0; t < nt; ++t)
    {
        int slot = -1;
        for (int i = 0; i < kSlots; ++i)
            if (notches[(size_t) i].state == Free) { slot = i; break; }

        if (slot < 0)
        {
            if (mergeIntoExisting (tg[t].f, kMergeThirdOctave))
                continue;

            gainDb = juce::jmax (-6.f, gainDb - 3.f);
            settleFrames = 60;
            fullFlag = true;
            continue;
        }

        auto& n = notches[(size_t) slot];
        n = Notch();
        n.state = Floating;
        n.freq = tg[t].f;
        n.q = tg[t].q;
        n.target = kStartDepthDb;
        n.lastDeepen = (int) mainFrames;

        info[(size_t) slot].manualDepth.store (0.f);
        info[(size_t) slot].manualQ.store (0.f);
        info[(size_t) slot].engaged.store (true);
        ++newCount;
    }

    if (newCount > 0)
    {
        gainDb = juce::jmax (0.f, gainDb - (newCount >= 2 ? 2.f : 1.f));
        settleFrames = 35;
    }
}

void FeedbackDestroyerAudioProcessor::housekeeping()
{
    ++mainFrames;

    const double hopSec = (double) mainA.hop / sr;

    for (auto& n : notches)
        if (n.state != Free)
            ++n.sinceNeeded;

    const int st = procState.load();

    if (st == Listening)
    {
        stageTime += (float) hopSec;
        stageProgress.store (juce::jlimit (0.f, 1.f, stageTime / listenSec));

        if (stageTime >= listenSec)
            finishListen();

        return;
    }

    if (st == Verifying)
    {
        stageTime += (float) hopSec;
        stageProgress.store (juce::jlimit (0.f, 1.f, (verifyGain - ceilingDb) / juce::jmax (1.f, verifyMaxExtraDb)));

        if (stageTime >= verifyHoldSec)
        {
            verifyClean = verifyGain;
            stageTime = 0.f;

            if (verifyGain + 3.f > ceilingDb + verifyMaxExtraDb + 0.01f)
                endVerify (true);
            else
            {
                verifyGain += 3.f;
                gainDb = verifyGain;
            }
        }

        return;
    }

    if (st != Running)
        return;

    elapsed += (float) hopSec;

    if (settleFrames > 0)
    {
        --settleFrames;
        creepFrames = 0;
        ceilFrames = 0;
    }
    else if (gainDb < ceilingDb)
    {
        ceilFrames = 0;

        if (++creepFrames >= 12)
        {
            creepFrames = 0;
            gainDb = juce::jmin (ceilingDb, gainDb + kGainStepDb);
        }
    }
    else if (++ceilFrames >= (int) (3.0 / hopSec))
    {
        finishProcess (EndCeiling);
        return;
    }

    if (fullFlag)
        finishProcess (EndOutOfNotches);
    else if (elapsed >= kProcessMaxSec)
        finishProcess (EndTimeLimit);
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::startProcess()
{
    const int st = procState.load();

    if (st == Running || st == Listening || st == Measuring || st == Verifying)
        return;

    stageMode = juce::jlimit (0, 2, (int) apvts.getRawParameterValue ("mode")->load());
    listenSec = 5.f;                    // a short, fixed listen at the start of every run
    verifyOn = stageMode != 0;
    verifyHoldSec = stageMode == 2 ? 8.f : 5.f;
    verifyMaxExtraDb = stageMode == 2 ? 9.f : 6.f;

    gainDb = 0.f;                       // measure the room at unity so the margin reading is meaningful
    ceilingDb = juce::jlimit (6.f, 18.f, apvts.getRawParameterValue ("push")->load());
    verifyCycles = 0;
    limitedByCut.store (false);
    elapsed = 0.f;
    settleFrames = creepFrames = ceilFrames = 0;
    fullFlag = false;
    endReason.store (EndNone);
    tidiedAtEnd.store (0);
    verifyResult.store (0);
    verifiedExtraDb.store (0.f);
    burstResult.store (0);
    suspectCount.store (0);
    toneCount.store (0);
    loopMarginDb.store (0.f);
    clearMaps();
    beginListen();
}

void FeedbackDestroyerAudioProcessor::beginListen()
{
    lowA.clearBaseline();
    mainA.clearBaseline();
    lowA.clearWatch();
    mainA.clearWatch();
    lowA.clearTracking();
    mainA.clearTracking();
    wBase.reset();
    stageTime = 0.f;
    stageProgress.store (0.f);
    procState.store (Listening);
}

float FeedbackDestroyerAudioProcessor::depthLimit (const Notch& n) const
{
    const float lim = juce::jlimit (6.f, kHardMaxDepthDb, maxCutDb);
    return n.q < 10.0 ? juce::jmin (lim, 9.f) : lim;     // wide notches take more tone, so they stay shallower
}

void FeedbackDestroyerAudioProcessor::resumeRingOut()
{
    gainDb = juce::jmin (ceilingDb, verifyClean);
    settleFrames = 35;
    creepFrames = ceilFrames = 0;
    stageProgress.store (0.f);
    procState.store (Running);
}

int FeedbackDestroyerAudioProcessor::heatBinFor (double f) const
{
    const double span = std::log (highLimit() / kMinHz);
    return juce::jlimit (0, kHeatBins - 1, (int) std::floor (std::log (juce::jmax (kMinHz, f) / kMinHz) / span * kHeatBins));
}

void FeedbackDestroyerAudioProcessor::clearMaps()
{
    roomVals.fill (0.f);
    suspectVals.fill (0.f);
}

void FeedbackDestroyerAudioProcessor::finishListen()
{
    const int tones = lowA.finishBaseline() + mainA.finishBaseline();
    toneCount.store (tones);

    roomVals.fill (0.f);

    for (int pass = 0; pass < 2; ++pass)
    {
        const Analyzer& a = pass == 0 ? lowA : mainA;

        for (int k = 2; k <= a.size / 2 - 2; ++k)
        {
            if (! a.isTone (k))
                continue;

            const double f = (double) k * sr / a.size;

            if ((pass == 0 && f < 400.0) || (pass == 1 && f >= 400.0))
                roomVals[(size_t) heatBinFor (f)] = 1.f;
        }
    }

    beginBurst (true);
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::beginBurst (bool firstAttempt)
{
    if (firstAttempt)
    {
        burstAttempts = 0;
        burstDbNow = juce::jlimit (-50.f, kBurstMaxDb, apvts.getRawParameterValue ("burstLevel")->load());
    }

    burstLevelUsedDb.store (burstDbNow);
    wIn.reset();
    wNoise.reset();
    burstPos = 0;
    stageProgress.store (0.f);
    procState.store (Measuring);
}

// Adds a smooth (faded in and out) burst of noise to the output and records what comes back in.
void FeedbackDestroyerAudioProcessor::runBurst (juce::AudioBuffer<float>& buffer, int numSamples, int numChannels)
{
    const float level = juce::Decibels::decibelsToGain (burstDbNow);
    const long rampN = (long) (0.6 * sr), holdN = (long) (2.0 * sr), tailN = (long) (0.8 * sr);
    const long noiseN = 2 * rampN + holdN, totalN = noiseN + tailN;

    const float* l = buffer.getReadPointer (0);
    const float* r = numChannels > 1 ? buffer.getReadPointer (1) : l;

    for (int s = 0; s < numSamples; ++s)
    {
        const float x = 0.5f * (l[s] + r[s]);
        float nz = 0.f;

        if (burstPos < noiseN)
        {
            double env = 1.0;

            if (burstPos < rampN)
                env = 0.5 - 0.5 * std::cos (3.141592653589793 * (double) burstPos / (double) rampN);
            else if (burstPos >= rampN + holdN)
                env = 0.5 - 0.5 * std::cos (3.141592653589793 * (double) (noiseN - burstPos) / (double) rampN);

            nz = (rng.nextFloat() * 2.f - 1.f) * 1.7320508f * level * (float) env;
        }

        wIn.push (x);
        wNoise.push (nz);

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.getWritePointer (ch)[s] += nz;

        ++burstPos;

        if (burstPos >= totalN)
        {
            finishBurst();
            return;
        }
    }

    stageProgress.store ((float) burstPos / (float) totalN);
}

// Turns the burst recording into a loop-gain map and picks the bands most likely to ring.
bool FeedbackDestroyerAudioProcessor::buildSuspectMap()
{
    const int half = wIn.size / 2;
    const double binHz = sr / wIn.size;

    if (! wBase.hasData() || ! wIn.hasData())
        return false;

    // Did the burst rise clearly above the room's own noise?
    double rise = 0.0;
    int rn = 0;
    for (int k = juce::jmax (2, (int) std::ceil (200.0 / binHz)); k <= juce::jmin (half - 2, (int) std::floor (4000.0 / binHz)); ++k)
    {
        const double pb = juce::jmax (1.0e-30, wBase.power (k));
        rise += 10.0 * std::log10 (juce::jmax (1.0e-30, wIn.power (k)) / pb);
        ++rn;
    }

    if (rn == 0 || rise / rn < 3.0)
        return false;

    for (int k = 0; k <= half; ++k)
    {
        const double pn = wNoise.power (k);
        const double px = wIn.power (k);
        const double ex = juce::jmax (px - wBase.power (k), 1.0e-3 * px);
        loopL[(size_t) k] = pn > 1.0e-30 ? (float) (10.0 * std::log10 (juce::jmax (1.0e-30, ex) / pn)) : -200.f;

        // A steady room tone (fan, hum) is not part of the loop; its baseline never cancels exactly, so skip it here.
        const double f = (double) k * binHz;
        const Analyzer& ta = f < 400.0 ? lowA : mainA;
        const int tk = (int) std::lround (f * ta.size / sr);

        if (ta.isTone (tk - 2) || ta.isTone (tk - 1) || ta.isTone (tk) || ta.isTone (tk + 1) || ta.isTone (tk + 2))
            loopL[(size_t) k] = -200.f;
    }

    const double lo = juce::jmax (40.0, (double) apvts.getRawParameterValue ("lowLimit")->load());
    const double hi = juce::jmin (highLimit(), 0.45 * sr);
    const int nb = juce::jlimit (4, (int) bandL.size(), (int) std::floor (12.0 * std::log2 (hi / lo)));

    float M = -200.f;

    for (int b = 0; b < nb; ++b)
    {
        const double f0 = lo * std::pow (2.0, b / 12.0), f1 = lo * std::pow (2.0, (b + 1) / 12.0);
        int k0 = (int) std::ceil (f0 / binHz), k1 = (int) std::floor (f1 / binHz);

        if (k1 < k0)
            k0 = k1 = (int) std::lround (std::sqrt (f0 * f1) / binHz);

        k0 = juce::jlimit (2, half - 2, k0);
        k1 = juce::jlimit (k0, half - 2, k1);

        float m = -200.f;
        for (int k = k0; k <= k1; ++k)
            m = juce::jmax (m, loopL[(size_t) k]);

        bandL[(size_t) b] = m;
        M = juce::jmax (M, m);
    }

    if (M < -150.f)
        return false;

    loopMarginDb.store (-M - gainDb);

    // Candidate bands: close to the worst band, or standing well above their neighbourhood.
    int picked[10];
    int np = 0;

    for (int round = 0; round < 10; ++round)
    {
        int best = -1;
        float bestL = -300.f;

        for (int b = 1; b < nb - 1; ++b)
        {
            if (bandL[(size_t) b] < bandL[(size_t) b - 1] || bandL[(size_t) b] < bandL[(size_t) b + 1])
                continue;

            bool used = false;
            for (int i = 0; i < np; ++i)
                used = used || std::abs (picked[i] - b) < 3;
            if (used)
                continue;

            float sum = 0.f;
            int cnt = 0;
            for (int d = -12; d <= 12; ++d)
            {
                const int j = b + d;
                if (std::abs (d) >= 2 && j >= 0 && j < nb) { sum += bandL[(size_t) j]; ++cnt; }
            }
            const float trend = cnt > 0 ? sum / (float) cnt : bandL[(size_t) b];

            const bool suspicious = bandL[(size_t) b] >= M - 8.f || bandL[(size_t) b] - trend >= 9.f;

            if (suspicious && bandL[(size_t) b] > bestL)
            {
                bestL = bandL[(size_t) b];
                best = b;
            }
        }

        if (best < 0)
            break;

        picked[np++] = best;
    }

    lowA.clearWatch();
    mainA.clearWatch();
    suspectVals.fill (0.f);

    for (int i = 0; i < np; ++i)
    {
        const double fc = lo * std::pow (2.0, (picked[i] + 0.5) / 12.0);
        lowA.addWatch (sr, fc, 1.0 / 6.0, kWatchBonusDb);
        mainA.addWatch (sr, fc, 1.0 / 6.0, kWatchBonusDb);

        const float strength = juce::jlimit (0.2f, 1.f, 1.f - (M - bandL[(size_t) picked[i]]) / 16.f);
        const int hb = heatBinFor (fc);
        suspectVals[(size_t) hb] = juce::jmax (suspectVals[(size_t) hb], strength);
    }

    suspectCount.store (np);
    return true;
}

void FeedbackDestroyerAudioProcessor::finishBurst()
{
    const bool ok = buildSuspectMap();

    if (! ok && burstDbNow + 6.f <= kBurstMaxDb + 0.01f && burstAttempts < 3)
    {
        ++burstAttempts;
        burstDbNow += 6.f;        // came back too quiet to trust: try again a little louder
        beginBurst (false);
        return;
    }

    burstResult.store (ok ? 1 : 2);
    beginRingOut();
}

void FeedbackDestroyerAudioProcessor::beginRingOut()
{
    elapsed = 0.f;
    settleFrames = creepFrames = ceilFrames = 0;
    fullFlag = false;
    lowA.clearTracking();
    mainA.clearTracking();
    stageProgress.store (0.f);
    procState.store (Running);
}

void FeedbackDestroyerAudioProcessor::beginVerify()
{
    verifyClean = gainDb;
    verifyGain = gainDb + 3.f;
    gainDb = verifyGain;
    stageTime = 0.f;
    stageProgress.store (0.f);
    lowA.clearTracking();
    mainA.clearTracking();
    procState.store (Verifying);
}

void FeedbackDestroyerAudioProcessor::endVerify (bool clean)
{
    secureAll();
    verifiedExtraDb.store (juce::jmax (0.f, verifyClean));   // highest gain that held clean
    verifyResult.store (clean ? 1 : 2);
    gainDb = 0.f;                    // the raised gain was only test pressure; leave the channel at unity
    endReason.store (EndCeiling);
    procState.store (Done);
    creepFrames = ceilFrames = settleFrames = 0;
}

void FeedbackDestroyerAudioProcessor::secureAll()
{
    safetyDbNow = juce::jlimit (0.f, 6.f, apvts.getRawParameterValue ("safety")->load());

    for (auto& n : notches)
    {
        if (n.state == Free)
            continue;

        if (! n.secured)
        {
            if (! n.pinned)
                n.target = juce::jmin (juce::jmax (n.target, depthLimit (n)), n.target + safetyDbNow);

            n.secured = true;
        }

        n.state = Latched;
        n.releasing = false;
    }
}

// Merges pairs of unpinned notches whose bands touch or nearly touch into one wider notch, freeing slots.
int FeedbackDestroyerAudioProcessor::tidyOverlaps()
{
    int merges = 0;

    for (int pass = 0; pass < kSlots; ++pass)
    {
        int bi = -1, bj = -1;
        double bestGap = 1e9;

        for (int i = 0; i < kSlots; ++i)
        {
            const auto& a = notches[(size_t) i];

            if (a.state == Free || a.releasing || a.pinned || ! info[(size_t) i].engaged.load())
                continue;

            for (int j = i + 1; j < kSlots; ++j)
            {
                const auto& b = notches[(size_t) j];

                if (b.state == Free || b.releasing || b.pinned || ! info[(size_t) j].engaged.load())
                    continue;

                const double ha = a.freq / (2.0 * a.q), hb = b.freq / (2.0 * b.q);
                const double d = std::abs (a.freq - b.freq);
                const double gap = d / (ha + hb);

                if (gap < 1.25 && gap < bestGap)
                {
                    bestGap = gap;
                    bi = i;
                    bj = j;
                }
            }
        }

        if (bi < 0)
            break;

        auto& a = notches[(size_t) bi];
        const auto& b = notches[(size_t) bj];

        const double lo = juce::jmin (a.freq - a.freq / (2.0 * a.q), b.freq - b.freq / (2.0 * b.q));
        const double hi = juce::jmax (a.freq + a.freq / (2.0 * a.q), b.freq + b.freq / (2.0 * b.q));
        const double centre = std::sqrt (a.freq * b.freq);

        a.freq = centre;
        a.q = juce::jlimit (kQWidest, kQNarrow, centre / juce::jmax (1.0, hi - lo));
        a.target = juce::jmax (a.target, b.target);
        a.cur = juce::jmax (a.cur, b.cur);
        a.secured = a.secured && b.secured;
        if (b.state == Floating)
            a.state = a.state == Latched ? Latched : Floating;

        notches[(size_t) bj] = Notch();
        info[(size_t) bj].engaged.store (true);
        info[(size_t) bj].manualDepth.store (0.f);
        info[(size_t) bj].manualQ.store (0.f);
        ++merges;
    }

    return merges;
}

void FeedbackDestroyerAudioProcessor::finishProcess (int reason)
{
    tidiedAtEnd.store (tidyOverlaps());
    secureAll();

    if (reason == EndCeiling && verifyOn && procState.load() == Running)
    {
        beginVerify();      // lock first, then lift the gain to see how much headroom is really left
        return;
    }

    endReason.store (reason);
    gainDb = 0.f;
    procState.store (Done);
    creepFrames = ceilFrames = settleFrames = 0;
}

void FeedbackDestroyerAudioProcessor::lockAll()
{
    for (auto& n : notches)
    {
        if (n.state != Free)
        {
            n.state = Latched;
            n.releasing = false;
        }
    }
}

void FeedbackDestroyerAudioProcessor::resetAll()
{
    for (int i = 0; i < kSlots; ++i)
    {
        notches[(size_t) i] = Notch();
        info[(size_t) i].engaged.store (true);
        info[(size_t) i].manualDepth.store (0.f);
        info[(size_t) i].manualQ.store (0.f);
    }

    gainDb = 0.f;
    elapsed = 0.f;
    settleFrames = creepFrames = ceilFrames = 0;
    fullFlag = false;
    endReason.store (EndNone);
    tidiedAtEnd.store (0);
    procState.store (Idle);
    lowA.clearTracking();
    mainA.clearTracking();
    lowA.clearBaseline();
    mainA.clearBaseline();
    lowA.clearWatch();
    mainA.clearWatch();
    clearMaps();
    verifyResult.store (0);
    verifiedExtraDb.store (0.f);
    burstResult.store (0);
    suspectCount.store (0);
    toneCount.store (0);
    loopMarginDb.store (0.f);
    stageProgress.store (0.f);
}

void FeedbackDestroyerAudioProcessor::handleCommands()
{
    const int cmd = uiCommand.exchange (CmdNone);
    switch (cmd)
    {
        case CmdReset:
            resetAll();
            break;

        case CmdStart:
            startProcess();
            break;

        case CmdStop:
            {
                const int st = procState.load();

                if (st == Running)
                    finishProcess (EndStopped);
                else if (st == Verifying)
                    endVerify (true);
                else if (st == Listening || st == Measuring)
                    procState.store (Idle);       // nothing has been changed yet, so just stop
            }
            break;

        case CmdLockAll:
            lockAll();
            break;

        default:
            break;
    }

    if (restoreFlag.load())
    {
        const juce::SpinLock::ScopedTryLockType lock (restoreLock);

        if (lock.isLocked())
        {
            applyRestore();
            restoreFlag.store (false);
        }
    }
}

void FeedbackDestroyerAudioProcessor::applyRestore()
{
    for (int i = 0; i < kSlots; ++i)
    {
        notches[(size_t) i] = Notch();
        info[(size_t) i].manualDepth.store (0.f);
        info[(size_t) i].manualQ.store (0.f);

        if (savedPending.valid[(size_t) i])
        {
            auto& n = notches[(size_t) i];
            n.state = Latched;
            n.freq = savedPending.freq[(size_t) i];
            n.q = savedPending.q[(size_t) i];
            n.target = savedPending.depth[(size_t) i];
            n.cur = savedPending.depth[(size_t) i];
            n.pinned = savedPending.pinned[(size_t) i];
            n.secured = true;
            info[(size_t) i].engaged.store (savedPending.on[(size_t) i]);
        }
    }

    gainDb = savedPending.gain;
    procState.store (Idle);
}

void FeedbackDestroyerAudioProcessor::publishInfo()
{
    for (int i = 0; i < kSlots; ++i)
    {
        const auto& n = notches[(size_t) i];
        info[(size_t) i].state.store (n.state);
        info[(size_t) i].freq.store ((float) n.freq);
        info[(size_t) i].depth.store (n.cur);
        info[(size_t) i].q.store ((float) n.q);
        info[(size_t) i].manual.store (n.pinned);
    }

    for (int b = 0; b < kHeatBins; ++b)
        heat[(size_t) b].store (heatVals[(size_t) b]);

    for (int b = 0; b < kHeatBins; ++b)
    {
        roomTone[(size_t) b].store (roomVals[(size_t) b]);
        suspect[(size_t) b].store (suspectVals[(size_t) b]);
    }

    gainDbOut.store (gainDb);
    elapsedSec.store (elapsed);
    highLimitHz.store ((float) highLimit());
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("gainDb", gainDbOut.load(), nullptr);

    juce::ValueTree locked ("LOCKED");

    for (int i = 0; i < kSlots; ++i)
    {
        if (info[(size_t) i].state.load() != Latched)
            continue;

        juce::ValueTree n ("N");
        n.setProperty ("slot", i, nullptr);
        n.setProperty ("f", info[(size_t) i].freq.load(), nullptr);
        n.setProperty ("d", juce::jmax (info[(size_t) i].depth.load(), 3.f), nullptr);
        n.setProperty ("q", info[(size_t) i].q.load(), nullptr);
        n.setProperty ("on", info[(size_t) i].engaged.load(), nullptr);
        n.setProperty ("m", info[(size_t) i].manual.load(), nullptr);
        locked.appendChild (n, nullptr);
    }

    state.appendChild (locked, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FeedbackDestroyerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    auto locked = tree.getChildWithName ("LOCKED");

    {
        const juce::SpinLock::ScopedLockType lock (restoreLock);
        savedPending = Saved();
        savedPending.gain = (float) (double) tree.getProperty ("gainDb", 0.0);

        for (int c = 0; c < locked.getNumChildren(); ++c)
        {
            auto n = locked.getChild (c);
            const int slot = n.getProperty ("slot", -1);

            if (slot >= 0 && slot < kSlots)
            {
                savedPending.valid[(size_t) slot]  = true;
                savedPending.freq[(size_t) slot]   = (float) (double) n.getProperty ("f", 1000.0);
                savedPending.depth[(size_t) slot]  = (float) (double) n.getProperty ("d", 6.0);
                savedPending.q[(size_t) slot]      = (float) (double) n.getProperty ("q", 28.0);
                savedPending.on[(size_t) slot]     = (bool) n.getProperty ("on", true);
                savedPending.pinned[(size_t) slot] = (bool) n.getProperty ("m", false);
            }
        }

        restoreFlag.store (true);
    }

    if (locked.isValid())
        tree.removeChild (locked, nullptr);

    apvts.replaceState (tree);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FeedbackDestroyerAudioProcessor();
}
