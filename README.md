# Bad Sector

**A stereo buffer-corruption and broken-playback processor** for VCV Rack 2, by halfagiraf.

<p align="center">
  <img src="docs/badsector.png" width="420" alt="Bad Sector panel"/>
</p>

Bad Sector records continuously into a 64-second ring and, on every clock division,
*acquires* the division that just finished — then mangles that beat-aligned block with
tape failures, digital breakage and media corruption. Because every division hard-resyncs
the playhead, **everything stays locked to the clock no matter how slow, pitched or
mangled it gets** — a simple melody comes out transformed but never out of time.
(The wet path is therefore always one division behind the input; that inherent clocked
delay is the price of the guarantee, and disappears with loop-based material.)

## Controls

- **BUFFER** — the clock. Internal: 16 s – 80 Hz. External: divide/multiply of the CLOCK
  input (/16 /8 /4 /2 ×1 ×2 ×3 ×4 ×8), hard-locked to incoming edges, free-running at the
  last rate when the clock disappears. Accepts audio-rate clocks to ~1 kHz for
  frequency-locked tones.
- **REPEAT** — subdivides each division into *musical* stutter counts only (powers of two
  plus triplets, 1…1024). Rhythm lives in the first half of the travel; audio-rate buzz in
  the top stretch.
- **MIX** — equal-power dry/wet. Wet = the previous clock division.
- **DAMAGE** — one knob, three independently stored channels, cycled by its square
  selector button: **Bend** (cyan), **Break** (amber), **Corrupt** (red-orange). Switching
  channels snaps the knob to that channel's stored value. The dot above the button shows
  the selected channel's level.
  - **Bend** — tape failures, rolled fresh every division: varispeed jumps in octaves and
    fifths, reverses, beat-length tape stops, wow/flutter wobble and vinyl crackle that
    scale with the amount, and gliding speed slews at the top of the range.
  - **Break** — digital failures: subsection jumps, extra repeats (always from the musical
    table), and up to 90 % silence per repeat at the top.
  - **Corrupt** — end-of-chain media damage: **Decimate** (variable bit-crush +
    downsample), **Dropout** (random gaps — fewer/longer left, more/shorter right),
    **Destroy** (soft saturation into devastation). Two extra effects (DJ Filter, Vinyl
    Sim) can be enabled in the context menu. The CRPT gate steps the effect.
- **CV AMT** — the same three-channel pattern for **bipolar** CV attenuverters
  (centre = no modulation) over the Bend/Break/Corrupt CV inputs.
- **MICRO** — manual playback speed, ±3 octaves. Active in Micro mode (and optionally as
  a global varispeed under Macro via the context menu).
- **MODE / CLK / FRZ** — Macro/Micro, internal/external clock, and Freeze (latching by
  default, engaging on the next division so everything stays in sync).

## Modes

**Macro** — the machine drives: Bend and Break roll new manipulations every clock
division, per-channel when *Stereo: unique* is enabled.

**Micro** — you drive: MICRO sets the speed (BEND CV tracks 1 V/oct), the BEND gate
toggles reverse, and the Break channel becomes **Traverse** (select the looping
subsection) or **Silence** (duty cycle, toggled by the BREAK gate). The display shows the
speed with the hardware-style colour code — cyan on an exact octave, green reversed, gold
reversed-on-octave — and blips gold when the traverse subsection changes. Selector
channels that are inactive in the current mode dim to 25 %.

## The display

The central checksum artwork is live: data rows fragment with the damage — **cyan**
displacement from Bend, **amber** broken/repeated rows from Break, **red-orange** noise
blocks from Corrupt — and the neon-red readout shows the clock division, current corrupt
effect and a checksum that destabilises as corruption rises.

## Jacks

Three rows on one grid: a CV row (Buffer, Repeat, Mix, Bend, Break, Crpt), a gate row
aligned under the matching CV columns (Frz, Bend, Break, Crpt), and audio I/O with
**CLOCK** and **RESET** centre-bottom. RESET resyncs the internal clock immediately, or
realigns the external division counter on the next beat — patch your sequencer's reset
here so divisions land on your downbeat.

## Factory presets

**Tape Ghost** (haunted one-division echo) · **Skipping CD** (on-grid beat repeater) ·
**Half-Speed Memory** (everything returns an octave down) · **Data Rot** (Destroy) ·
**Underwater Vinyl** (max-Bend slew glides with dropouts) · **Ambient Wash** (fast-grain
shimmer texture).

## Building

```
export RACK_DIR=/path/to/Rack-SDK
make
make install
```

Two unit-test suites live in `tests/` (build each with `g++ -std=c++11 <file> -o test && ./test`):
`timing_test.cpp` validates the repeat grid — the same `BsGrid.hpp` arithmetic the module
runs — against exact rational clock fractions: exact window counts, boundaries within one
sample of the ideal fraction, and safe live Repeat changes, across sample rates and odd
division lengths. `selector_test.cpp` covers the three-channel knob recall.

## License

GPL-3.0-or-later. Panel artwork © halfagiraf. Inspired by hardware media-failure
processors; all DSP is original. Jack graphics derived from a generative render; some
earlier revisions used a resized VCV Component Library jack under its Rack-plugin licence.
