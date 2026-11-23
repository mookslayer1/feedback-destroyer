#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr double kQ              = 28.0;   // very narrow notches
    constexpr float  kStartDepthDb   = 6.f;    // a new notch starts shallow...
    constexpr float  kDepthStepDb    = 3.f;    // ...and deepens in 3 dB steps while still needed
    constexpr float  kMinMaxDepthDb  = 6.f;    // Amount = 0   -> deepest cut allowed is 6 dB
    constexpr float  kMaxMaxDepthDb  = 24.f;   // Amount = 100 -> deepest cut allowed is 24 dB
    constexpr float  kGainCeilingDb  = 6.f;    // automatic gain never goes above this
    constexpr float  kGainStepDb     = 0.5f;   // ring-out creeps up this much at a time
    constexpr double kReadyToLockSec = 4.0;    // a floating notch that has held this long asks to be locked
    constexpr double kReleaseSec     = 12.0;   // a floating notch nobody needs lets go after this long
    constexpr double kCoverRatio     = 0.03;   // a notch covers anything within 3% of its frequency
    constexpr float  kAttackDbPerSec = 240.f;  // how fast a cut is applied
    constexpr float  kReleaseDbPerSec = 12.f;  // how fast a cut is removed
}

//==============================================================================
FeedbackDestroyerAudioProcessor::FeedbackDestroyerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
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

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "ringout", 1 }, "Ring-out", false));

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
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    window.assign ((size_t) fftSize, 0.f);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (
        window.data(), (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);

    ring.assign ((size_t) fftSize, 0.f);
    fftData.assign ((size_t) fftSize * 2, 0.f);
    level.assign ((size_t) fftSize / 2 + 1, -120.f);
    hist.assign ((size_t) (fftSize / 2 + 1) * 4, -120.f);
    persist.assign ((size_t) fftSize / 2 + 1, 0);
    persistNew.assign ((size_t) fftSize / 2 + 1, 0);

    ringPos = hopCount = histFilled = 0;
    frameCount = 0;
    settleFrames = creepFrames = 0;
    lastGainLin = juce::Decibels::decibelsToGain (gainDb);

    for (auto& n : notches)
    {
        n.coefDepth = -1.f;
        n.bq[0].reset();
        n.bq[1].reset();
    }
}

