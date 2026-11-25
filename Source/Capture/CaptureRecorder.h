#pragma once
//==============================================================================
//  Freq'd Out - Capture module                                  SJAM Audio
//
//  A test-only recorder that collects training examples for teaching the
//  plugin to spot feedback earlier. It does NOT change the sound and makes no
//  decisions: it only listens and writes files.
//
//  What it writes, per ring-out run, into
//      ~/Music/SJAM Audio/Freqd Out Captures/<date>_<time>/
//
//    audio.wav    the exact signal the detector listened to (mono, 24-bit),
//                 so the examples can be re-analysed later in any way we like
//    events.csv   one line per example: when, which frequency, and its label
//                   new_notch / deepen / merge  -> label 1 (it WAS feedback)
//                   no_feedback                 -> label 0 (a peak that never
//                                                  needed a notch: voice, fan
//                                                  heater, room tone...)
//    frames.csv   for every example, the 2.5 s leading up to it, frame by
//                 frame: level, how far the peak stands out, how fast it is
//                 growing, how steady its pitch is, and how far it sits above
//                 the room's normal level at that frequency
//    summary.txt  settings and counts for the run
//
//  The plugin labels its own examples: when it places or deepens a notch, the
//  seconds before that moment are "the start of feedback". Peaks that stood out
//  but never got a notch within 3 s become "not feedback".
//
//  Real-time safety: the audio thread only copies samples into a lock-free
//  ring buffer and posts small "mark" messages. All analysis and file writing
//  happens on a background thread. No JUCE dependency, so it can be tested on
//  its own (see tests/capture_test.cpp).
//==============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace sjam
{

//------------------------------------------------------------------------------
// Single-producer / single-consumer lock-free ring buffer.
template <typename T>
class SpscRing
{
public:
    void allocate (size_t capacityPow2)
    {
        buf.assign (capacityPow2, T {});
        mask = capacityPow2 - 1;
        head.store (0);
        tail.store (0);
    }

    bool push (const T& v) noexcept
    {
        const size_t h = head.load (std::memory_order_relaxed);
        const size_t t = tail.load (std::memory_order_acquire);
        if (h - t >= buf.size())
            return false;
        buf[h & mask] = v;
        head.store (h + 1, std::memory_order_release);
        return true;
    }

    size_t pop (T* out, size_t maxItems) noexcept
    {
        const size_t t = tail.load (std::memory_order_relaxed);
        const size_t h = head.load (std::memory_order_acquire);
        const size_t n = std::min (maxItems, h - t);
        for (size_t i = 0; i < n; ++i)
            out[i] = buf[(t + i) & mask];
        tail.store (t + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<T> buf;
    size_t mask = 0;
    std::atomic<size_t> head { 0 }, tail { 0 };
};

//------------------------------------------------------------------------------
class CaptureRecorder
{
public:
    static constexpr const char* kVersion = "capture-1.0";
    static constexpr double kPi = 3.14159265358979323846;

    enum MarkKind
    {
        NewNotch = 1,   // a new notch was placed at freq
        Deepen   = 2,   // an existing notch at freq was cut deeper (still feeding back)
        Merge    = 3,   // freq was folded into a neighbouring notch
        RunStart = 10,
        RunEnd   = 11
    };

    CaptureRecorder() = default;
    ~CaptureRecorder() { stopThread(); }

    // Optional. Call before prepare(). Empty = ~/Music/SJAM Audio/Freqd Out Captures
    void setOutputFolder (const std::string& folder) { baseFolder = folder; }

    // Call from prepareToPlay (never from the audio thread).
    void prepare (double sampleRate)
    {
        stopThread();

        sr = sampleRate > 0 ? sampleRate : 48000.0;
        fftSize = 1;
        while (fftSize < (int) (sr * 0.17))
            fftSize <<= 1;
        hop = fftSize / 8;
        hopSec = (double) hop / sr;
        bins = fftSize / 2 + 1;

        leadFrames     = (int) std::ceil (2.5 / hopSec);
        waitFrames     = (int) std::ceil (3.0 / hopSec);
        scanEvery      = std::max (1, (int) std::round (0.25 / hopSec));
        historyFrames  = leadFrames + waitFrames + 16;

        samples.allocate (1u << 21);   // about 40 s of audio at 48 kHz
        marks.allocate (1024);
        audioIndex = 0;
        droppedSamples.store (0);
        droppedMarks.store (0);

        setupFft();
        ring.assign ((size_t) fftSize, 0.f);
        history.assign ((size_t) historyFrames * (size_t) bins, -120.f);
        broadband.assign ((size_t) historyFrames, -120.f);
        baseline.assign ((size_t) bins, -120.f);
        baseHist.assign ((size_t) historyFrames * (size_t) bins, -120.f);

        consumed = 0;
        runOpen = false;

        quit.store (false);
        worker = std::thread ([this] { threadLoop(); });
    }

    //==========================================================================
    // Audio thread. Feed the same mono signal the detector analyses.
    void pushSample (float s) noexcept
    {
        if (! samples.push (s))
            droppedSamples.fetch_add (1, std::memory_order_relaxed);
        ++audioIndex;
    }

    // Audio thread. Call right after the notch decision is made.
    void mark (int kind, double freqHz, float extra = 0.f) noexcept
    {
        Mark m { kind, freqHz, extra, audioIndex };
        if (! marks.push (m))
            droppedMarks.fetch_add (1, std::memory_order_relaxed);
    }

    void runStarted() noexcept         { mark (RunStart, 0.0); }
    void runEnded (int reason) noexcept { mark (RunEnd, 0.0, (float) reason); }

    //==========================================================================
    // Safe to read from anywhere (e.g. to show "Capturing" in the UI).
    bool isCapturing() const noexcept { return capturing.load(); }
    int  examplesThisRun() const noexcept { return examples.load(); }
    bool hadError() const noexcept { return error.load(); }

private:
    struct Mark
    {
        int kind = 0;
        double freq = 0.0;
        float extra = 0.f;
        uint64_t index = 0;
    };

    struct Pending
    {
        double freq;
        long frame;
        double runTime;
        float prom;
    };

    //--------------------------------------------------------------------------
    void stopThread()
    {
        quit.store (true);
        if (worker.joinable())
            worker.join();
        if (runOpen)
            closeRun (-1);
    }

    void threadLoop()
    {
        std::vector<float> tmp (4096);
        bool haveMark = false;
        Mark next;

        auto handleDueMarks = [&] {
            for (;;)
            {
                if (! haveMark)
                    haveMark = marks.pop (&next, 1) == 1;
                if (! haveMark || next.index > consumed)
                    return;
                handleMark (next);
                haveMark = false;
            }
        };

        while (! quit.load())
        {
            const size_t n = samples.pop (tmp.data(), tmp.size());

            for (size_t i = 0; i < n; ++i)
            {
                handleDueMarks();
                processSample (tmp[i]);
                ++consumed;
            }

            handleDueMarks();

            if (n == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        // drain whatever is left so a final RunEnd is not lost
        for (;;)
        {
            const size_t n = samples.pop (tmp.data(), tmp.size());
            for (size_t i = 0; i < n; ++i)
            {
                handleDueMarks();
                processSample (tmp[i]);
                ++consumed;
            }
            handleDueMarks();
            if (n == 0)
                break;
        }
    }

    //--------------------------------------------------------------------------
    void processSample (float s)
    {
        if (! runOpen)
            return;

        writeWavSample (s);

        ring[(size_t) ringPos] = s;
        ringPos = (ringPos + 1) & (fftSize - 1);
        hopEnergy += (double) s * s;
        ++runSamples;

        if (++hopCount >= hop)
        {
            hopCount = 0;
            if (runSamples >= (uint64_t) fftSize)
                analyseFrame();
            hopEnergy = 0.0;
        }
    }

    void analyseFrame()
    {
        // windowed FFT of the last fftSize samples
        for (int j = 0; j < fftSize; ++j)
            fftBuf[(size_t) j] = { ring[(size_t) ((ringPos + j) & (fftSize - 1))] * window[(size_t) j], 0.f };
        fft (fftBuf);

        const float norm = 4.f / (float) fftSize;   // full-scale sine reads about 0 dB
        float* row = frameRow (frame);
        for (int k = 0; k < bins; ++k)
        {
            const float mag = std::abs (fftBuf[(size_t) k]) * norm;
            row[k] = mag > 1.0e-6f ? 20.f * std::log10 (mag) : -120.f;
        }

        const double rms = std::sqrt (hopEnergy / (double) hop);
        broadband[(size_t) (frame % historyFrames)] = rms > 1.0e-6 ? (float) (20.0 * std::log10 (rms)) : -120.f;

        // slow per-frequency "room level" (about 8 s memory)
        const float a = (float) std::exp (-hopSec / 8.0);
        if (frame == 0)
            std::copy (row, row + bins, baseline.begin());

        // keep the room level as it was *at this frame*, so every example is judged
        // against the same moment in time whether it is labelled now or 3 s later
        std::copy (baseline.begin(), baseline.end(), baseRow (frame));

        for (int k = 0; k < bins; ++k)
            baseline[(size_t) k] = a * baseline[(size_t) k] + (1.f - a) * row[k];

        if (frame % scanEvery == 0)
            scanForNegatives();

        resolvePending (false);
        ++frame;
    }

    // Find peaks that stand out but are not (yet) being notched. Each becomes a
    // "maybe" which turns into a no_feedback example if no notch follows in 3 s.
    void scanForNegatives()
    {
        const float* row = frameRow (frame);
        const int kLo = std::max (16, (int) std::ceil (30.0 * fftSize / sr));
        const int kHi = std::min (bins - 16, (int) std::floor (std::min (40000.0, 0.45 * sr) * fftSize / sr));

        struct P { int k; float prom; };
        P best[3] = { { -1, 0.f }, { -1, 0.f }, { -1, 0.f } };

        for (int k = kLo; k <= kHi; ++k)
        {
            const float lv = row[k];
            if (lv < -70.f)
                continue;
            if (! (lv > row[k - 1] && lv >= row[k + 1] && lv > row[k - 2] && lv >= row[k + 2]))
                continue;

            const float prom = prominence (row, k);
            if (prom < 10.f)
                continue;

            int w = 0;
            for (int i = 1; i < 3; ++i)
                if (best[i].prom < best[w].prom)
                    w = i;
            if (prom > best[w].prom)
                best[w] = { k, prom };
        }

        for (const auto& b : best)
        {
            if (b.k < 0 || pending.size() >= 64)
                continue;

            const double f = interpFreq (row, b.k);
            if (nearAny (notchFreqs, f, 0.04))
                continue;

            bool dup = false;
            for (const auto& p : pending)
                if (std::abs (p.freq / f - 1.0) < 0.04)
                    dup = true;
            if (dup)
                continue;

            pending.push_back ({ f, frame, (double) runSamples / sr, b.prom });
        }
    }

    void resolvePending (bool flushAll)
    {
        for (size_t i = 0; i < pending.size();)
        {
            if (flushAll || frame - pending[i].frame >= waitFrames)
            {
                writeExample (pending[i].frame, pending[i].freq, "no_feedback", 0, pending[i].runTime);
                pending.erase (pending.begin() + (long) i);
            }
            else
            {
                ++i;
            }
        }
    }

    //--------------------------------------------------------------------------
    void handleMark (const Mark& m)
    {
        if (m.kind == RunStart)
        {
            if (runOpen)
                closeRun (-1);
            openRun();
            return;
        }

        if (! runOpen)
            return;

        if (m.kind == RunEnd)
        {
            closeRun ((int) m.extra);
            return;
        }

        const char* kind = m.kind == NewNotch ? "new_notch" : m.kind == Deepen ? "deepen" : "merge";

        // anything we were unsure about near this frequency was in fact feedback
        for (size_t i = 0; i < pending.size();)
        {
            if (std::abs (pending[i].freq / m.freq - 1.0) < 0.04)
                pending.erase (pending.begin() + (long) i);
            else
                ++i;
        }

        if (! nearAny (notchFreqs, m.freq, 0.01))
            notchFreqs.push_back (m.freq);

        if (frame > 0)
            writeExample (frame - 1, m.freq, kind, 1, (double) runSamples / sr);
    }

    //--------------------------------------------------------------------------
    // Writes one example: the leadFrames frames ending at endFrame, tracking
    // the peak nearest freq.
    void writeExample (long endFrame, double freq, const char* kind, int label, double runTime)
    {
        if (! events.is_open())
            return;

        const long first = std::max ({ 0L, endFrame - leadFrames + 1, frame - historyFrames + 1 });
        const int count = (int) (endFrame - first + 1);
        if (count <= 0 || (label == 0 && count < leadFrames / 4))
            return;   // a "not feedback" example needs some history to be useful

        const int id = nextEventId++;
        const double binHz = sr / fftSize;
        const int centre = (int) std::lround (freq / binHz);
        const int reach = std::max (3, (int) std::ceil (freq * 0.015 / binHz));

        std::vector<double> fr ((size_t) count);
        std::vector<float> lv ((size_t) count), pr ((size_t) count), ab ((size_t) count);

        for (int i = 0; i < count; ++i)
        {
            const float* row = frameRow (first + i);
            int kBest = std::clamp (centre, 16, bins - 16);
            for (int k = std::max (16, centre - reach); k <= std::min (bins - 16, centre + reach); ++k)
                if (row[k] > row[kBest])
                    kBest = k;

            fr[(size_t) i] = interpFreq (row, kBest);
            lv[(size_t) i] = row[kBest];
            pr[(size_t) i] = prominence (row, kBest);
            ab[(size_t) i] = row[kBest] - baseRow (first + i)[kBest];
        }

        for (int i = 0; i < count; ++i)
        {
            const int j = std::max (0, i - 3);
            const float slope = i > j ? (lv[(size_t) i] - lv[(size_t) j]) / (float) ((i - j) * hopSec) : 0.f;
            const double drift = i > 0 ? fr[(size_t) i] - fr[(size_t) (i - 1)] : 0.0;
            const double tRel = (double) (first + i - endFrame) * hopSec;

            std::fprintf (framesFile, "%d,%d,%.4f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f\n",
                          id, i - count + 1, tRel, fr[(size_t) i], lv[(size_t) i], pr[(size_t) i],
                          slope, drift, ab[(size_t) i],
                          broadband[(size_t) ((first + i) % historyFrames)]);
        }

        char line[256];
        std::snprintf (line, sizeof (line), "%d,%.3f,%s,%d,%.2f,%d\n", id, runTime, kind, label, freq, count);
        events << line;
        examples.fetch_add (1);
    }

    //--------------------------------------------------------------------------
    void openRun()
    {
        error.store (false);
        namespace fs = std::filesystem;

        std::string base = baseFolder;
        if (base.empty())
        {
            const char* home = std::getenv ("HOME");
            base = std::string (home != nullptr ? home : ".") + "/Music/SJAM Audio/Freqd Out Captures";
        }

        const std::time_t now = std::time (nullptr);
        char stamp[64];
        std::strftime (stamp, sizeof (stamp), "%Y-%m-%d_%H-%M-%S", std::localtime (&now));

        std::error_code ec;
        fs::path dir = fs::path (base) / stamp;
        for (int i = 2; fs::exists (dir, ec); ++i)
            dir = fs::path (base) / (std::string (stamp) + "_" + std::to_string (i));
        fs::create_directories (dir, ec);
        if (ec)
        {
            error.store (true);
            return;
        }

        runDir = dir.string();
        events.open ((dir / "events.csv").string());
        events << "event_id,run_time_s,kind,label,freq_hz,frames\n";

        framesFile = std::fopen ((dir / "frames.csv").string().c_str(), "w");
        if (framesFile != nullptr)
            std::fprintf (framesFile, "event_id,frame,t_rel_s,freq_hz,level_db,prom_db,slope_db_per_s,"
                                      "drift_hz,above_room_db,broadband_db\n");

        openWav ((dir / "audio.wav").string());

        if (! events.is_open() || framesFile == nullptr || wav == nullptr)
        {
            error.store (true);
            closeFiles();
            return;
        }

        runOpen = true;
        capturing.store (true);
        examples.store (0);
        nextEventId = 1;
        frame = 0;
        runSamples = 0;
        ringPos = hopCount = 0;
        hopEnergy = 0.0;
        std::fill (ring.begin(), ring.end(), 0.f);
        pending.clear();
        notchFreqs.clear();
        startDropped = droppedSamples.load();
    }

    void closeRun (int reason)
    {
        resolvePending (true);

        const char* why = reason == 1 ? "reached gain ceiling" : reason == 2 ? "out of notches"
                        : reason == 3 ? "time limit" : reason == 4 ? "stopped by user" : "interrupted";

        std::ofstream sum (runDir + "/summary.txt");
        sum << "Freq'd Out capture (" << kVersion << ")\n"
            << "sample_rate " << sr << "\n"
            << "fft_size " << fftSize << "\n"
            << "hop " << hop << " (" << hopSec * 1000.0 << " ms)\n"
            << "lead_in_s 2.5\n"
            << "no_feedback_wait_s 3.0\n"
            << "run_length_s " << (double) runSamples / sr << "\n"
            << "examples " << examples.load() << "\n"
            << "notched_frequencies " << notchFreqs.size() << "\n"
            << "end " << why << "\n"
            << "dropped_samples " << (droppedSamples.load() - startDropped) << "\n"
            << "dropped_marks " << droppedMarks.load() << "\n";

        closeFiles();
        runOpen = false;
        capturing.store (false);
    }

    void closeFiles()
    {
        if (events.is_open())
            events.close();
        if (framesFile != nullptr)
        {
            std::fclose (framesFile);
            framesFile = nullptr;
        }
        closeWav();
    }

    //--------------------------------------------------------------------------
    // Minimal 24-bit mono WAV writer.
    static void put32 (std::FILE* f, uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc ((int) ((v >> (8 * i)) & 0xff), f); }
    static void put16 (std::FILE* f, uint16_t v) { std::fputc (v & 0xff, f); std::fputc ((v >> 8) & 0xff, f); }

    void openWav (const std::string& path)
    {
        wav = std::fopen (path.c_str(), "wb");
        if (wav == nullptr)
            return;
        wavFrames = 0;
        std::fwrite ("RIFF", 1, 4, wav); put32 (wav, 0);
        std::fwrite ("WAVEfmt ", 1, 8, wav); put32 (wav, 16);
        put16 (wav, 1); put16 (wav, 1);
        put32 (wav, (uint32_t) std::lround (sr)); put32 (wav, (uint32_t) std::lround (sr) * 3);
        put16 (wav, 3); put16 (wav, 24);
        std::fwrite ("data", 1, 4, wav); put32 (wav, 0);
    }

    void writeWavSample (float s)
    {
        if (wav == nullptr)
            return;
        const int32_t v = (int32_t) std::lround (std::clamp (s, -1.f, 1.f) * 8388607.f);
        std::fputc (v & 0xff, wav);
        std::fputc ((v >> 8) & 0xff, wav);
        std::fputc ((v >> 16) & 0xff, wav);
        ++wavFrames;
    }

    void closeWav()
    {
        if (wav == nullptr)
            return;
        const uint32_t data = (uint32_t) (wavFrames * 3);
        if (data & 1)
            std::fputc (0, wav);   // pad byte
        std::fseek (wav, 4, SEEK_SET);  put32 (wav, 36 + data + (data & 1));
        std::fseek (wav, 40, SEEK_SET); put32 (wav, data);
        std::fclose (wav);
        wav = nullptr;
    }

    //--------------------------------------------------------------------------
    float* frameRow (long f) { return history.data() + (size_t) (f % historyFrames) * (size_t) bins; }
    float* baseRow (long f)  { return baseHist.data() + (size_t) (f % historyFrames) * (size_t) bins; }

    float prominence (const float* row, int k) const
    {
        float sum = 0.f;
        int n = 0;
        for (int d = 6; d <= 14; ++d)
        {
            if (k - d >= 0)        { sum += row[k - d]; ++n; }
            if (k + d < bins)      { sum += row[k + d]; ++n; }
        }
        return n > 0 ? row[k] - sum / (float) n : 0.f;
    }

    double interpFreq (const float* row, int k) const
    {
        const float a = row[k - 1], b = row[k], c = row[k + 1];
        const float den = a - 2.f * b + c;
        const float d = std::abs (den) > 1.0e-6f ? std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f) : 0.f;
        return ((double) k + d) * sr / fftSize;
    }

    static bool nearAny (const std::vector<double>& fs, double f, double ratio)
    {
        for (double x : fs)
            if (std::abs (f / x - 1.0) < ratio)
                return true;
        return false;
    }

    //--------------------------------------------------------------------------
    void setupFft()
    {
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * i / fftSize);

        fftBuf.assign ((size_t) fftSize, {});
        twiddle.resize ((size_t) fftSize / 2);
        for (int i = 0; i < fftSize / 2; ++i)
            twiddle[(size_t) i] = std::polar (1.f, (float) (-2.0 * kPi * i / fftSize));

        bitrev.resize ((size_t) fftSize);
        int bits = 0;
        while ((1 << bits) < fftSize)
            ++bits;
        for (int i = 0; i < fftSize; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            bitrev[(size_t) i] = r;
        }
    }

    void fft (std::vector<std::complex<float>>& x) const
    {
        const int n = fftSize;
        for (int i = 0; i < n; ++i)
            if (i < bitrev[(size_t) i])
                std::swap (x[(size_t) i], x[(size_t) bitrev[(size_t) i]]);

        for (int len = 2; len <= n; len <<= 1)
        {
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < len / 2; ++j)
                {
                    const auto t = twiddle[(size_t) (j * step)] * x[(size_t) (i + j + len / 2)];
                    x[(size_t) (i + j + len / 2)] = x[(size_t) (i + j)] - t;
                    x[(size_t) (i + j)] += t;
                }
        }
    }

    //--------------------------------------------------------------------------
    // shared
    SpscRing<float> samples;
    SpscRing<Mark>  marks;
    std::atomic<bool> quit { false }, capturing { false }, error { false };
    std::atomic<int>  examples { 0 };
    std::atomic<uint64_t> droppedSamples { 0 }, droppedMarks { 0 };
    std::thread worker;
    uint64_t audioIndex = 0;   // audio thread only

    // settings
    std::string baseFolder;
    double sr = 48000.0, hopSec = 0.02;
    int fftSize = 8192, hop = 1024, bins = 4097;
    int leadFrames = 0, waitFrames = 0, scanEvery = 1, historyFrames = 1;

    // background thread only
    uint64_t consumed = 0, startDropped = 0;
    bool runOpen = false;
    std::string runDir;
    std::ofstream events;
    std::FILE* framesFile = nullptr;
    std::FILE* wav = nullptr;
    uint64_t wavFrames = 0;
    int nextEventId = 1;

    std::vector<float> ring, window, history, baseHist, broadband, baseline;
    std::vector<std::complex<float>> fftBuf, twiddle;
    std::vector<int> bitrev;
    int ringPos = 0, hopCount = 0;
    double hopEnergy = 0.0;
    long frame = 0;
    uint64_t runSamples = 0;
    std::vector<Pending> pending;
    std::vector<double> notchFreqs;
};

} // namespace sjam
