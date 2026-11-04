# OpGrid

A 16-track FM groovebox for Ableton Move, built as a [Schwung](https://github.com/charlesvestal/schwung) overtake module.

**Status: milestone 2 of 8** — FM Poly engine, filter, envelopes, 12-voice pool, keys mode, sound editing. No sequencer yet.

## Build and install

```bash
make test               # engine + UI tests, writes build/demo.wav
./scripts/build.sh      # cross-compile for the Move -> build/opgrid-module.tar.gz
MOVE_HOST=ableton@move.local ./scripts/install.sh
```

Requires Schwung installed on the Move.

## Controls (current)

| Control | Action |
|---|---|
| Pads | Play the selected track (rows are a fourth apart) |
| Shift + pad (bottom 16) / Shift + step | Select track 1–16 |
| Knobs 1–8 | Edit the page's 8 parameters (Shift = fine) |
| Jog wheel / Left / Right | Change page: FM, FM 2, Filter, Amp |
| Up / Down | Octave |
| Back | Leave; sound keeps running. Shift + Vol touch + Jog click exits fully |

External MIDI: channel N plays track N.

## Sound (FM Poly)

4 operators, 8 algorithms (op 4 has feedback), 22 ratios per operator, per-op decay envelopes for modulators, 12/24 dB LP, HP, BP and notch filter with its own ADSR, amp ADSR, drive, pan.

| Algo | Routing |
|---|---|
| 1 | 4 > 3 > 2 > 1 |
| 2 | (3 + 4) > 2 > 1 |
| 3 | (2 + 3 + 4) > 1 |
| 4 | 4 > 3 > 1, 2 > 1 |
| 5 | 2 > 1 and 4 > 3, both heard |
| 6 | 4 > 1, 2, 3, all heard |
| 7 | 4 > 3, plus 1 and 2 heard |
| 8 | 1 + 2 + 3 + 4 additive |

When an operator other than 1 is heard (a carrier), its LVL knob sets its volume.

## Layout

- `src/og_engine.c/.h` — DSP core (no allocation or I/O on the audio thread)
- `src/og_plugin.c` — Schwung plugin API v2 glue and param protocol
- `src/ui.js` — screen, pads, knobs, LEDs
- `test/` — C engine tests + benchmark, Node UI tests with a fake host
