# Freqd Out

A feedback-suppression plugin by SJAM Audio, for use in Waves SuperRack and other plugin hosts.

## How it works

One process, run and set:

1. Set the **Low limit** to your channel's high-pass frequency (30–400 Hz).
2. Press **START**. Raise the channel gain gradually while the plugin rings out the system, finds feedback frequencies and places notches (up to 48).
3. The run ends by itself (gain ceiling reached, out of notches, or time limit) or when you press **STOP**. The status line says which. All notches are then locked with an extra safety cut.
4. Press **START** again at any time to add notches; existing notches are kept.

Close frequencies are merged into one wider notch as the bank fills. Drag a notch to cut deeper (24 dB max), use the mouse wheel for Q, double-click to switch a notch on or off. Hover over a notch to see its frequency.

## Repository

- `Source/` the plugin (JUCE 8)
- `CMakeLists.txt` build; VST3 by default, other formats opt-in (see the comments in the file)
- `.github/workflows/main.yml` official macOS VST3 installer build
- `.github/workflows/build-matrix.yml` manual cross-platform build scaffold
- `docs/` roadmap, architecture and the desk-recipe format
- `core/`, `hardware/` placeholders for the JUCE-free DSP library and the pedal
