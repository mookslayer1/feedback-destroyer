# Capture module: adding it to Freq'd Out later

**Status:** stand-alone. Nothing in the main plugin uses it yet, and `CMakeLists.txt` and the
plugin source are untouched. The normal build behaves exactly as before.

## What it does

While a ring-out runs, it saves training examples for teaching the plugin to spot feedback
earlier. It doesn't change the sound. For each run it creates a folder in
`~/Music/SJAM Audio/Freqd Out Captures/<date>_<time>/` containing:

| File | Contents |
|---|---|
| `audio.wav` | The exact signal the detector heard (mono, 24-bit). |
| `events.csv` | One line per example: time, frequency, kind and label. |
| `frames.csv` | The 2.5 s leading up to each example, about 21 ms per row. |
| `summary.txt` | Settings and counts for the run. |

The plugin labels the examples itself:

* `new_notch`, `deepen`, `merge` are labelled **1**: this frequency really fed back.
* `no_feedback` is labelled **0**: a peak stood out but needed no notch within 3 s
  (a voice, the fan heater, room tone).

The columns in `frames.csv` are:

* `level_db`
* `prom_db`: how far the peak stands above its neighbours.
* `slope_db_per_s`: how fast it is growing.
* `drift_hz`: pitch wobble. Feedback holds still; voices move.
* `above_room_db`: how far it sits above the room's usual level at that frequency.
* `broadband_db`: overall level.

The `~/Music` folder is used because macOS doesn't show a privacy prompt for plugins
writing there (Documents and Desktop would trigger one).

## Real-time safety

The audio thread only copies one sample and sometimes posts a small message, with no locks,
memory allocation or file access. A background thread does all the analysis and writing. It
was tested under ThreadSanitizer with a simulated 16 s ring-out (`tests/capture_test.cpp`)
and needs no JUCE.

## How to switch it on (when we're ready)

It's built as an option, so a "capture" test build and the normal build come from the same code.

### 1. CMakeLists.txt: add after `target_sources(...)`

```cmake
option(FREQD_CAPTURE "Build the test-only Capture recorder into the plugin" OFF)
if(FREQD_CAPTURE)
    target_sources(FeedbackDestroyer PRIVATE Source/Capture/CaptureRecorder.h)
    target_compile_definitions(FeedbackDestroyer PRIVATE FREQD_CAPTURE=1)
endif()
```

### 2. PluginProcessor.h

At the top, after `#include "Analyzer.h"`:

```cpp
#if FREQD_CAPTURE
 #include "Capture/CaptureRecorder.h"
 #define FREQD_CAPTURE_DO(x) x
#else
 #define FREQD_CAPTURE_DO(x)
#endif
```

In the `private:` section, next to `Analyzer lowA, mainA;`:

```cpp
#if FREQD_CAPTURE
    sjam::CaptureRecorder capture;
#endif
```

### 3. PluginProcessor.cpp: six one-line hooks

| Where | Add |
|---|---|
| `prepareToPlay`, after `sr = sampleRate;` | `FREQD_CAPTURE_DO (capture.prepare (sampleRate));` |
| `processBlock`, in the sample loop, straight after `const float m = ...;` | `FREQD_CAPTURE_DO (capture.pushSample (m));` |
| `handleCandidates`, after `n.target += kDepthStepDb;` | `FREQD_CAPTURE_DO (capture.mark (sjam::CaptureRecorder::Deepen, n.freq));` |
| `handleCandidates`: change `if (mergeIntoExisting (f, mergeRatio)) continue;` to | `if (mergeIntoExisting (f, mergeRatio)) { FREQD_CAPTURE_DO (capture.mark (sjam::CaptureRecorder::Merge, f)); continue; }` |
| `handleCandidates`, after `++newCount;` | `FREQD_CAPTURE_DO (capture.mark (sjam::CaptureRecorder::NewNotch, tg[t].f));` |
| `startProcess`, after `procState.store (Running);` | `FREQD_CAPTURE_DO (capture.runStarted());` |
| `finishProcess`, after `endReason.store (reason);` | `FREQD_CAPTURE_DO (capture.runEnded (reason));` |

(The `startProcess` and `finishProcess` lines are just the run start and end markers.)

### 4. Build workflow

In the step that runs `cmake -B build ...`, add `-DFREQD_CAPTURE=ON` for a capture test build.
Leave it out for the normal build.

## Test it on its own (optional)

```
c++ -std=c++17 -O2 -pthread -ISource tests/capture_test.cpp -o capture_test
./capture_test /tmp/capture_out
```

## Next steps after collecting

Send the capture folders (or zip `Freqd Out Captures`). They'll be used to train a small
early-warning model, which goes into the plugin through RTNeural, or as a simpler rule if
that does the job as well.
