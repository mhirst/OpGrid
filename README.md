# OpGrid

A 16-track FM groovebox for Ableton Move, built as a [Schwung](https://github.com/charlesvestal/schwung) overtake module.

**Status: milestone 5 of 8** — FM Poly engine, 12-voice pool, 16-track step sequencer (step locks, conditions, micro-timing, ratchets, chords, swing), 3 LFOs per track, shared delay/reverb/chorus, master compressor, mixer, MIDI out, and 8 clips per track with a launch grid and scenes. No recording, saving or song mode yet.

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
| Shift + Play | Stop all clips (at the next launch point) |
| Loop | Clip grid on / off |
| Pads | Play the selected track (rows are a fourth apart) |
| Shift + pad (bottom 16) | Select track 1–16 |
| Mute + pad (bottom 16) | Mute / unmute a track |
| Mute (tap) | Mixer mode on / off |
| Shift + Mute (hold) | Fill mode |
| Knobs 1–8 | Edit the page's 8 values (Shift = fine) |
| Jog wheel / Left / Right | Change page: FM, FM 2, FILTER, AMP, LFO 1–3, SEND, STEP, CLIP, FX 1, FX 2 |
| Up / Down | Octave |
| Back | Leave; music keeps playing. Shift + Vol touch + Jog click exits fully |

External MIDI: channel N plays track N.

### Clip grid (Loop)

8 tracks × 4 scenes on the pads; each track has 8 clip slots. Top pad row = scene 1.

| Control | Action |
|---|---|
| Pad with a clip | Launch it (on the next launch point) and edit it |
| Empty pad | Select it for editing; it becomes a clip once you add steps |
| Shift + pad | Launch that row's scene (empty slots stop their track) |
| Mute + pad | Stop that track |
| Copy (hold) + pad, pad | Copy a clip |
| Delete + pad | Clear a clip |
| Left / Right | Tracks 1–8 / 9–16 |
| Up / Down | Scenes 1–4 / 5–8 |

Launch quantize (CLIP page, QUAN): off, 1 step, 1 beat, 1 bar (default), 2 bars, 4 bars. Steps, locks and clip settings always edit the selected clip, even while another one plays.

### Mixer mode (tap Mute)

| Control | Action |
|---|---|
| Bottom 16 pads | Mute / unmute track (Shift = solo) |
| Top 16 pads | Select track |
| Knobs 1–8 | Track volume (Left = tracks 1–8, Right = 9–16) |

### Pages

- **STEP** (hold a step): type (note / lock-only), note, velocity, length, micro-timing (±23/48 step), ratchets (1–8), condition, slide. With no step held, NOTE/VEL/LEN set the defaults for new steps.
- **LFO 1–3** (per track, lockable): destination (pitch, any sound value or send), wave (sine, triangle, square, saw, ramp, random), sync or free, rate, depth (±), trigger (free-running or restart per note), phase, fade-in.
- **SEND**: delay, reverb and chorus sends; MIDI channel (OFF or 1–16) and mode (INT = internal sound only, EXT = MIDI to USB-A only, BOTH).
- **FX 1** (global): delay time (synced, 1/32 to 1 bar, dotted and triplet), feedback, ping-pong, tone (low-pass ↔ high-pass); reverb size, decay, pre-delay, damping.
- **FX 2** (global): chorus depth, rate, width; master compressor threshold, ratio, makeup gain; master drive and volume.
- **CLIP**: length (1–64), speed (1/8x–2x), swing (50–80%), tempo, tempo source (MOVE = follow Move's tempo, OWN = this module's), launch quantize.

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
- `src/og_fx.c` — delay, reverb, chorus, master bus
- `src/og_plugin.c` — Schwung plugin API v2 glue and param protocol
- `src/ui.js` — screen, pads, knobs, LEDs
- `test/` — C engine, sequencer, effects and clip tests (incl. a 10-minute drift test and a 20 s max-feedback stability test), Node UI tests with a fake host
