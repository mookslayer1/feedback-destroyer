# Architecture

## Today
- `Source/Analyzer.h` two FFT analysers (30-400 Hz, 32768-point; above 400 Hz, 8192-point) with peak prominence, growth and persistence tracking.
- `Source/Biquad.h` peaking filter (RBJ), double precision, used as a notch.
- `Source/PluginProcessor.*` the run-and-set process: notch bank (48), merging of close neighbours, gain compensation and ceiling, lock and safety cut, state save/restore.
- `Source/PluginEditor.*` the dark graphic-EQ style interface.

The one real-time rule: the audio thread never allocates or locks. The editor reads atomics only.

## Target shape
```
core/        JUCE-free C++17: analyser, biquad, notch bank, run state machine, recipe export. Unit-tested.
Source/      JUCE wrapper: processor, editor, parameters, state.
hardware/    firmware for the pedal; links core/ only.
```
The pedal and any future platform reuse `core/`, so the algorithm is tuned once. This move should wait until the plugin's behaviour is settled, because moving code while it still changes would make every fix harder.

## Build
- Default CMake configure builds VST3 only (the official build).
- `FD_BUILD_AU`, `FD_BUILD_STANDALONE`, `FD_BUILD_AUV3` add formats; see the comments in `CMakeLists.txt`.
- Plugin identity must never change: manufacturer `Sjam`, plugin code `Fqdo`, bundle id `com.sjamaudio.freqdout`. Changing them makes hosts treat it as a different plugin.
