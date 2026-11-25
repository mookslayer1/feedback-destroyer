#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double kQNarrow         = 28.0;   // narrow, surgical static notch
    constexpr double kQWidest         = 3.0;    // widest a merged notch may become
    constexpr float  kStartDepthDb    = 6.f;    // initial Pre-Show cut
    constexpr float  kDepthStepDb     = 3.f;    // additional cut while a notch is still needed
    constexpr float  kHardMaxDepthDb  = 24.f;   // safety ceiling for an automatically deployed notch
    constexpr float  kSecureExtraDb   = 6.f;    // failsafe margin added when a Pre-Show notch is locked
    constexpr float  kPromThr         = 18.f;   // fixed internal detection prominence
    constexpr float  kGainCeilingDb   = 6.f;
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
}

juce::AudioProcessorValueTreeState::ParameterLayout FeedbackDestroyerAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "lowLimit", 1 }, "Low limit",
        juce::NormalisableRange<float> (30.f, 400.f, 1.f, 0.5f), 30.f));

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

    const bool analyse = procState.load() == Running;

    if (analyse)
    {
        const float* l = buffer.getReadPointer (0);
        const float* r = nch > 1 ? buffer.getReadPointer (1) : l;
        const double hl = highLimit();
        const double lim = juce::jlimit (kMinHz, 400.0, (double) apvts.getRawParameterValue ("lowLimit")->load());

        for (int s = 0; s < n; ++s)
        {
            const float m = 0.5f * (l[s] + r[s]);

            if (lowA.push (m) && lim < 399.0)
                runAnalysis (lowA, lim, juce::jmin (400.0, hl), false);

            if (mainA.push (m))
                runAnalysis (mainA, juce::jmax (400.0, lim), hl, true);
        }
    }

    const float g = juce::Decibels::decibelsToGain (gainDb);
    buffer.applyGainRamp (0, n, lastGainLin, g);
    lastGainLin = g;

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
    n.target = juce::jmax (n.target, kStartDepthDb);
    n.sinceNeeded = 0;
    settleFrames = 35;
    return true;
}

void FeedbackDestroyerAudioProcessor::runAnalysis (Analyzer& a, double lowHz, double highHz, bool isMain)
{
    Candidate cands[8];
    const int n = a.analyse (sr, lowHz, highHz, kPromThr, 1.0, cands, 8);

    updateHeat (a);

    if (n > 0)
        handleCandidates (cands, n);

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

            if (! n.pinned && (int) (mainFrames - n.lastDeepen) > 8
                && n.target + kDepthStepDb <= kHardMaxDepthDb + 0.01f)
            {
                n.target += kDepthStepDb;
                n.lastDeepen = (int) mainFrames;
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

    if (procState.load() != Running)
        return;

    elapsed += (float) hopSec;

    if (settleFrames > 0)
    {
        --settleFrames;
        creepFrames = 0;
        ceilFrames = 0;
    }
    else if (gainDb < kGainCeilingDb)
    {
        ceilFrames = 0;

        if (++creepFrames >= 12)
        {
            creepFrames = 0;
            gainDb = juce::jmin (kGainCeilingDb, gainDb + kGainStepDb);
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
    if (procState.load() == Running)
        return;

    procState.store (Running);
    elapsed = 0.f;
    settleFrames = creepFrames = ceilFrames = 0;
    fullFlag = false;
    endReason.store (EndNone);
    tidiedAtEnd.store (0);
    lowA.clearTracking();
    mainA.clearTracking();
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

    for (auto& n : notches)
    {
        if (n.state == Free)
            continue;

        if (! n.secured)
        {
            if (! n.pinned)
                n.target = juce::jmin (kHardMaxDepthDb, n.target + kSecureExtraDb);

            n.secured = true;
        }

        n.state = Latched;
        n.releasing = false;
    }

    endReason.store (reason);
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
            if (procState.load() == Running)
                finishProcess (EndStopped);
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
