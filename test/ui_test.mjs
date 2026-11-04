/* Loads src/ui.js under a fake Schwung host and drives it like hardware. */
import fs from 'fs';
import path from 'path';
import os from 'os';
import { execFileSync } from 'child_process';
import { fileURLToPath, pathToFileURL } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let failures = 0;
const check = (c, msg) => { if (!c) { failures++; console.log('FAIL', msg); } };

/* stub of Schwung's shared/input_filter.mjs */
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'opgrid-ui-'));
const leds = [];
fs.writeFileSync(path.join(tmp, 'input_filter.mjs'), `
export function setLED(n, c) { globalThis.__leds.push(['n', n, c]); }
export function setButtonLED(n, c) { globalThis.__leds.push(['c', n, c]); }
export function decodeDelta(v) { return v === 0 ? 0 : v < 64 ? 1 : -1; }
export function decodeAcceleratedDelta(v) { return v === 0 ? 0 : v < 64 ? v : v - 128; }
export function invalidateLedCache() {}
`);
const src = fs.readFileSync(path.join(root, 'src/ui.js'), 'utf8')
    .replace('/data/UserData/schwung/shared/input_filter.mjs',
             pathToFileURL(path.join(tmp, 'input_filter.mjs')).href);
fs.writeFileSync(path.join(tmp, 'ui.mjs'), src);

/* fake host */
const sets = [];
let screenOps = 0;
const dsp = {};
globalThis.__leds = leds;
globalThis.host_module_set_param = (k, v) => { sets.push([k, v]); };
globalThis.host_module_get_param = k => dsp[k] ?? null;
globalThis.clear_screen = () => { screenOps++; };
globalThis.print = (x, y, t) => { check(typeof t === 'string', 'print non-string'); check(x >= 0 && x < 128 && y >= 0 && y < 64, `print off-screen ${x},${y}`); screenOps++; };
globalThis.fill_rect = () => { screenOps++; };
globalThis.draw_rect = () => { screenOps++; };
globalThis.text_width = t => t.length * 6;

dsp['tp:0'] = Array.from({ length: 32 }, (_, i) => i).join(',');
await import(pathToFileURL(path.join(tmp, 'ui.mjs')).href);
const ui = globalThis.__opgrid;

/* parameter table must match the C engine */
const dump = execFileSync(path.join(root, 'build/engine_test'), ['--dump-params']).toString().trim().split('\n');
check(dump.length === ui.PARAMS.length, `param count C=${dump.length} JS=${ui.PARAMS.length}`);
dump.forEach((line, i) => {
    const [def, max] = line.split(' ').map(Number);
    check(ui.PARAMS[i][3] === def, `default mismatch pid ${i}: C ${def} JS ${ui.PARAMS[i][3]}`);
    check(ui.PARAMS[i][2] === max, `max mismatch pid ${i}: C ${max} JS ${ui.PARAMS[i][2]}`);
    for (const v of [0, Math.floor(max / 2), max]) check(typeof ui.PARAMS[i][4](v) === 'string', `fmt pid ${i}`);
});

globalThis.init();
check(ui.values[0][5] === 5, 'pulled track 0 params from DSP');
for (let i = 0; i < 20; i++) globalThis.tick();
check(screenOps > 0, 'screen drawn');
check(leds.length > 0 && leds.length <= 20 * 12, 'LEDs sent progressively');

const midi = (...d) => globalThis.onMidiMessageInternal(d);

/* pad plays a note on track 0, release sends note off */
sets.length = 0;
midi(0x90, 68, 100);
check(sets[0]?.[0] === 'non' && sets[0][1] === '0 48 100', `pad note on: ${JSON.stringify(sets[0])}`);
midi(0x80, 68, 0);
check(sets[1]?.[0] === 'noff' && sets[1][1] === '0 48', `pad note off: ${JSON.stringify(sets[1])}`);

/* knob 1 on page 0 changes ALGO by one step, clamped */
sets.length = 0;
for (let i = 0; i < 20; i++) midi(0xB0, 71, 1);
check(ui.values[0][0] === 7, `algo clamps at 7: ${ui.values[0][0]}`);
check(sets.length === 7 - 0 && sets.at(-1)[1] === '0 0 7', `algo sends: ${JSON.stringify(sets.at(-1))}`);

/* jog to filter page, knob 1 edits cutoff */
midi(0xB0, 14, 1); midi(0xB0, 14, 1);
check(ui.state().page === 2, 'page 2 after two jog turns');
sets.length = 0;
midi(0xB0, 71, 127);       /* -1 */
check(sets[0]?.[1] === `0 16 ${16 - 1}`, `cutoff edit: ${JSON.stringify(sets[0])}`);

/* shift + step 5 selects track 5; pads then play track 4 (0-based) */
dsp['tp:4'] = Array.from({ length: 32 }, () => 10).join(',');
midi(0xB0, 49, 127);
midi(0x90, 20, 100);
midi(0xB0, 49, 0);
check(ui.state().track === 4, `track select via step: ${ui.state().track}`);
check(ui.values[4][5] === 10 && ui.values[4][0] === 7, 'pulled (and clamped) track 4 from DSP on select');
sets.length = 0;
midi(0x90, 70, 90);
check(sets[0]?.[1].startsWith('4 '), `pad plays selected track: ${JSON.stringify(sets[0])}`);

/* octave change while a pad is held: release still stops the held note */
midi(0xB0, 55, 127);
midi(0x80, 70, 0);
check(sets.at(-1)[0] === 'noff' && sets.at(-1)[1] === '4 50', `held note released correctly: ${JSON.stringify(sets.at(-1))}`);

/* knob touch notes are ignored */
sets.length = 0;
midi(0x90, 3, 127);
check(sets.length === 0, 'knob touch ignored');

/* run many ticks: no exceptions */
for (let i = 0; i < 2000; i++) globalThis.tick();
globalThis.onResume();
globalThis.onUnload();
check(sets.at(-1)[0] === 'panic', 'panic on unload');

fs.rmSync(tmp, { recursive: true, force: true });
if (failures) { console.log(`${failures} UI test(s) FAILED`); process.exit(1); }
console.log('all UI tests passed');
