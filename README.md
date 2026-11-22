# Feedback Destroyer (VST3, macOS)

24-band adaptive feedback suppressor. Analysis-only detector (FFT, PNPR / PHPR / growth /
persistence / tonal guard) driving 24 dynamically allocated narrow notches. Zero reported
latency. Mono and stereo. VST3 only (SuperRack Performer's third-party support is VST3).

## Signal flow
input -> 24 dynamic notches -> frequency shifter (0 to 6 Hz) -> output. The detector listens
to the input only. The shifter sits last so the notches act on the frequencies the detector saw.

## Controls
- **Strength** - the master knob. 0 % bypasses everything, 100 % uses the full Max Depth and
  the full Max Shift. At 70 % (default) you get 70 % of each.
- **Max Shift** - the shift at 100 % Strength. 1-2 Hz is hard to hear on speech; sustained
  singing may sound slightly off above ~3 Hz. Set 0 for notches only.
- Sensitivity, Speed, Max Depth, Notch Q, Release, Bands - advanced notch behaviour.

## Layout
- `Source/Core/` - pure C++ DSP, no JUCE: `FeedbackDetector.h`, `FeedbackEngine.h` (detector +
  notch bank + band allocation), `Biquad.h`, `Fft.h`, `FrequencyShifter.h`, `LinkRegistry.h` (stage-2 stub)
- `Source/Plugin*.{h,cpp}` - JUCE wrapper: parameters, state, editor
- `tests/core_tests.cpp` - closed-loop feedback simulation + false-positive tests

## Prerequisites
    xcode-select --install
    brew install cmake

## Build
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release -j
The first configure downloads JUCE (needs internet). `COPY_PLUGIN_AFTER_BUILD` installs
`Feedback Destroyer.vst3` into `~/Library/Audio/Plug-Ins/VST3`.

## Test the DSP core (no JUCE needed)
    c++ -std=c++20 -O2 -I Source/Core tests/core_tests.cpp -o core_tests && ./core_tests

## Using it in SuperRack Performer
1. If macOS blocks it: `codesign --force --deep -s - "$HOME/Library/Audio/Plug-Ins/VST3/Feedback Destroyer.vst3"`
2. In Performer, scan for VST3 plugins from the menu below the rack (clear the VST3 cache and
   rescan if it was blacklisted). Needs Performer V14+; the plugin must be arm64 on Apple Silicon
   (the default build here is universal).
3. Optional validation: `pluginval --strictness-level 5 "$HOME/Library/Audio/Plug-Ins/VST3/Feedback Destroyer.vst3"`

## Link test (stage 2 groundwork)
Put an instance on a few channels and one on a bus, set them all to the same Link Group. The
status line shows how many instances share the group. If it counts them all, Performer runs
every instance in one process and the linking approach will work.

## Known limits / next steps
- A pure steady sine with no growth (a held synth tone) can look like feedback after ~0.5-1 s
  at Medium speed. Use Slow, or lower Sensitivity.
- FFT runs on the audio thread once per 2048 samples. Move it to a worker thread if profiling
  at very small buffers says so.
- Not yet done: spectrum display, per-band manual edit, master/channel attribution logic.
