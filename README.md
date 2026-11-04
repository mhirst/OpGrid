# OpGrid

A 16-track FM groovebox for Ableton Move, built as a [Schwung](https://github.com/charlesvestal/schwung) overtake module.

**Status: milestone 3 of 8** — FM Poly engine, 12-voice pool, and a 16-track step sequencer with step locks, conditions, micro-timing, ratchets, chords and swing. No clip launching, saving or song mode yet.

## Build and install

```bash
make test               # engine, sequencer and UI tests; writes build/demo.wav and build/seq_demo.wav
./scripts/build.sh      # cross-compile for the Move -> build/opgrid-module.tar.gz
MOVE_HOST=ableton@move.local ./scripts/install.sh
```

Requires Schwung installed on the Move.

## Controls (current)

| Control | Action |
|---|---|
| Step buttons | Toggle steps on the selected track |
| Hold step + knob | Lock that value on the step (sound pages), or edit the step (STEP page) |
| Hold step + pad | Set the step's note; more pads add a chord (up to 4 notes) |
| Delete + step | Clear the step and its locks |
| Shift + step 1–4 | Show steps 1–16 / 17–32 / 33–48 / 49–64 |
| Play | Start / stop |
| Pads | Play the selected track (rows are a fourth apart) |
| Shift + pad (bottom 16) | Select track 1–16 |
| Mute + pad (bottom 16) | Mute / unmute a track |
| Shift + Mute (hold) | Fill mode |
| Knobs 1–8 | Edit the page's 8 values (Shift = fine) |
| Jog wheel / Left / Right | Change page: FM, FM 2, FILTER, AMP, STEP, CLIP |
| Up / Down | Octave |
| Back | Leave; music keeps playing. Shift + Vol touch + Jog click exits fully |

External MIDI: channel N plays track N.

### Pages

- **STEP** (hold a step): type (note / lock-only), note, velocity, length, micro-timing (±23/48 step), ratchets (1–8), condition, slide. With no step held, NOTE/VEL/LEN set the defaults for new steps.
- **CLIP**: length (1–64), speed (1/8x–2x), swing (50–80%), tempo, and tempo source (MOVE = follow Move's tempo, OWN = this module's).

### Step conditions

| Condition | Plays when |
|---|---|
| A:B (1:2 … 8:8) | On loop A of every B loops |
| 5% … 95% | That chance |
| FILL / !FIL | Fill mode is held / not held |
| PRE / !PRE | The last conditional step on this track played / did not |
| 1ST / !1ST | First loop since play started / every loop after |

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

- `src/og_engine.c/.h` — voices and DSP (no allocation or I/O on the audio thread)
- `src/og_seq.c` — sequencer, runs every 16 samples inside the audio callback
- `src/og_plugin.c` — Schwung plugin API v2 glue and param protocol
- `src/ui.js` — screen, pads, knobs, LEDs
- `test/` — C engine and sequencer tests (incl. a 10-minute drift test), Node UI tests with a fake host
