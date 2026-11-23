# Freq'd Out: design notes (working title: Feedback Destroyer)

A 24-notch feedback suppressor, built as a VST3 plugin for macOS and intended to run
inside Waves SuperRack Performer (probably on buses).

The goal is more gain before feedback, achieved with an original design: our own
detection method, our own notch handling and our own interface. It does not use any
code, presets, graphics or wording from other products.

## Two modes

**Pre-show** is a run-and-done process with a clear start and end.
- **Start** begins the run. The plugin listens, raises its output gain a little at a
  time, and places a notch on each frequency that starts to ring.
- It ends when: the gain ceiling (+6 dB) is reached and things stay calm, all 24 notches
  are used, 3 minutes pass, or you press **Stop**. Switching to Live also ends it.
- At the end every notch is **locked** in place with an **extra 3 dB cut**, and the
  output gain stays at the level it reached. Nothing analyses or changes gain afterwards.
- Re-running keeps the existing notches and only adds new ones. **Reset** starts over.

**Live** is for performance. Locked notches stay as they are and the gain never creeps.
New feedback is caught by notches about a **quarter octave wide** (Q about 5.8) that
start gentle (4 dB) and deepen only while the frequency keeps ringing. A live notch
follows its frequency if it drifts within a quarter octave, and lets go after about
12 seconds if it is no longer needed.

## Controls

| Control | What it does |
| --- | --- |
| **Threshold** | How readily a peak counts as feedback. Lower = reacts earlier. The heat strip shows what the plugin sees, with a dashed line at the Threshold. |
| **Amount** | How deep automatic cuts may go: 6 dB (0) to 24 dB (100). The shaded zone on the faders is beyond it. |
| **Mode** | Pre-show or Live. |
| **Start / Stop** | Runs the pre-show process (Pre-show mode only). |
| **Lock all** | Locks every active notch where it is. |
| **Reset** | Clears every notch and returns the gain to 0 dB. |

On the faders: **drag** a fader to set its depth by hand (up to 24 dB, past the Amount
limit). A manual notch is pinned and is never changed automatically. **Mouse wheel** over
a fader changes its Q. **Double-click** switches a notch off or on.

## Notches

- **24 notches**, any of which can be floating (found, not yet locked) or locked.
- Range **30 Hz to 40 kHz**, but never above 45% of the sample rate. At 48 kHz that is
  about 21.6 kHz; the full 40 kHz needs a session at 96 kHz or higher.
- Pre-show notches are very narrow (Q about 28). If two ringing frequencies are within
  about 12% of each other they share one wider notch (Q between 6 and 28), freeing a slot.
- Depth: starts at 6 dB (pre-show) or 4 dB (live), deepens in 3 dB steps only while still
  ringing, up to the Amount limit. 24 dB is the hard maximum for everything.
- Locked notches are saved with the session, along with the earned output gain.

## Detection

Two analysers run side by side on the plugin's own output (after the notches, in mono):
- **Low band, 30 to 400 Hz:** a long 32768-sample window (about 1.5 Hz per bin at 48 kHz).
  Slower to react, but much finer detail where it matters for the low end.
- **Everything above 400 Hz:** an 8192-sample window, updated about every 43 ms.

A peak counts as feedback when it stands well above its neighbours (the margin comes from
Threshold) and is either growing over a few frames or has stayed prominent for about a
second. The exact frequency is sharpened between FFT bins. Up to 3 problem frequencies are
handled per frame. The notches add no latency.

## Safety

- If feedback grows and no notch is free, the gain drops 3 dB straight away.
- In the pre-show run, a new notch is placed first, then the gain backs off (1 dB for one
  frequency, 2 dB for two) and creeping pauses for about 1.5 s.
- Run the pre-show with the monitors low and a limiter on the output.

## Interface

Graphic EQ style, log frequency axis. Each notch is a fader that drops from 0 dB to its
cut. Blue = locked, amber = found or live, grey = switched off, white outline = manual.
A heat strip above shows how feedback-like each part of the spectrum looks. The window
also shows the run state, output gain and the selected notch's frequency, cut and Q.

## Not done yet / open questions

- Dereverb is not designed or built.
- The 40 kHz top end depends on the session sample rate (needs 96 kHz or higher).
- Whether the pre-show run should also stop when each new notch buys under about 1 dB.
- Final gain ceiling (currently +6 dB).
- A sustained pure tone from an instrument can look like feedback. Raise Threshold if so.
- The look in the mockups (Silver rack) is not in the plugin yet; the plugin keeps its
  current dark Graphic EQ look.
- This version has not been compiled or tested yet. Expect build errors to fix.
