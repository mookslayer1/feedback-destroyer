# Freq’d Out — Design

Freq’d Out is a live feedback-suppression plugin designed around two distinct operating phases: **Pre-Show** and **Live**.

## Operating model

### Pre-Show

Pre-Show is a controlled ring-out procedure. It identifies feedback modes while the operator raises system gain, deploys narrow static notches, and then locks the resulting safety EQ.

- Each new feedback mode receives a narrow notch.
- A notch deepens while its frequency remains active.
- When the Pre-Show process completes, every automatic notch receives an additional 6 dB failsafe cut.
- The locked notches remain in place with minimal ongoing DSP.
- Close feedback frequencies may share one widened notch, conserving filter slots.

### Live

Live uses a deliberately minimal protection strategy: **one intelligent adaptive notch**.

- It monitors for a new feedback event.
- It deploys its single adaptive notch at the offending frequency.
- Its depth is increased while the event remains active.
- If a nearby second frequency emerges, the notch is re-centred and widened rather than consuming another filter.
- Once the event is no longer needed, the notch releases after a short timeout.
- If a new, unrelated feedback mode appears later, the same adaptive notch moves to it.

## Controls

The user interface intentionally avoids threshold and amount controls.

- **START / STOP** starts or stops the selected mode.
- **PRE-SHOW / LIVE** selects the operating mode.
- Manual notch depth and Q remain available directly on the frequency display.

## Safety behaviour

Automatic notches are capped at 24 dB. Pre-Show notches receive an additional 6 dB failsafe cut when locked. If Live protection cannot resolve feedback with its single adaptive notch, output gain is reduced as a last-resort safety action.
