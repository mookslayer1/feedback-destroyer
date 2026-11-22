// Off-plugin tests for the DSP core. No JUCE needed:
//   c++ -std=c++20 -O2 -I Source/Core tests/core_tests.cpp -o core_tests && ./core_tests
#include "FeedbackEngine.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace fd;

static int failures = 0;
#define CHECK(cond, ...) do { if (! (cond)) { ++failures; std::printf ("  FAIL: "); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

// A closed acoustic loop: out -> (delay + resonant room/mic response + soft clip) -> in.
// Peak loop gain is just over unity at 3 kHz, so noise slowly rings up into feedback.
struct LoopSim
{
    static constexpr double fs = 48000.0;
    static constexpr int delay = 960;      // 20 ms; 60 whole cycles at 3 kHz
    static constexpr int block = 64;

    FeedbackEngine engine;
    std::vector<float> ring = std::vector<float> (delay, 0.f);
    int w = 0;
    Coeffs room = designPeaking (fs, 3000.0, 8.0, 6.27);
    double rz1 = 0, rz2 = 0;
    std::mt19937 rng { 1234 };
    std::normal_distribution<float> noise { 0.f, 1.f };
    double sampleClock = 0;
    double firstBandTime = -1;

    explicit LoopSim (const EngineParams& p)
    {
        engine.prepare (fs, block);
        engine.setParams (p);
    }

    explicit LoopSim (bool bypass) : LoopSim (makeParams (bypass)) {}

    static EngineParams makeParams (bool bypass)
    {
        EngineParams p;
        p.bypass = bypass;
        p.releaseSec = 600.f; // don't let it release inside the test
        return p;
    }

    void setRoomGainDb (double g) { room = designPeaking (fs, 3000.0, 8.0, g); }

    float runSeconds (double seconds, float* peakLastSecond)
    {
        const int total = (int) (seconds * fs);
        float lastPeak = 0.f;
        float buf[block];
        for (int done = 0; done < total; done += block)
        {
            for (int i = 0; i < block; ++i)
                buf[i] = 0.001f * noise (rng) + ring[(size_t) ((w + i) % delay)];

            float* ch[1] = { buf };
            engine.process (ch, 1, block);

            for (int i = 0; i < block; ++i)
            {
                const double x = buf[i] * 0.5;
                const double y = room.b0 * x + rz1;
                rz1 = room.b1 * x - room.a1 * y + rz2;
                rz2 = room.b2 * x - room.a2 * y;
                ring[(size_t) ((w + i) % delay)] = (float) std::tanh (y);
                if (done >= total - (int) fs) lastPeak = std::max (lastPeak, std::abs (buf[i]));
            }
            w = (w + block) % delay;
            sampleClock += block;
            if (firstBandTime < 0 && engine.activeBandCount() > 0) firstBandTime = sampleClock / fs;
        }
        if (peakLastSecond) *peakLastSecond = lastPeak;
        return lastPeak;
    }
};

static void testFeedbackIsSuppressed()
{
    std::printf ("closed-loop feedback\n");

    float peakOff = 0.f;
    { LoopSim sim (true);  sim.runSeconds (30.0, &peakOff); }
    std::printf ("  bypassed: peak over last second = %.3f\n", peakOff);
    CHECK (peakOff > 0.3f, "the simulated loop should run away into feedback when bypassed");

    LoopSim sim (false);
    float peakOn = 0.f;
    sim.runSeconds (30.0, &peakOn);
    std::printf ("  engine:   peak over last second = %.4f, first notch at %.1f s, %d bands\n",
                 peakOn, sim.firstBandTime, sim.engine.activeBandCount());
    for (int i = 0; i < FeedbackEngine::kMaxBands; ++i)
        if (sim.engine.bandActive (i))
            std::printf ("    band %2d: %.1f Hz, -%.1f dB\n", i, sim.engine.bandFreq (i), sim.engine.bandDepth (i));

    CHECK (peakOn < 0.05f, "feedback should be tamed (peak %.4f)", peakOn);
    bool near3k = false;
    for (int i = 0; i < FeedbackEngine::kMaxBands; ++i)
        if (sim.engine.bandActive (i) && std::abs (sim.engine.bandFreq (i) - 3000.f) < 30.f) near3k = true;
    CHECK (near3k, "a notch should sit near 3 kHz");
}

static void testMicMovesCloser()
{
    std::printf ("mic moved closer mid-show (needs deeper cut)\n");
    LoopSim sim (false);
    float peak = 0.f;
    sim.runSeconds (10.0, &peak);
    const float before = sim.engine.bandDepth (0);
    sim.setRoomGainDb (12.0);               // loop gain at 3 kHz jumps by ~6 dB
    sim.runSeconds (30.0, &peak);
    std::printf ("  depth before %.1f dB, after %.1f dB, final peak %.4f, bands %d\n",
                 before, sim.engine.bandDepth (0), peak, sim.engine.activeBandCount());
    CHECK (peak < 0.05f, "feedback should be tamed after the loop gain increase (peak %.4f)", peak);
    CHECK (sim.engine.bandDepth (0) > before, "the notch should have deepened");
}

