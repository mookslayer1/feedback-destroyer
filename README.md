# Freq’d Out

A live feedback-suppression plugin for use in Waves SuperRack and other plugin hosts.

## Modes

- **Pre-Show:** Rings out the system, deploys narrow static safety notches, then locks them with an additional 6 dB failsafe cut.
- **Live:** Uses one intelligent adaptive notch to protect against unexpected feedback during the show.

## Controls

- **START / STOP** — starts or stops the selected mode.
- **PRE-SHOW / LIVE** — selects the operating mode.
- **Frequency display** — drag a notch to change depth; use the mouse wheel for Q; double-click to enable or disable it.

## Live-use workflow

1. Set a safe starting gain.
2. Select **PRE-SHOW** and press **START**.
3. Gradually raise gain until feedback modes are identified and notched.
4. Stop the process to lock the safety EQ.
5. Select **LIVE** and press **START** before the performance.
6. Use **STOP** during Live to release the adaptive notch.
