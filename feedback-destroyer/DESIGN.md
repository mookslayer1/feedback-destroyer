# Feedback Destroyer: design notes

A 24-notch intelligent feedback suppressor, built as a VST3 plugin for macOS and
intended to run inside Waves SuperRack Performer (probably on buses).

The goal is more gain before feedback, achieved with an original design: our own
detection method, our own notch handling and our own interface. It does not use
any code, presets, graphics or wording from other products.

## Controls

| Control | What it does |
| --- | --- |
| **Threshold** | How readily the plugin decides a peak is feedback. Lower = reacts earlier. |
| **Amount** | How deep the cuts are allowed to go: 6 dB (0) up to 24 dB (100). |
| **Ring-out** | Lets the plugin raise its own output gain step by step (see below). |
| **Reset** | Clears every notch and returns the automatic gain to 0 dB. |
| **Lock notch / Keep floating** | Appears when a floating notch has held for a few seconds. |

Click a latched fader in the display to switch that notch off or on. A notch that
has been switched off keeps its frequency and is not re-learned until Reset.

## Notches

- **24 narrow notches** (Q about 28), working between **80 Hz and 8 kHz**.
  Below 80 Hz is left to the HPF.
- **12 latched + 12 floating.**
  - *Latched* notches stay at the frequency where they proved effective. They are
    the plugin's memory of the room and are saved with the session.
  - *Floating* notches are the spare team. They move to new feedback straight away,
    and let go after about 12 seconds if they are no longer needed.
- A floating notch that has held for about 4 seconds asks: "Lock it?" Yes makes it
  latched. No leaves it floating. If all 12 latched slots are used, the latched notch
  that has gone longest without work is replaced.
- Each notch is placed at the exact frequency of the ringing peak (not a fixed grid).

## Depth

- A new notch starts at **6 dB** and deepens in **3 dB** steps only while that
  frequency is still ringing.
- The limit is set by Amount (6 to 24 dB). 24 dB is the hard maximum.
- It is a purely adaptive EQ: no gate and no per-frequency maximiser.

## Detection

- The plugin listens to its own output (after the notches), in mono.
- An 8192-point FFT runs about every 43 ms. A peak counts as feedback when it
  stands well above its neighbours (the margin comes from Threshold) and is either
  growing over a few frames, or has been steadily prominent for about a second.
- Up to 3 problem frequencies are handled in the same frame. Two frequencies close
  together (within 3%) share one notch.
- The notches add no latency.

## Ring-out and automatic gain

The plugin works like an engineer ringing out a system:

1. With Ring-out on, the output gain creeps up 0.5 dB about every half second.
2. When new feedback appears, a floating notch is placed first, the gain backs off
   (1 dB for one frequency, 2 dB for two at once) and creeping stops for about 1.5 s.
3. When things are stable, creeping resumes.
4. The gain never goes above **+6 dB** (placeholder ceiling, to be confirmed).
5. If feedback grows and no floating notch is free, the gain drops 3 dB immediately.

With Ring-out off, the plugin holds the gain it has earned and only deals with new
feedback using floating notches. It never creeps upward by itself.

## Interface

Graphic EQ style: each notch is a vertical fader on a log frequency axis that drops
from 0 dB to its current cut. Faders move on their own. Blue = latched, amber =
floating, grey = switched off. Two knobs (Threshold, Amount), a Ring-out switch,
Reset, and the output gain readout.

## Not done yet / open questions

- Dereverb is not designed or built.
- Catch-all notches above 8 kHz: undecided (currently nothing above 8 kHz).
- Whether the gain creep should stop when each new notch buys under about 1 dB.
- Final gain ceiling (currently +6 dB).
- A sustained pure tone from an instrument can look like feedback. Raise Threshold
  if that happens. Needs real-world testing.
- Linked design (a master instance fed by instances on each channel) is a possible
  later step.
- This first version has not been compiled or tested yet. Expect build errors to fix.
