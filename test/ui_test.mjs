/* Loads src/ui.js under a fake Schwung host + fake DSP and drives it like hardware. */
import fs from 'fs';
import path from 'path';
import os from 'os';
import { execFileSync } from 'child_process';
import { fileURLToPath, pathToFileURL } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let failures = 0;
const check = (c, msg) => { if (!c) { failures++; console.log('FAIL', msg); } };

/* ---- stub of Schwung's shared/input_filter.mjs ---- */
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'opgrid-ui-'));
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

/* ---- fake DSP implementing the param protocol (see src/og_plugin.c) ---- */
const dsp = {
    params: Array.from({ length: 16 }, (_, t) => Array.from({ length: 32 }, (_, i) => t === 0 ? i : 10)),
    steps: Array.from({ length: 16 }, () => Array.from({ length: 64 }, () => [0, 60, 0, 0, 0, 100, 4, 1, 0, 0])),
    locks: Array.from({ length: 16 }, () => new Map()),       /* "s:p" -> v */
    clip: Array.from({ length: 16 }, () => [16, 4, 50, 0]),
    mute: new Array(16).fill(0), play: 0, tempo: 120, sync: 1, fill: 0,
};
const reset = s => [0, 60, 0, 0, 0, 100, 4, 1, 0, 0];
const ni = v => `${v}`.split(' ').map(Number);
function dspSet(k, v) {
    const a = ni(v);
    switch (k) {
    case 'p': dsp.params[a[0]][a[1]] = a[2]; break;
    case 'play': dsp.play = a[0]; break;
    case 'tempo': dsp.tempo = a[0]; dsp.sync = 0; break;
    case 'sync': dsp.sync = a[0]; break;
    case 'fill': dsp.fill = a[0]; break;
    case 'mute': dsp.mute[a[0]] = a[1]; break;
    case 'sf': dsp.steps[a[0]][a[1]][a[2]] = a[3]; break;
    case 'sclr': dsp.steps[a[0]][a[1]] = reset();
        for (const key of [...dsp.locks[a[0]].keys()]) if (key.startsWith(`${a[1]}:`)) dsp.locks[a[0]].delete(key);
        break;
    case 'lock': dsp.locks[a[0]].set(`${a[1]}:${a[2]}`, a[3]); break;
    case 'clen': dsp.clip[a[0]][0] = a[1]; break;
    case 'cspd': dsp.clip[a[0]][1] = a[1]; break;
    case 'cswg': dsp.clip[a[0]][2] = a[1]; break;
    default: break;
    }
}
function dspGet(k) {
    let m;
    if ((m = k.match(/^tp:(\d+)$/))) return dsp.params[+m[1]].join(',');
    if ((m = k.match(/^steps:(\d+)$/))) {
        const t = +m[1];
        return dsp.steps[t].map((st, s) => {
            const has = [...dsp.locks[t].keys()].some(key => key.startsWith(`${s}:`));
            return String.fromCharCode(48 + ((st[0] & 7) | (has ? 8 : 0)));
        }).join('');
    }
    if ((m = k.match(/^step:(\d+):(\d+)$/))) return dsp.steps[+m[1]][+m[2]].join(',');
    if ((m = k.match(/^locks:(\d+):(\d+)$/))) {
        const out = [];
        for (const [key, v] of dsp.locks[+m[1]]) { const [s, p] = key.split(':'); if (+s === +m[2]) out.push(`${p}=${v}`); }
        return out.join(',');
    }
    if ((m = k.match(/^clip:(\d+)$/))) { const t = +m[1]; return [...dsp.clip[t].slice(0, 3), dsp.locks[t].size, dsp.mute[t]].join(','); }
    if (k === 'tempo') return `${dsp.tempo}.0`;
    if (k === 'sync') return `${dsp.sync}`;
    if (k === 'play') return `${dsp.play}`;
    if (k === 'pos') return new Array(16).fill(dsp.play ? 2 : -1).join(',');
    return null;
}

/* ---- fake host ---- */
const sets = [];
let screenOps = 0;
globalThis.__leds = [];
globalThis.host_module_set_param = (k, v) => { sets.push([k, v]); dspSet(k, v); };
globalThis.host_module_get_param = k => dspGet(k);
globalThis.clear_screen = () => { screenOps++; };
globalThis.print = (x, y, t) => {
    check(typeof t === 'string' && !t.includes('undefined') && !t.includes('NaN'), `bad print text: ${t}`);
    check(x >= 0 && x < 128 && y >= 0 && y < 64, `print off-screen ${x},${y}`);
    check(x + t.length * 6 <= 132, `text runs off the right edge: "${t}" at ${x}`);
    screenOps++;
};
globalThis.fill_rect = () => { screenOps++; };
globalThis.draw_rect = () => { screenOps++; };
globalThis.text_width = t => t.length * 6;

