// Stand-alone test for the Capture module (no JUCE needed).
// Simulates a ring-out: room noise, a steady fan-heater tone at 820 Hz,
// two feedback tones that grow until the "detector" notches them, and a
// short vibrato vocal line. Then checks what was written.
//
//   c++ -std=c++17 -O2 -pthread -I../Source tests/capture_test.cpp -o capture_test
//   ./capture_test /tmp/capture_out

#include "Capture/CaptureRecorder.h"
#include <cstdio>
#include <random>

int main (int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : "/tmp/capture_out";
    const double sr = 48000.0, pi = 3.14159265358979;

    sjam::CaptureRecorder cap;
    cap.setOutputFolder (out);
    cap.prepare (sr);

    std::mt19937 rng (1);
    std::normal_distribution<float> noise (0.f, 0.003f);   // about -50 dBFS room noise

    auto db = [] (double d) { return std::pow (10.0, d / 20.0); };

    double ph1 = 0, ph2 = 0, ph3 = 0, ph4 = 0;
    bool fb1Notched = false, fb2Notched = false, fb2Deepened = false;

    cap.runStarted();

    const int total = (int) (16.0 * sr);
    for (int i = 0; i < total; ++i)
    {
        const double t = i / sr;
        double s = noise (rng);

        // fan heater: steady, never notched
        s += db (-32) * std::sin (ph1 += 2 * pi * 820 / sr);

        // feedback 1: 2500 Hz from t=3 s, grows 20 dB/s from -70 dB, notched at -20 dB
        if (t > 3.0 && ! fb1Notched)
        {
            const double lv = -70 + 20 * (t - 3.0);
            s += db (lv) * std::sin (ph2 += 2 * pi * 2500 / sr);
            if (lv >= -20) { cap.pushSample ((float) s); cap.mark (sjam::CaptureRecorder::NewNotch, 2500.0); fb1Notched = true; continue; }
        }

        // feedback 2: 630 Hz from t=8 s, notched, then rings again and gets deepened
        if (t > 8.0)
        {
            double lv = -70 + 22 * (t - 8.0);
            if (fb2Notched) lv -= 30;
            if (! fb2Deepened || t < 12.0)
                s += db (lv) * std::sin (ph3 += 2 * pi * 630 / sr);
            if (! fb2Notched && lv >= -20) { cap.pushSample ((float) s); cap.mark (sjam::CaptureRecorder::NewNotch, 630.0); fb2Notched = true; continue; }
            if (fb2Notched && ! fb2Deepened && lv >= -22) { cap.pushSample ((float) s); cap.mark (sjam::CaptureRecorder::Deepen, 630.0); fb2Deepened = true; continue; }
        }

        // vocal-ish line with vibrato around 300 Hz, 12-14 s
        if (t > 12.0 && t < 14.0)
            s += db (-26) * std::sin (ph4 += 2 * pi * (300 + 8 * std::sin (2 * pi * 5.5 * t)) / sr);

        cap.pushSample ((float) s);
    }

    cap.runEnded (1);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    std::printf ("examples written: %d, error: %d\n", cap.examplesThisRun(), (int) cap.hadError());
    return 0;
}