float FeedbackDestroyerAudioProcessor::currentMaxDepth() const
{
    const float amount = apvts.getRawParameterValue ("amount")->load();
    return juce::jmap (amount, 0.f, 100.f, kMinMaxDepthDb, kMaxMaxDepthDb);
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Some hosts call processBlock in odd situations while scanning; never touch unprepared buffers.
    if (fft == nullptr || ring.empty() || buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
        return;

    handleCommands();

    const int n   = buffer.getNumSamples();
    const int nch = juce::jmin (buffer.getNumChannels(), 2);

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

    // 2. Listen to the result (after the notches), in mono, and analyse it.
    const float* l = buffer.getReadPointer (0);
    const float* r = nch > 1 ? buffer.getReadPointer (1) : l;

    for (int s = 0; s < n; ++s)
    {
        ring[(size_t) ringPos] = 0.5f * (l[s] + r[s]);
        ringPos = (ringPos + 1) & (fftSize - 1);

        if (++hopCount >= hop)
        {
            hopCount = 0;
            analyse();
        }
    }

    // 3. Automatic output gain (earned during ring-out).
    const float g = juce::Decibels::decibelsToGain (gainDb);
    buffer.applyGainRamp (0, n, lastGainLin, g);
    lastGainLin = g;

    publishInfo();
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::updateFilters (int numSamples, float maxDepth)
{
    const float atk = kAttackDbPerSec * (float) numSamples / (float) sr;
    const float rel = kReleaseDbPerSec * (float) numSamples / (float) sr;

    for (int i = 0; i < kSlots; ++i)
    {
        auto& n = notches[(size_t) i];

        float want = 0.f;

        if (n.state != Free && ! n.releasing)
        {
            const bool switchedOff = (n.state == Latched && ! info[(size_t) i].engaged.load());
            want = switchedOff ? 0.f : juce::jmin (n.target, maxDepth);
        }

        if (n.cur < want)       n.cur = juce::jmin (want, n.cur + atk);
        else if (n.cur > want)  n.cur = juce::jmax (want, n.cur - rel);

        if (n.state != Free && n.releasing && n.cur <= 0.01f)
        {
            n = Notch();   // fully released, slot is free again
            continue;
        }

        if (n.cur > 0.01f)
        {
            if (n.cur != n.coefDepth || n.freq != n.coefFreq)
            {
                for (auto& b : n.bq)
                    b.setPeak (sr, n.freq, kQ, -(double) n.cur);

                n.coefDepth = n.cur;
                n.coefFreq  = n.freq;
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

        if (n.state != Free && ! n.releasing && std::abs (f / n.freq - 1.0) < kCoverRatio)
            return i;
    }

    return -1;
}

void FeedbackDestroyerAudioProcessor::analyse()
{
    const int half = fftSize / 2;

    // --- spectrum of the last 8192 samples --------------------------------
    for (int j = 0; j < fftSize; ++j)
        fftData[(size_t) j] = ring[(size_t) ((ringPos + j) & (fftSize - 1))] * window[(size_t) j];

    std::fill (fftData.begin() + fftSize, fftData.end(), 0.f);
    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

    const float norm = 1.f / ((float) fftSize * 0.25f);   // a full-scale sine reads about 0 dB
    for (int k = 0; k <= half; ++k)
        level[(size_t) k] = juce::Decibels::gainToDecibels (fftData[(size_t) k] * norm, -120.f);

    // --- find peaks that look like feedback ----------------------------------
    const float threshold = apvts.getRawParameterValue ("threshold")->load();
    const float promThr   = juce::jmap (threshold, 0.f, 100.f, 8.f, 30.f);   // lower Threshold = more sensitive

    const int kLo = juce::jmax (14, (int) std::floor (80.0 * fftSize / sr));
    const int kHi = juce::jmin (half - 16, (int) std::ceil (8000.0 * fftSize / sr));

    struct Cand { float prom; double freq; };
    Cand cands[8];
    int numCands = 0;

    std::fill (persistNew.begin(), persistNew.end(), 0);
    const size_t oldSlot = (size_t) ((frameCount + 1) % 4);   // the frame from 3 frames ago

    for (int k = kLo; k <= kHi; ++k)
    {
        const float lv = level[(size_t) k];

        if (lv < -65.f)
            continue;

        const bool isPeak = lv > level[(size_t) k - 1] && lv >= level[(size_t) k + 1]
                         && lv > level[(size_t) k - 2] && lv >= level[(size_t) k + 2];
        if (! isPeak)
            continue;

        float sum = 0.f;
        for (int d = 6; d <= 14; ++d)
            sum += level[(size_t) (k - d)] + level[(size_t) (k + d)];

        const float prom = lv - sum / 18.f;
        if (prom < promThr)
            continue;

        const int p = 1 + juce::jmax (persist[(size_t) k - 1], persist[(size_t) k], persist[(size_t) k + 1]);
        persistNew[(size_t) k] = p;

        const bool growing = histFilled >= 4
                          && (lv - hist[oldSlot * (size_t) (half + 1) + (size_t) k]) >= 2.5f;
        const bool steady  = p >= 20 && prom >= promThr + 6.f;

        if ((p >= 3 && growing) || steady)
        {
            // sharpen the frequency estimate between bins
            const float a = level[(size_t) k - 1], b = lv, c = level[(size_t) k + 1];
            const float denom = a - 2.f * b + c;
            const float delta = std::abs (denom) > 1.0e-6f ? 0.5f * (a - c) / denom : 0.f;
            const double f = ((double) k + juce::jlimit (-0.5f, 0.5f, delta)) * sr / fftSize;

            if (numCands < 8)
                cands[numCands++] = { prom, f };
        }
    }

    persist.swap (persistNew);

    std::copy (level.begin(), level.end(), hist.begin() + (size_t) (frameCount % 4) * (size_t) (half + 1));
    histFilled = juce::jmin (histFilled + 1, 4);
    ++frameCount;

    std::sort (cands, cands + numCands, [] (const Cand& x, const Cand& y) { return x.prom > y.prom; });

    // --- deal with what we found --------------------------------------------
    const float maxDepth = currentMaxDepth();
    int newCount = 0;
    int taken = 0;
    double takenFreq[3] {};

    for (int c = 0; c < numCands && taken < 3; ++c)
    {
        const double f = cands[c].freq;

        bool duplicate = false;
        for (int t = 0; t < taken; ++t)
            if (std::abs (f / takenFreq[t] - 1.0) < kCoverRatio)
                duplicate = true;
        if (duplicate)
            continue;

        takenFreq[taken++] = f;

        const int idx = findCovering (f);

        if (idx >= 0)
        {
            auto& n = notches[(size_t) idx];

            // A latched notch the user switched off holds its place: don't re-learn it.
            if (n.state == Latched && ! info[(size_t) idx].engaged.load())
                continue;

            // Still ringing despite the notch: deepen it a step, up to the limit.
            n.sinceNeeded = 0;
            settleFrames = 35;

            if ((int) (frameCount - n.lastDeepen) > 8 && n.target + kDepthStepDb <= maxDepth + 0.01f)
            {
                n.target += kDepthStepDb;
                n.lastDeepen = (int) frameCount;
            }
        }
        else
        {
            int slot = -1;
            for (int i = kLatched; i < kSlots; ++i)
                if (notches[(size_t) i].state == Free) { slot = i; break; }

            if (slot < 0)
            {
                // Out of floating notches and still ringing: pull the gain down, quickly.
                gainDb = juce::jmax (-6.f, gainDb - 3.f);
                settleFrames = 60;
                continue;
            }

            auto& n = notches[(size_t) slot];
            n = Notch();
            n.state = Floating;
            n.freq = f;
            n.target = juce::jmin (kStartDepthDb, maxDepth);
            n.lastDeepen = (int) frameCount;
            ++newCount;
        }
    }

    // --- ring-out gain: notch first, back off, then creep up again --------------
    if (newCount > 0)
    {
        gainDb = juce::jmax (0.f, gainDb - (newCount >= 2 ? 2.f : 1.f));
        settleFrames = 35;   // about 1.5 s of calm before any creeping resumes
    }

    if (settleFrames > 0)
    {
        --settleFrames;
        creepFrames = 0;
    }
    else if (apvts.getRawParameterValue ("ringout")->load() > 0.5f && gainDb < kGainCeilingDb)
    {
        if (++creepFrames >= 12)   // about every half second
        {
            creepFrames = 0;
            gainDb = juce::jmin (kGainCeilingDb, gainDb + kGainStepDb);
        }
    }

    // --- housekeeping: who is ready to lock, who can let go -----------------------
    const int readyFrames   = (int) (kReadyToLockSec * sr / hop);
    const int releaseFrames = (int) (kReleaseSec * sr / hop);

    for (int i = 0; i < kSlots; ++i)
    {
        auto& n = notches[(size_t) i];

        if (n.state == Free)
            continue;

        ++n.sinceNeeded;

        if (n.state == Floating && ! n.releasing)
        {
            if (n.sinceNeeded >= readyFrames && ! n.dismissed && pendingSlot.load() == -1)
                pendingSlot.store (i);

            if (n.sinceNeeded >= releaseFrames && pendingSlot.load() != i)
                n.releasing = true;
        }
    }
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::handleCommands()
{
    const int cmd = uiCommand.exchange (0);

    if (cmd == 1)
    {
        resetAll();
    }
    else if (cmd == 2)
    {
        lockPending();
    }
    else if (cmd == 3)
    {
        const int p = pendingSlot.load();
        if (p >= 0 && p < kSlots)
            notches[(size_t) p].dismissed = true;
        pendingSlot.store (-1);
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

void FeedbackDestroyerAudioProcessor::resetAll()
{
    for (int i = 0; i < kSlots; ++i)
    {
        notches[(size_t) i] = Notch();
        info[(size_t) i].engaged.store (true);
    }

    std::fill (persist.begin(), persist.end(), 0);
    pendingSlot.store (-1);
    gainDb = 0.f;
    settleFrames = creepFrames = 0;
}

void FeedbackDestroyerAudioProcessor::lockPending()
{
    const int p = pendingSlot.load();
    pendingSlot.store (-1);

    if (p < kLatched || p >= kSlots || notches[(size_t) p].state != Floating)
        return;

    int dst = -1;
    for (int i = 0; i < kLatched; ++i)
        if (notches[(size_t) i].state == Free) { dst = i; break; }

    if (dst < 0)
    {
        // All 12 latched slots are used: replace the one that has gone longest without work.
        dst = 0;
        for (int i = 1; i < kLatched; ++i)
            if (notches[(size_t) i].sinceNeeded > notches[(size_t) dst].sinceNeeded)
                dst = i;
    }

    Notch moved = notches[(size_t) p];
    moved.state = Latched;
    moved.releasing = false;
    moved.dismissed = false;
    moved.sinceNeeded = 0;

    notches[(size_t) dst] = moved;
    info[(size_t) dst].engaged.store (true);
    notches[(size_t) p] = Notch();
}

void FeedbackDestroyerAudioProcessor::applyRestore()
{
    for (int i = 0; i < kLatched; ++i)
    {
        notches[(size_t) i] = Notch();

        if (savedPending.valid[i])
        {
            auto& n = notches[(size_t) i];
            n.state = Latched;
            n.freq = savedPending.freq[i];
            n.target = savedPending.depth[i];
            n.cur = savedPending.depth[i];
            info[(size_t) i].engaged.store (savedPending.on[i]);
        }
    }
}

void FeedbackDestroyerAudioProcessor::publishInfo()
{
    for (int i = 0; i < kSlots; ++i)
    {
        const auto& n = notches[(size_t) i];
        info[(size_t) i].state.store (n.state);
        info[(size_t) i].freq.store ((float) n.freq);
        info[(size_t) i].depth.store (n.cur);
    }

    gainDbOut.store (gainDb);
}

//==============================================================================
void FeedbackDestroyerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    juce::ValueTree latched ("LATCHED");
    for (int i = 0; i < kLatched; ++i)
    {
        if (info[(size_t) i].state.load() != Latched)
            continue;

        juce::ValueTree n ("N");
        n.setProperty ("slot", i, nullptr);
        n.setProperty ("f", info[(size_t) i].freq.load(), nullptr);
        n.setProperty ("d", juce::jmax (info[(size_t) i].depth.load(), 6.f), nullptr);
        n.setProperty ("on", info[(size_t) i].engaged.load(), nullptr);
        latched.appendChild (n, nullptr);
    }
    state.appendChild (latched, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void FeedbackDestroyerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    auto latched = tree.getChildWithName ("LATCHED");

    {
        const juce::SpinLock::ScopedLockType lock (restoreLock);
        savedPending = SavedLatched();

        for (int c = 0; c < latched.getNumChildren(); ++c)
        {
            auto n = latched.getChild (c);
            const int slot = n.getProperty ("slot", -1);

            if (slot >= 0 && slot < kLatched)
            {
                savedPending.valid[slot] = true;
                savedPending.freq[slot]  = (float) (double) n.getProperty ("f", 1000.0);
                savedPending.depth[slot] = (float) (double) n.getProperty ("d", 6.0);
                savedPending.on[slot]    = (bool) n.getProperty ("on", true);
            }
        }

        restoreFlag.store (true);
    }

    if (latched.isValid())
        tree.removeChild (latched, nullptr);

    apvts.replaceState (tree);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FeedbackDestroyerAudioProcessor();
}
