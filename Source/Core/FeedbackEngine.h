#pragma once
#include "Biquad.h"
#include "FeedbackDetector.h"
#include "FrequencyShifter.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#ifdef FD_DEBUG
#include <cstdio>
#endif

namespace fd
{
struct EngineParams
{
    float sensitivity = 0.5f;  // 0..1
    int   speed       = 1;     // 0 slow, 1 medium, 2 fast
    float maxDepthDb  = 18.f;  // deepest cut a band may reach
    float q           = 40.f;  // notch Q (higher = narrower)
    int   numBands    = 24;    // 1..24 usable bands
    float releaseSec  = 30.f;  // how long an unhit band holds before decaying
    bool  bypass      = false;
    float shiftHz     = 0.f;   // frequency shift applied after the notches (0 = off)
    bool  notchesEnabled = true; // false = shifter only (used by tests)
};

// Lock-free snapshot of a band for the UI / state saving.
struct BandDisplay
{
    std::atomic<float> freq   { 0.f };
    std::atomic<float> depth  { 0.f };
    std::atomic<bool>  locked { false };
};

// Detector + 24-band dynamic notch bank. Pure C++ (no JUCE) so it can be unit tested.
// All methods except prepare() are real-time safe.
class FeedbackEngine
{
public:
    static constexpr int kMaxBands    = 24;
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, int maxBlockSize)
    {
        sr_ = sampleRate;
        maxBlock_ = std::max (1, maxBlockSize);
        mono_.assign ((size_t) maxBlock_, 0.f);
        detector_.prepare (sampleRate);
        shifter_.prepare (sampleRate);
        reset();
    }

    void reset()
    {
        detector_.reset();
        shifter_.reset();
        for (auto& b : bands_) b = Band{};
        clock_ = 0;
        publish();
    }

    void setParams (const EngineParams& p)
    {
        params_ = p;
        params_.numBands = std::clamp (p.numBands, 1, kMaxBands);
        DetectorParams dp;
        dp.sensitivity = p.sensitivity;
        dp.speed = p.speed;
        detector_.setParams (dp);
    }

    // Audio-thread API. Processes in place; first numCh channels (max 2).
    void process (float* const* ch, int numCh, int n)
    {
        numCh = std::min (numCh, kMaxChannels);
        for (int done = 0; done < n;)
        {
            const int len = std::min (n - done, maxBlock_);
            float* ptrs[kMaxChannels] {};
            for (int c = 0; c < numCh; ++c) ptrs[c] = ch[c] + done;
            processChunk (ptrs, numCh, len);
            done += len;
        }
    }

    void clearAll()
    {
        for (auto& b : bands_) { b.active = false; b.locked = false; b.depth = 0.f; }
    }

    void lockAll()
    {
        for (auto& b : bands_) if (b.active) b.locked = true;
    }

    void loadLocked (const float* freqs, const float* depths, int n)
    {
        clearAll();
        for (int i = 0; i < std::min (n, kMaxBands); ++i)
        {
            Band& b = bands_[(size_t) i];
            b.active = true; b.locked = true;
            b.freq = freqs[i]; b.depth = depths[i];
            b.lastHit = b.lastChange = clock_;
        }
    }

    void getBandDisplay (int i, float& freq, float& depth, bool& locked) const
    {
        freq   = display_[(size_t) i].freq.load (std::memory_order_relaxed);
        depth  = display_[(size_t) i].depth.load (std::memory_order_relaxed);
        locked = display_[(size_t) i].locked.load (std::memory_order_relaxed);
    }

    int activeBandCount() const
    {
        int n = 0;
        for (const auto& b : bands_) n += b.active;
        return n;
    }

    // Test/inspection helpers (audio-thread or single-threaded use only).
    float bandFreq (int i) const  { return bands_[(size_t) i].freq; }
    float bandDepth (int i) const { return bands_[(size_t) i].depth; }
    bool  bandActive (int i) const { return bands_[(size_t) i].active; }

private:
    struct Band
    {
        bool  active = false, locked = false;
        float freq = 0.f, depth = 0.f;
        std::int64_t lastHit = 0, lastChange = 0;
        Coeffs cur;                          // coefficients currently in use
        double z1[kMaxChannels] {}, z2[kMaxChannels] {};
        // cache of what 'cur' was designed from, to skip redundant trig
        float dFreq = -1.f, dDepth = -1.f, dQ = -1.f;
        Coeffs designed;
    };

    static constexpr float kInitialDepthDb = 6.f;
    static constexpr float kStepDb         = 3.f;
    static constexpr float kDecayDbPerSec  = 3.f;

    void processChunk (float* const* ptrs, int numCh, int n)
    {
        if (! params_.bypass && params_.notchesEnabled)
        {
            for (int i = 0; i < n; ++i)
            {
                float s = 0.f;
                for (int c = 0; c < numCh; ++c) s += ptrs[c][i];
                mono_[(size_t) i] = s / (float) std::max (1, numCh);
            }

            Detection det[FeedbackDetector::kMaxDetections];
            const int nd = detector_.push (mono_.data(), n, det, FeedbackDetector::kMaxDetections);
            for (int i = 0; i < nd; ++i) handleDetection (det[i]);
            tick (n);
        }

        for (auto& b : bands_)
        {
            const bool wantActive = b.active && ! params_.bypass;
            Coeffs target;
            if (wantActive)
            {
                if (b.dFreq != b.freq || b.dDepth != b.depth || b.dQ != params_.q)
                {
                    b.designed = designPeaking (sr_, b.freq, params_.q, -b.depth);
                    b.dFreq = b.freq; b.dDepth = b.depth; b.dQ = params_.q;
                }
                target = b.designed;
            }
            if (! wantActive && b.cur.isIdentity()) continue;
            runBand (b, ptrs, numCh, n, target);
        }

        // Shifter goes last: the notches are placed at frequencies seen at the input, so they
        // must act before the shift moves the signal.
        shifter_.process (ptrs, numCh, n, params_.shiftHz, ! params_.bypass);

        clock_ += n;
        publish();
    }

