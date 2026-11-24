# Freqd Out v3.1

A 24-notch feedback suppressor (VST3, macOS) with a pre-show ring-out mode and a live tracking mode. See [DESIGN.md](DESIGN.md)
for how it works and what is still to be decided.

## Getting a build

1. Open the **Actions** tab on GitHub and click the latest run.
2. When it finishes with a green tick, scroll to **Artifacts** and download
   `FreqdOut-v3.1-macOS-installer`.
3. Run `FreqdOut-v3.1-macOS.pkg`.

The workflow also uploads `FreqdOut-v3.1-macOS-zip` for direct testing without the installer.

## Installing on a Mac

The installer removes the older `Freq'd Out.vst3` and `Freqd Out.vst3` bundles, then installs:

```text
/Library/Audio/Plug-Ins/VST3/Freqd Out v3.1.vst3
```

After installing, restart your host and rescan plugins. Look for **Freqd Out v3.1** by **SJAM Audio**.

The plugin is currently ad-hoc signed rather than Developer ID signed and notarized. If macOS blocks it after downloading, remove the quarantine flag with:

```bash
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Freqd Out v3.1.vst3"
```

## Status

Version 3.1.1 adds legacy-bundle cleanup and automated pluginval validation. Test with monitors turned down and a
limiter on the output: ringing out a system can produce loud feedback.
