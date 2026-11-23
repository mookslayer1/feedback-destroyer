#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double kQNarrow         = 28.0;   // pre-show notches: very narrow
    constexpr double kQLive           = 5.8;    // live notches: about a quarter of an octave wide
    constexpr float  kStartDepthDb    = 6.f;    // a new pre-show notch starts here...
    constexpr float  kLiveStartDb     = 4.f;    // ...a new live notch starts a little gentler
    constexpr float  kDepthStepDb     = 3.f;    // ...and deepens in 3 dB steps while still ringing
    constexpr float  kMinMaxDepthDb   = 6.f;    // Amount = 0   -> automatic cuts stop at 6 dB
    constexpr float  kMaxMaxDepthDb   = 24.f;   // Amount = 100 -> automatic cuts stop at 24 dB
    constexpr float  kHardMaxDepthDb  = 24.f;   // nothing, manual or automatic, goes deeper
    constexpr float  kSecureExtraDb   = 3.f;    // extra cut added to every notch when the pre-show ends
    constexpr float  kGainCeilingDb   = 6.f;    // automatic gain never goes above this
    constexpr float  kGainStepDb      = 0.5f;   // ring-out creeps up this much at a time
    constexpr float  kProcessMaxSec   = 180.f;  // pre-show stops by itself after this long
    constexpr double kReleaseSec      = 12.0;   // a live notch nobody needs lets go after this long
    constexpr float  kAttackDbPerSec  = 240.f;
    constexpr float  kReleaseDbPerSec = 12.f;
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
        juce::ParameterID { "threshold", 1 }, "Threshold",
        juce::NormalisableRange<float> (0.f, 100.f, 0.1f), 50.f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "amount", 1 }, "Amount",
        juce::NormalisableRange<float> (0.f, 100.f, 0.1f), 40.f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "mode", 1 }, "Mode", juce::StringArray { "Pre-show", "Live" }, 0));

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

    lowA.prepare (15, 8192);    // 32768 samples: about 1.5 Hz per bin at 48 kHz, for 30 Hz to 400 Hz
    mainA.prepare (13, 2048);   // 8192 samples: about 6 Hz per bin at 48 kHz, for everything above

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
    return juce::jmin (kMaxHz, 0.45 * sr);   // can't look above what the sample rate allows
}

float FeedbackDestroyerAudioProcessor::currentMaxDepth() const
{
    const float amount = apvts.getRawParameterValue ("amount")->load();
    return juce::jmap (amount, 0.f, 100.f, kMinMaxDepthDb, kMaxMaxDepthDb);
}