await import(pathToFileURL(path.join(tmp, 'ui.mjs')).href);
const ui = globalThis.__opgrid;
const midi = (...d) => globalThis.onMidiMessageInternal(d);
const ticks = n => { for (let i = 0; i < n; i++) globalThis.tick(); };
const lastSet = k => [...sets].reverse().find(s => s[0] === k);

/* ---- tables must match the C engine ---- */
const bin = path.join(root, 'build/engine_test');
const dump = execFileSync(bin, ['--dump-params']).toString().trim().split('\n');
check(dump.length === ui.PARAMS.length, `param count C=${dump.length} JS=${ui.PARAMS.length}`);
dump.forEach((line, i) => {
    const [def, max] = line.split(' ').map(Number);
    check(ui.PARAMS[i][3] === def, `default mismatch pid ${i}: C ${def} JS ${ui.PARAMS[i][3]}`);
    check(ui.PARAMS[i][2] === max, `max mismatch pid ${i}: C ${max} JS ${ui.PARAMS[i][2]}`);
    for (const v of [0, Math.floor(max / 2), max]) check(typeof ui.PARAMS[i][4](v) === 'string', `fmt pid ${i}`);
});
const [nConds, nLens, nSpeeds] = execFileSync(bin, ['--dump-seq']).toString().trim().split(' ').map(Number);
check(ui.CONDS.length === nConds, `conds C=${nConds} JS=${ui.CONDS.length}`);
check(ui.LENS.length === nLens, `lens C=${nLens} JS=${ui.LENS.length}`);
check(ui.SPEEDS.length === nSpeeds, `speeds C=${nSpeeds} JS=${ui.SPEEDS.length}`);
check(ui.CONDS[7] === '1:2' && ui.CONDS[41] === '8:8' && ui.CONDS[42] === '5%' && ui.CONDS[60] === '95%', 'cond names line up with ids');

/* ---- startup ---- */
globalThis.init();
check(ui.values[0][5] === 5, 'pulled track 0 params from DSP');
ticks(20);
check(screenOps > 0, 'screen drawn');
check(globalThis.__leds.length > 0 && globalThis.__leds.length <= 20 * 12, 'LEDs sent progressively');

/* ---- keys mode ---- */
sets.length = 0;
midi(0x90, 68, 100);
check(sets[0]?.[0] === 'non' && sets[0][1] === '0 48 100', `pad note on: ${JSON.stringify(sets[0])}`);
midi(0x80, 68, 0);
check(sets[1]?.[0] === 'noff' && sets[1][1] === '0 48', `pad note off: ${JSON.stringify(sets[1])}`);

/* ---- sound edit ---- */
for (let i = 0; i < 20; i++) midi(0xB0, 71, 1);
check(ui.values[0][0] === 7, `algo clamps at 7: ${ui.values[0][0]}`);
midi(0xB0, 14, 1); midi(0xB0, 14, 1);
check(ui.state().page === 2, 'jog to filter page');
sets.length = 0;
midi(0xB0, 71, 127);
check(sets[0]?.[1] === '0 16 15', `cutoff edit: ${JSON.stringify(sets[0])}`);

/* ---- steps: tap empty step creates it, stays after release ---- */
sets.length = 0;
midi(0x90, 16, 100); midi(0x80, 16, 0);
check(sets.some(s => s[0] === 'sf' && s[1] === '0 0 0 1'), 'empty step press creates a note step');
check((dsp.steps[0][0][0] & 1) === 1, 'step 1 is on in DSP');
ticks(8);
check((ui.stepFlags[0][0] & 1) === 1, 'UI pulled step flags');

/* tap an existing step removes it */
midi(0x90, 16, 100); midi(0x80, 16, 0);
check(dsp.steps[0][0][0] === 0, 'tap on existing step removes it');

/* hold existing step + knob = lock, release keeps the step */
midi(0x90, 20, 100); midi(0x80, 20, 0); ticks(8);         /* create step 5 */
sets.length = 0;
midi(0x90, 20, 100);
midi(0xB0, 72, 5);                                          /* filter page knob 2 = RESO (pid 17) */
check(lastSet('lock')?.[1] === '0 4 17 22', `lock sent: ${JSON.stringify(lastSet('lock'))}`);
check(ui.values[0][17] === 17, 'base value untouched by a lock');
ticks(8);
midi(0x80, 20, 0);
check((dsp.steps[0][4][0] & 1) === 1, 'edited step survives release');
check(dsp.locks[0].get('4:17') === 22, 'lock stored');
ticks(8);
check(ui.stepFlags[0][4] === (1 | 8), `step shows lock marker: ${ui.stepFlags[0][4]}`);

/* hold step + pads = notes / chord */
sets.length = 0;
midi(0x90, 20, 100);
midi(0x90, 70, 90); midi(0x90, 71, 90);
midi(0x80, 70, 0); midi(0x80, 71, 0);
midi(0x80, 20, 0);
check(dsp.steps[0][4][1] === 50 && dsp.steps[0][4][2] === 51, `chord notes: ${dsp.steps[0][4].slice(1, 5)}`);

