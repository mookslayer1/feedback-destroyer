# Roadmap: the Freqd Out product range

Status key: **done** works today / **scaffolded** build files exist but have never been run / **planned** idea only / **blocked** needs something outside the repo.

| Product | Status | Notes |
|---|---|---|
| macOS VST3 (installer .pkg) | done | `main.yml`; the reference build. |
| macOS Audio Unit | scaffolded | `-DFD_BUILD_AU=ON`. |
| Standalone app (macOS / Windows / Linux) | scaffolded | `-DFD_BUILD_STANDALONE=ON`. Mic permission text is set for macOS. |
| Windows VST3 | scaffolded | Unsigned. A code-signing certificate is needed before public release. |
| Linux VST3 | scaffolded | Dependency list in `build-matrix.yml` is a first guess. |
| iOS AUv3 + app | scaffolded, experimental | Builds unsigned. Needs an Apple Developer account to run on a device, and a touch-friendly editor (no mouse wheel for Q, small hit targets). |
| CLAP | planned | Via clap-juce-extensions; add once the model is settled. |
| AAX (Pro Tools) | blocked | Needs the Avid AAX SDK and developer account. |
| Desk recipe export | planned | See `recipe-format.md`. Most valuable next feature: works on desks that cannot host plugins. |
| Desk remote control (push the recipe to a console) | planned | Per desk, via its published network/MIDI protocol. Start with the desk you own. |
| Venue profiles (save/recall/share a room) | planned | Builds on the recipe format. |
| Dereverb companion | planned | Not designed yet. |
| Inline pedal / box | planned | See `../hardware/README.md`. Only after the algorithm has been proven in real rooms. |
| Console-native versions | blocked | Mostly a licensing conversation with each console maker; recipe export covers most of the need. |

## Order of work once the model is settled

1. Move the DSP into a JUCE-free core (`../core/`) with unit tests.
2. Recipe export (JSON) and a viewer inside the plugin.
3. Turn on the AU and standalone builds in the main workflow; run the matrix on Windows and Linux.
4. Touch-friendly editor, then iOS.
5. Desk control for one console, then venue profiles.
6. Pedal prototype.

## Before any public release

- Trademark check on the name.
- Code signing and notarisation on macOS; signing on Windows.
- Per-format validation (pluginval for VST3, `auval` for AU).
