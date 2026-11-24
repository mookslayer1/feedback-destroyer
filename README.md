# Freq’d Out

A 24-notch feedback suppressor (VST3, macOS) with a pre-show ring-out mode and a live tracking mode. See [DESIGN.md](DESIGN.md)
for how it works and what is still to be decided.

## Getting a build

1. Open the **Actions** tab on GitHub and click the latest run.
2. When it finishes with a green tick, scroll to **Artifacts** and download
   `FreqdOut-VST3-macOS-installer`.
3. Unzip it and run `FreqdOut-VST3-macOS.pkg`.

## Installing on a Mac

The installer places the plugin at:

```text
/Library/Audio/Plug-Ins/VST3/Freq'd Out.vst3
```

After installing, restart your host and rescan plugins. Look for **Freq’d Out** by **SJAM Audio**.

The plugin is currently ad-hoc signed rather than Developer ID signed and notarized. If macOS blocks it after downloading, remove the quarantine flag with:

```bash
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Freq'd Out.vst3"
```

## Status

Second version (two modes, 30 Hz to 40 kHz, Q and manual depth control). Test with monitors turned down and a
limiter on the output: ringing out a system can produce loud feedback.