/* STEP page: hold step, knob 7 (COND) */
midi(0xB0, 14, 1); midi(0xB0, 14, 1);
check(ui.state().page === 4, `step page: ${ui.state().page}`);
midi(0x90, 20, 100);
for (let i = 0; i < 7; i++) midi(0xB0, 77, 1);
midi(0xB0, 75, 1); midi(0xB0, 75, 1);                       /* MICRO +2 */
ticks(8);
midi(0x80, 20, 0);
check(dsp.steps[0][4][8] === 7, `cond set to 1:2 (id 7): ${dsp.steps[0][4][8]}`);
check(dsp.steps[0][4][9] === 2, `micro +2: ${dsp.steps[0][4][9]}`);

/* type LOCK keeps slide bit off and clears NOTE */
midi(0x90, 20, 100); midi(0xB0, 71, 1); midi(0x80, 20, 0);
check(dsp.steps[0][4][0] === 2, `type lock-only: flags ${dsp.steps[0][4][0]}`);

/* step defaults when nothing held */
midi(0xB0, 72, 5);                                           /* NOTE default +5 */
midi(0x90, 25, 100); midi(0x80, 25, 0);
check(dsp.steps[0][9][1] === 65, `new step uses default note: ${dsp.steps[0][9][1]}`);

/* Delete + step clears */
midi(0xB0, 119, 127); midi(0x90, 25, 100); midi(0x80, 25, 0); midi(0xB0, 119, 0);
check(dsp.steps[0][9][0] === 0, 'delete + step clears');

/* CLIP page: length 32, then Shift+step 2 pages */
midi(0xB0, 14, 1);
check(ui.state().page === 5, 'clip page');
midi(0xB0, 49, 127); midi(0x90, 17, 100); midi(0xB0, 49, 0);
check(ui.state().stepPage === 0, 'cannot page past clip length');
for (let i = 0; i < 16; i++) midi(0xB0, 71, 1);
check(dsp.clip[0][0] === 32, `clip length 32: ${dsp.clip[0][0]}`);
midi(0xB0, 49, 127); midi(0x90, 17, 100); midi(0x80, 17, 0); midi(0xB0, 49, 0);
check(ui.state().stepPage === 1, 'shift + step 2 = steps 17-32');
midi(0x90, 16, 100); midi(0x80, 16, 0);
check((dsp.steps[0][16][0] & 1) === 1, 'step 17 created on page 2');
midi(0xB0, 74, 3);                                            /* BPM */
check(dsp.tempo === 123 && dsp.sync === 0, `tempo 123, sync off: ${dsp.tempo} ${dsp.sync}`);
ticks(8);

/* transport, mute, fill */
midi(0xB0, 85, 127); midi(0xB0, 85, 0);
check(dsp.play === 1 && ui.state().playing, 'play starts');
ticks(40);
midi(0xB0, 88, 127); midi(0x90, 70, 100); midi(0x80, 70, 0); midi(0xB0, 88, 0);
check(dsp.mute[2] === 1, 'mute + pad 3 mutes track 3');
midi(0xB0, 49, 127); midi(0xB0, 88, 127);
check(dsp.fill === 1, 'shift + mute = fill');
midi(0xB0, 88, 0); midi(0xB0, 49, 0);
check(dsp.fill === 0, 'fill released');
midi(0xB0, 85, 127);
check(dsp.play === 0, 'play stops');

/* shift + pad selects track and pulls its data */
midi(0xB0, 49, 127); midi(0x90, 72, 100); midi(0xB0, 49, 0);
check(ui.state().track === 4, `track select: ${ui.state().track}`);
check(ui.values[4][5] === 10 && ui.values[4][0] === 7, 'pulled (and clamped) track 4 params');
sets.length = 0;
midi(0x90, 70, 90);
check(sets[0]?.[1].startsWith('4 '), `pad plays selected track: ${JSON.stringify(sets[0])}`);
midi(0xB0, 55, 127);
midi(0x80, 70, 0);
check(lastSet('noff')?.[1] === '4 50', `held note released after octave change: ${JSON.stringify(lastSet('noff'))}`);

sets.length = 0;
midi(0x90, 3, 127);
check(sets.length === 0, 'knob touch ignored');

/* every page draws cleanly, with and without a held step */
for (let p = 0; p < ui.PAGES.length; p++) {
    midi(0xB0, 14, 1); ticks(8);
    midi(0x90, 16, 100); midi(0xB0, 71, 1); ticks(8); midi(0x80, 16, 0); ticks(8);
}
ticks(2000);
globalThis.onResume();
globalThis.onUnload();
check(sets.at(-1)[0] === 'panic', 'panic on unload');

fs.rmSync(tmp, { recursive: true, force: true });
if (failures) { console.log(`${failures} UI test(s) FAILED`); process.exit(1); }
console.log('all UI tests passed');