float FeedbackDestroyerAudioProcessor::currentPromThr() const
{
    const float threshold = apvts.getRawParameterValue ("threshold")->load();
    return juce::jmap (threshold, 0.f, 100.f, 8.f, 30.f);   // lower Threshold = more sensitive
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Some hosts call processBlock in odd situations while scanning; never touch unprepared buffers.
    if (! mainA.isReady() || ! lowA.isReady() || buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
        return;

    handleCommands();

    const bool live = apvts.getRawParameterValue ("mode")->load() > 0.5f;
    if (live && procState.load() == Running)
        finishProcess();   // switching to Live ends a pre-show run

    const int n   = buffer.getNumSamples();
    const int nch = juce::jmin (buffer.getNumChannels(), 2);

    applyManual();
    updateFilters (n, currentMaxDepth());

    // 1. Apply every active notch to the audio.
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

    // 2. Listen to the result (after the notches), in mono, with both analysers.
    const float* l = buffer.getReadPointer (0);
    const float* r = nch > 1 ? buffer.getReadPointer (1) : l;
    const double hl = highLimit();

    for (int s = 0; s < n; ++s)
    {
        const float m = 0.5f * (l[s] + r[s]);

        if (lowA.push (m))
            runAnalysis (lowA, kMinHz, juce::jmin (400.0, hl), false, live);

        if (mainA.push (m))
            runAnalysis (mainA, 400.0, hl, true, live);
    }

    // 3. Output gain (earned during the pre-show run, then held).
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

void FeedbackDestroyerAudioProcessor::updateFilters (int numSamples, float maxDepth)
{
    const float atk = kAttackDbPerSec * (float) numSamples / (float) sr;
    const float rel = kReleaseDbPerSec * (float) numSamples / (float) sr;

    for (int i = 0; i < kSlots; ++i)
    {
        auto& n = notches[(size_t) i];

        float want = 0.f;

        if (n.state != Free && ! n.releasing && info[(size_t) i].engaged.load())
            want = (n.pinned || n.secured) ? juce::jmin (n.target, kHardMaxDepthDb)
                                           : juce::jmin (n.target, maxDepth);

        if (n.cur < want)       n.cur = juce::jmin (want, n.cur + atk);
        else if (n.cur > want)  n.cur = juce::jmax (want, n.cur - rel);

        if (n.state != Free && n.releasing && n.cur <= 0.01f)
        {
            n = Notch();   // fully released, slot is free again
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

void FeedbackDestroyerAudioProcessor::runAnalysis (Analyzer& a, double lowHz, double highHz, bool isMain, bool live)
{
    Candidate cands[8];
    const int n = a.analyse (sr, lowHz, highHz, currentPromThr(), 1.0, cands, 8);

    updateHeat (a);

    const bool acting = live || procState.load() == Running;
    if (acting && n > 0)
        handleCandidates (cands, n, live);

    if (isMain)
        housekeeping (live);
}

void FeedbackDestroyerAudioProcessor::handleCandidates (const Candidate* cands, int numCands, bool live)
{
    const float maxDepth = currentMaxDepth();
    const bool running = procState.load() == Running;

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

            // A notch switched off from the editor holds its place: don't re-learn it.
            if (! info[(size_t) idx].engaged.load())
                continue;

            // Still ringing despite the notch: deepen it a step, up to the limit.
            n.sinceNeeded = 0;
            if (running)
                settleFrames = 35;

            const bool canDeepen = ! n.pinned && (n.state == Floating || running);
            if (canDeepen && (int) (mainFrames - n.lastDeepen) > 8 && n.target + kDepthStepDb <= maxDepth + 0.01f)
            {
                n.target += kDepthStepDb;
                n.lastDeepen = (int) mainFrames;
            }
            continue;
        }

        // Live: a floating notch within a quarter octave follows the ringing frequency.
        if (live)
        {
            int follow = -1;
            for (int i = 0; i < kSlots; ++i)
            {
                const auto& n = notches[(size_t) i];
                if (n.state == Floating && ! n.pinned && ! n.releasing && std::abs (f / n.freq - 1.0) < 0.19)
                {
                    follow = i;
                    break;
                }
            }

            if (follow >= 0)
            {
                notches[(size_t) follow].freq = f;
                notches[(size_t) follow].sinceNeeded = 0;
                continue;
            }
        }

        // Pre-show: two close frequencies share one wider notch, freeing a slot.
        bool merged = false;
        if (! live)
        {
            for (int t = 0; t < nt; ++t)
            {
                if (std::abs (f / tg[t].f - 1.0) < 0.12)
                {
                    const double f1 = tg[t].f, f2 = f;
                    const double centre = std::sqrt (f1 * f2);
                    tg[t].f = centre;
                    tg[t].q = juce::jlimit (6.0, kQNarrow, centre / (std::abs (f2 - f1) * 1.6 + centre * 0.02));
                    merged = true;
                    break;
                }
            }
        }

        if (! merged && nt < 3)
        {
            tg[nt].f = f;
            tg[nt].q = live ? kQLive : kQNarrow;
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
            // Out of notches and still ringing: pull the gain down, quickly.
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
        n.target = juce::jmin (live ? kLiveStartDb : kStartDepthDb, maxDepth);
        n.lastDeepen = (int) mainFrames;

        info[(size_t) slot].manualDepth.store (0.f);
        info[(size_t) slot].manualQ.store (0.f);
        info[(size_t) slot].engaged.store (true);
        ++newCount;
    }

    // Pre-show: notch first, then back the gain off and pause before creeping again.
    if (newCount > 0 && running)
    {
        gainDb = juce::jmax (0.f, gainDb - (newCount >= 2 ? 2.f : 1.f));
        settleFrames = 35;   // about 1.5 s of calm
    }
}

void FeedbackDestroyerAudioProcessor::housekeeping (bool live)
{
    ++mainFrames;

    const double hopSec = (double) mainA.hop / sr;
    const int releaseFrames = (int) (kReleaseSec / hopSec);

    for (auto& n : notches)
    {
        if (n.state == Free)
            continue;

        ++n.sinceNeeded;

        // Only live notches let go. Pre-show notches stay until the run ends and locks them.
        if (live && n.state == Floating && ! n.pinned && ! n.releasing && n.sinceNeeded >= releaseFrames)
            n.releasing = true;
    }

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
        if (++creepFrames >= 12)   // about every half second
        {
            creepFrames = 0;
            gainDb = juce::jmin (kGainCeilingDb, gainDb + kGainStepDb);
        }
    }
    else if (++ceilFrames >= (int) (3.0 / hopSec))
    {
        finishProcess();   // reached the gain ceiling and stayed calm
        return;
    }

    if (fullFlag || elapsed >= kProcessMaxSec)
        finishProcess();   // out of notches, or out of time
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
    lowA.clearTracking();
    mainA.clearTracking();
}

// Ends the pre-show run: every notch is locked in place, gets its extra safety cut,
// and the output gain stays where it got to.
void FeedbackDestroyerAudioProcessor::finishProcess()
{
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
    procState.store (Idle);
    lowA.clearTracking();
    mainA.clearTracking();
}

void FeedbackDestroyerAudioProcessor::handleCommands()
{
    const int cmd = uiCommand.exchange (CmdNone);

    switch (cmd)
    {
        case CmdReset:   resetAll(); break;
        case CmdStart:   if (apvts.getRawParameterValue ("mode")->load() < 0.5f) startProcess(); break;
        case CmdStop:    if (procState.load() == Running) finishProcess(); break;
        case CmdLockAll: lockAll(); break;
        default: break;
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