    void handleDetection (const Detection& d)
    {
#ifdef FD_DEBUG
        std::printf ("  [t=%.2fs] det %.1f Hz level %.1f dB growth %.1f pnpr %.1f\n", (double) clock_ / sr_, d.freqHz, d.levelDb, d.growthDb, d.pnpr);
#endif
        const double binHz = detector_.binHz();
        const float tol = (float) std::max (2.0 * binHz, 0.02 * (double) d.freqHz);

        int match = -1;
        float bestDist = 1e9f;
        for (int i = 0; i < params_.numBands; ++i)
        {
            const Band& b = bands_[(size_t) i];
            if (! b.active) continue;
            const float dist = std::abs (b.freq - d.freqHz);
            if (dist <= tol && dist < bestDist) { bestDist = dist; match = i; }
        }

        const std::int64_t settle = FeedbackDetector::kFftSize + FeedbackDetector::kHop;

        if (match >= 0)
        {
            Band& b = bands_[(size_t) match];
            b.lastHit = clock_;
            if (! b.locked && clock_ - b.lastChange >= settle && b.depth < params_.maxDepthDb)
            {
                b.depth = std::min (params_.maxDepthDb, b.depth + kStepDb);
                b.freq += 0.25f * (d.freqHz - b.freq); // small nudge toward the interpolated peak
                b.lastChange = clock_;
            }
            return;
        }

        // New frequency: take a free band, else steal the shallowest/oldest unlocked one.
        int slot = -1;
        for (int i = 0; i < params_.numBands; ++i)
            if (! bands_[(size_t) i].active) { slot = i; break; }

        if (slot < 0)
        {
            for (int i = 0; i < params_.numBands; ++i)
            {
                const Band& b = bands_[(size_t) i];
                if (b.locked) continue;
                if (slot < 0) { slot = i; continue; }
                const Band& s = bands_[(size_t) slot];
                if (b.depth < s.depth || (b.depth == s.depth && b.lastHit < s.lastHit)) slot = i;
            }
        }
        if (slot < 0) return; // everything locked

        Band& b = bands_[(size_t) slot];
        b.active = true; b.locked = false;
        b.freq = d.freqHz;
        b.depth = std::min (kInitialDepthDb, params_.maxDepthDb);
        b.lastHit = b.lastChange = clock_;
    }

    void tick (int n)
    {
        const std::int64_t hold = (std::int64_t) ((double) params_.releaseSec * sr_);
        const float decay = kDecayDbPerSec * (float) n / (float) sr_;

        for (int i = 0; i < kMaxBands; ++i)
        {
            Band& b = bands_[(size_t) i];
            if (! b.active) continue;

            if (i >= params_.numBands) { b.active = false; b.locked = false; b.depth = 0.f; continue; }

            b.depth = std::min (b.depth, params_.maxDepthDb);
            if (! b.locked && clock_ - b.lastHit > hold)
            {
                b.depth -= decay;
                if (b.depth < 0.3f) { b.active = false; b.depth = 0.f; }
            }
        }
    }

    // TDF-II biquad with the coefficients ramped linearly across the block. The stable
    // region of (a1,a2) is convex, so interpolating between two stable sets stays stable.
    void runBand (Band& b, float* const* ptrs, int numCh, int n, const Coeffs& target)
    {
        const Coeffs c0 = b.cur;
        const double inv = 1.0 / (double) n;
        const double d0 = (target.b0 - c0.b0) * inv, d1 = (target.b1 - c0.b1) * inv,
                     d2 = (target.b2 - c0.b2) * inv, e1 = (target.a1 - c0.a1) * inv,
                     e2 = (target.a2 - c0.a2) * inv;

        for (int ch = 0; ch < numCh; ++ch)
        {
            double b0 = c0.b0, b1 = c0.b1, b2 = c0.b2, a1 = c0.a1, a2 = c0.a2;
            double z1 = b.z1[ch], z2 = b.z2[ch];
            float* x = ptrs[ch];
            for (int i = 0; i < n; ++i)
            {
                b0 += d0; b1 += d1; b2 += d2; a1 += e1; a2 += e2;
                const double in = x[i];
                const double y = b0 * in + z1;
                z1 = b1 * in - a1 * y + z2;
                z2 = b2 * in - a2 * y;
                x[i] = (float) y;
            }
            b.z1[ch] = z1; b.z2[ch] = z2;
        }
        b.cur = target;
    }

    void publish()
    {
        for (int i = 0; i < kMaxBands; ++i)
        {
            const Band& b = bands_[(size_t) i];
            display_[(size_t) i].freq.store (b.active ? b.freq : 0.f, std::memory_order_relaxed);
            display_[(size_t) i].depth.store (b.active ? b.depth : 0.f, std::memory_order_relaxed);
            display_[(size_t) i].locked.store (b.active && b.locked, std::memory_order_relaxed);
        }
    }

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    std::int64_t clock_ = 0;
    EngineParams params_;
    FeedbackDetector detector_;
    FrequencyShifter shifter_;
    std::vector<float> mono_;
    std::array<Band, kMaxBands> bands_;
    std::array<BandDisplay, kMaxBands> display_;
};
} // namespace fd
