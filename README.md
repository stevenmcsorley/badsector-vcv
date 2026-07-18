# Bad Sector

**A stereo buffer-corruption and broken-playback processor** for VCV Rack 2, by halfagiraf.

Bad Sector is a circuit-bent digital audio buffer inspired by the ways audio media fail:
skipping CDs, defective tape machines, scratched records and software bugs.

- **Time** — internal clock (16 s – 80 Hz) or external clock with musical divide/multiply
  (/16 … x8). Every division acquires the just-completed slice of audio for mangling.
- **Repeats** — subdivides the division into musical stutter counts (powers of two + triplets),
  right up into audio rate.
- **Bend** — tape-medium failures: varispeed pitch changes in octaves and fifths, reverses,
  tape stops, vinyl clicks — rolled automatically every clock division.
- **Break** — digital failures: subsection jumps, extra repeats, synchronized dropouts.
- **Corrupt** — end-of-chain media damage: Decimate / Dropout / Destroy (plus two bonus
  effects in the context menu).
- **Freeze**, **Micro mode** (manual playback speed ±3 octaves + reverse), glitch windowing,
  latching/momentary gates, clock-reset input, stereo-unique or shared behaviour.

Includes the **Ambient Wash** factory preset — fast divisions, full windowing and Bend
shimmer turn the buffer into a beat-synced granular cloud.

## Building

```
export RACK_DIR=/path/to/Rack-SDK
make
make install
```

## License

GPL-3.0-or-later. Panel artwork © halfagiraf.
