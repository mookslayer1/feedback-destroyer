# Freqd Out v3.1

A 24-notch feedback suppressor (VST3, macOS) with a pre-show ring-out mode and a live tracking mode. See [DESIGN.md](DESIGN.md)
for how it works and what is still to be decided.

## Product identity

- **Product name:** Freqd Out v3.1
- **Manufacturer:** SJAM Audio
- **Plugin format:** VST3
- **Supported macOS architectures:** Apple Silicon and Intel
- **Installation path:** `/Library/Audio/Plug-Ins/VST3/Freqd Out v3.1.vst3`

## Getting a build

1. Open the **Actions** tab on GitHub and click the latest run.
2. When it finishes with a green tick, scroll to **Artifacts**.
3. Download **`FreqdOut-v3.1-macOS-installer`** for the macOS installer.
4. Alternatively, download **`FreqdOut-v3.1-macOS-zip`** for direct manual testing.

## Installing on a Mac

Run `FreqdOut-v3.1-macOS.pkg`. The installer removes the older `Freq'd Out.vst3` and `Freqd Out.vst3` bundles, then installs:

```text
/Library/Audio/Plug-Ins/VST3/Freqd Out v3.1.vst3
```

After installing, restart your host and rescan plugins. Look for **Freqd Out v3.1** by **SJAM Audio**.

## Manual installation

If you downloaded `FreqdOut-v3.1-macOS-zip`, unzip it and copy `Freqd Out v3.1.vst3` to:

```text
/Library/Audio/Plug-Ins/VST3/
```

## macOS security

The plugin is currently ad-hoc signed rather than Developer ID signed and notarized. If macOS blocks it after downloading, remove the quarantine flag with:

```bash
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Freqd Out v3.1.vst3"
```

Then restart and rescan the host.

## Status

Version 3.1.1 adds legacy-bundle cleanup and automated pluginval validation. Test with monitors turned down and a
limiter on the output: ringing out a system can produce loud feedback.
