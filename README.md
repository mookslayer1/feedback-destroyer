# Freq'd Out

A 24-notch feedback suppressor (VST3, macOS) with a pre-show ring-out mode and a live tracking mode. See [DESIGN.md](DESIGN.md)
for how it works and what is still to be decided.

## Getting a build

1. Open the **Actions** tab on GitHub and click the latest run.
2. When it finishes with a green tick, scroll to **Artifacts** and download
   `FeedbackDestroyer-macOS-VST3`.
3. Unzip it. You get `FeedbackDestroyer-VST3.zip`, which holds the plugin.

## Installing on a Mac

1. Unzip `FeedbackDestroyer-VST3.zip` and copy `Freqd Out.vst3` into
   `/Library/Audio/Plug-Ins/VST3/`.
2. The plugin isn't signed by Apple yet, so macOS will block it. In Terminal, run:

   ```
   xattr -cr "/Library/Audio/Plug-Ins/VST3/Freqd Out.vst3"
   ```
3. Rescan plugins in your host.

## Status

Second version (two modes, 30 Hz to 40 kHz, Q and manual depth control). Not yet compiled or tested. Test with monitors turned down and a
limiter on the output: ringing out a system can produce loud feedback.