// Largest loop gain (dB of room resonance peak) that stays stable for 40 s, found by bisection.
static double stabilityLimitDb (const EngineParams& p)
{
    double lo = 4.0, hi = 30.0;          // lo: stable, hi: runaway
    for (int i = 0; i < 7; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        LoopSim sim (p);
        sim.setRoomGainDb (mid);
        float peak = 0.f;
        sim.runSeconds (40.0, &peak);
        (peak > 0.2f ? hi : lo) = mid;
    }
    return lo;
}

static void testShifterDirectionAndGain()
{
    std::printf ("frequency shifter\n");

    // 1) direction and image rejection
    {
        const double fs = 48000.0;
        FeedbackEngine e;
        e.prepare (fs, 256);
        EngineParams p; p.shiftHz = 3.f; p.notchesEnabled = false;
        e.setParams (p);
        const int N = (int) (4 * fs);
        std::vector<float> buf ((size_t) N);
        for (int n = 0; n < N; ++n) buf[(size_t) n] = 0.5f * (float) std::sin (2 * 3.14159265358979 * 1000.0 * n / fs);
        for (int i = 0; i < N; i += 256)
        {
            float* ch[1] = { buf.data() + i };
            e.process (ch, 1, std::min (256, N - i));
        }
        auto mag = [&] (double f)
        {
            double re = 0, im = 0;
            for (int n = N / 2; n < N; ++n) { const double a = 2 * 3.14159265358979 * f * n / fs; re += buf[(size_t) n] * std::cos (a); im -= buf[(size_t) n] * std::sin (a); }
            return std::sqrt (re * re + im * im) / (N / 2) * 2;
        };
        const double up = mag (1003.0), down = mag (997.0);
        std::printf ("  1 kHz +3 Hz -> up %.4f, image %.4f (%.0f dB down)\n", up, down, 20 * std::log10 (down / up));
        CHECK (up > 0.45 && down < 0.02, "shifter should move 1 kHz up to 1003 Hz with a clean image rejection");
    }

    // 2) how much more loop gain the simulated room tolerates
    EngineParams off = LoopSim::makeParams (true);
    EngineParams shift = LoopSim::makeParams (false); shift.notchesEnabled = false; shift.shiftHz = 2.f;
    EngineParams notch = LoopSim::makeParams (false);
    EngineParams both = notch; both.shiftHz = 2.f;

    const double gOff = stabilityLimitDb (off), gShift = stabilityLimitDb (shift),
                 gNotch = stabilityLimitDb (notch), gBoth = stabilityLimitDb (both);
    std::printf ("  simulated gain before feedback (dB above bypassed limit):\n"
                 "    shifter only (2 Hz): %+.1f dB\n    notches only:        %+.1f dB\n    both:                %+.1f dB\n",
                 gShift - gOff, gNotch - gOff, gBoth - gOff);
    CHECK (gShift - gOff > 0.5, "the shifter alone should raise the stable gain");
    CHECK (gBoth >= gNotch - 0.5, "adding the shifter must not make the notch result worse");
}

static void testMusicDoesNotTrigger()
{
    std::printf ("steady harmonic music (false-positive check)\n");
    const double fs = 48000.0;
    FeedbackEngine e;
    e.prepare (fs, 512);
    e.setParams (EngineParams{});

    std::mt19937 rng (7);
    std::normal_distribution<float> noise (0.f, 1.f);
    std::vector<float> buf (512);

    double t = 0;
    for (int blk = 0; blk < (int) (30 * fs / 512); ++blk)
    {
        for (int i = 0; i < 512; ++i, t += 1.0 / fs)
        {
            double s = 0;
            for (int h = 1; h <= 20; ++h)                         // 220 Hz saw-ish stack
                s += std::sin (2 * 3.14159265358979 * 220.0 * h * t) / h;
            const double env = std::min (1.0, t / 0.5);           // 0.5 s fade-in, then steady
            buf[(size_t) i] = (float) (0.2 * env * s) + 0.003f * noise (rng);
        }
        float* ch[1] = { buf.data() };
        e.process (ch, 1, 512);
    }
    std::printf ("  active bands after 30 s: %d\n", e.activeBandCount());
    CHECK (e.activeBandCount() == 0, "harmonic music should not allocate notches");
}

static void testLockSurvives()
{
    std::printf ("locked bands\n");
    FeedbackEngine e;
    e.prepare (48000.0, 256);
    EngineParams p; p.releaseSec = 0.01f;
    e.setParams (p);
    const float f[2] = { 1000.f, 2500.f }, d[2] = { 12.f, 9.f };
    e.loadLocked (f, d, 2);
    std::vector<float> buf (256, 0.f);
    float* ch[1] = { buf.data() };
    for (int i = 0; i < 2000; ++i) e.process (ch, 1, 256);
    CHECK (e.activeBandCount() == 2, "locked bands must not release (got %d)", e.activeBandCount());
}

int main()
{
    testFeedbackIsSuppressed();
    testMicMovesCloser();
    testShifterDirectionAndGain();
    testMusicDoesNotTrigger();
    testLockSurvives();
    std::printf (failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
