# Desk recipe format (draft)

A recipe is what a finished run produces, in a form that can be typed into, or pushed to, a desk's own EQ.

```json
{
  "format": "freqd-out-recipe",
  "version": 1,
  "created": "2026-11-25T20:00:00Z",
  "venue": "Example Hall",
  "channel": "Vox C",
  "sampleRate": 96000,
  "lowLimitHz": 247,
  "gainCompensationDb": 6.0,
  "endReason": "ceiling",
  "notches": [
    { "freqHz": 3290.0, "cutDb": 15.0, "q": 28.0, "locked": true, "manual": false }
  ]
}
```

Notes
- `endReason`: `ceiling`, `outOfNotches`, `timeLimit` or `stopped`.
- Desks with fewer EQ bands than notches: sort by cut depth and apply the deepest first; the exporter should say which were left out.
- Desk-specific mappings (band counts, Q scales, protocol) live in per-desk adapters, not in this format.
