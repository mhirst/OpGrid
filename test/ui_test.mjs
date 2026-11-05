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
/* in-memory filesystem shared by host_read_file/host_write_file and the os stub */
globalThis.__files = new Map();
fs.writeFileSync(path.join(tmp, 'os.mjs'), `
const files = () => globalThis.__files;
export function mkdir() { return 0; }
export function readdir(dir) {
    const names = [...files().keys()].filter(k => k.startsWith(dir + '/') && !k.slice(dir.length + 1).includes('/'))
        .map(k => k.slice(dir.length + 1));
    return [['.', '..', ...names], 0];
}
export function rename(a, b) { if (!files().has(a)) return -2; files().set(b, files().get(a)); files().delete(a); return 0; }
export function remove(a) { files().delete(a); return 0; }
`);
const src = fs.readFileSync(path.join(root, 'src/ui.js'), 'utf8')
    .replace('/data/UserData/schwung/shared/input_filter.mjs',
             pathToFileURL(path.join(tmp, 'input_filter.mjs')).href)
    .replace("from 'os'", `from '${pathToFileURL(path.join(tmp, 'os.mjs')).href}'`);
fs.writeFileSync(path.join(tmp, 'ui.mjs'), src);

/* ---- fake DSP implementing the param protocol (see src/og_plugin.c) ---- */
const dsp = {
    params: Array.from({ length: 16 }, (_, t) => Array.from({ length: 64 }, (_, i) => i === 61 ? 0 : t === 0 ? i % 30 : 10)),
    steps: Array.from({ length: 16 }, () => Array.from({ length: 64 }, () => [0, 60, 0, 0, 0, 100, 4, 1, 0, 0])),
    locks: Array.from({ length: 16 }, () => new Map()),       /* "s:p" -> v */
    clip: Array.from({ length: 16 }, () => [16, 4, 50, 0]),
    mute: new Array(16).fill(0), solo: new Array(16).fill(0), play: 0, tempo: 120, sync: 1, fill: 0,
    fx: [7, 50, 64, 64, 80, 70, 10, 60, 60, 30, 100, 127, 32, 0, 0, 100],
    clips: Array.from({ length: 16 }, () => [3, 0, 0, 0, 0, 0, 0, 0]),   /* states, track-major */
    edits: new Array(16).fill(0), lq: 3, log: [],
    rec: [0, 0, -1, 1, 0, 1], cdumps: new Map(),
    songs: Array.from({ length: 8 }, () => ({ rows: [], loop: 1 })), sstat: [0, 0, 0, 0, 0, 0],
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
    case 'solo': dsp.solo[a[0]] = a[1]; break;
    case 'fx': dsp.fx[a[0]] = a[1]; break;
    case 'edit': dsp.edits[a[0]] = a[1]; break;
    case 'lq': dsp.lq = a[0]; break;
    case 'launch': for (let c = 0; c < 8; c++) if (dsp.clips[a[0]][c] === 3) dsp.clips[a[0]][c] = 1;
        dsp.clips[a[0]][a[1]] = 3; break;
    case 'stop': for (let c = 0; c < 8; c++) if (dsp.clips[a[0]][c] === 3) dsp.clips[a[0]][c] = 1; break;
    case 'scene': dsp.log.push(['scene', a[0]]); break;
    case 'stopall': dsp.log.push(['stopall']); break;
    case 'ccopy': dsp.clips[a[2]][a[3]] = Math.max(1, dsp.clips[a[2]][a[3]]); dsp.log.push(['ccopy', ...a]); break;
    case 'cclr': dsp.clips[a[0]][a[1]] = 0; break;
    case 'rec': dsp.rec[0] = a[0]; dsp.log.push(['rec', a[0]]); break;
    case 'rquant': dsp.rec[3] = a[0]; break;
    case 'rmode': dsp.rec[4] = a[0]; break;
    case 'click': dsp.rec[5] = a[0]; break;
    case 'countin': dsp.log.push(['countin']); dsp.rec[1] = 1; break;
    case 'rnew': dsp.rec[0] = 1; dsp.rec[2] = a[0]; dsp.clips[a[0]][a[1]] = 2; dsp.log.push(['rnew', a[0], a[1]]); break;
    case 'rclose': dsp.rec[2] = -1; dsp.log.push(['rclose', a[0]]); break;
    case 'undo': dsp.log.push(['undo']); break;
    case 'rp': dsp.log.push(['rp', ...a]); break;
    case 'lnon': dsp.log.push(['lnon', ...a]); break;
    case 'tpset': { const vals = `${v}`.split(' ')[1].split(',').map(Number); dsp.params[a[0]] = vals; break; }
    case 'nclr': for (const st of dsp.steps[a[0]]) {
            if (!(st[0] & 1)) continue;
            const kept = st.slice(1, 5).filter(n => n && n !== a[1]);
            if (kept.length === st.slice(1, 5).filter(n => n).length) continue;
            if (!kept.length) st.splice(0, 10, ...reset()); else for (let k = 0; k < 4; k++) st[1 + k] = kept[k] || 0;
        } dsp.log.push(['nclr', ...a]); break;
    case 'csclr': dsp.steps[a[0]] = dsp.steps[a[0]].map(() => reset()); dsp.log.push(['csclr', a[0]]); break;
    case 'cload': { const parts = `${v}`.split(' ');
        try { const st = JSON.parse(Buffer.from(parts[2], 'hex').toString()); if (Array.isArray(st)) { dsp.steps[+parts[0]] = st; dsp.log.push(['cload-steps', +parts[0]]); break; } } catch (e) { /* not ours */ }
        dsp.cdumps.set(`${parts[0]}:${parts[1]}`, parts[2]);
        dsp.clips[+parts[0]][+parts[1]] = Math.max(1, dsp.clips[+parts[0]][+parts[1]]); break; }
    case 'sins': { const sg = dsp.songs[a[0]]; const row = a[2] >= 0
            ? { scene: a[2], bars: 4, reps: 1, tempo: 0, swing: 0, mutes: 0, clips: new Array(16).fill(a[2]) }
            : { ...(sg.rows[a[1] - 1] || { scene: 0, bars: 4, reps: 1, tempo: 0, swing: 0, mutes: 0, clips: new Array(16).fill(-1) }) };
        sg.rows.splice(a[1], 0, row); dsp.log.push(['sins', ...a]); break; }
    case 'sdel': dsp.songs[a[0]].rows.splice(a[1], 1); dsp.log.push(['sdel', ...a]); break;
    case 'srow': { const r = dsp.songs[a[0]].rows[a[1]]; const key = ['scene', 'bars', 'reps', 'tempo', 'swing', 'mutes'][a[2]];
        r[key] = a[3]; if (a[2] === 0) r.clips = new Array(16).fill(a[3]); dsp.log.push(['srow', ...a]); break; }
    case 'sloop': dsp.songs[a[0]].loop = a[1]; break;
    case 'song': dsp.sstat[0] = a[0]; if (a[0]) { dsp.sstat[1] = a[1]; dsp.sstat[2] = a[2]; } dsp.log.push(['song', ...a]); break;
    case 'sjump': dsp.log.push(['sjump', a[0]]); break;
    case 'capture': dsp.sstat[5] = a[0]; if (a[0]) dsp.songs[a[1]].rows = []; dsp.log.push(['capture', ...a]);
        if (!a[0]) dsp.songs[dsp.capDest ?? 0].rows.push({ scene: -1, bars: 2, reps: 1, tempo: 0, swing: 0, mutes: 0, clips: new Array(16).fill(1) });
        if (a[0]) dsp.capDest = a[1]; break;
    case 'sload': { const parts = `${v}`.split(' '); dsp.songs[+parts[0]].rows = JSON.parse(Buffer.from(parts[1], 'hex').toString()); break; }
    case 'reset':
        dsp.params = Array.from({ length: 16 }, () => new Array(64).fill(0));
        dsp.clips = Array.from({ length: 16 }, () => [3, 0, 0, 0, 0, 0, 0, 0]);
        dsp.cdumps = new Map(); dsp.tempo = 120; dsp.fx = dsp.fx.map(() => 0); dsp.play = 0;
        dsp.mute.fill(0); dsp.solo.fill(0); dsp.log.push(['reset']);
        dsp.songs = Array.from({ length: 8 }, () => ({ rows: [], loop: 1 })); break;
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
    if (k === 'fx') return dsp.fx.join(',');
    if (k === 'clips') return dsp.clips.map(r => r.join('')).join('');
    if (k === 'edits') return dsp.edits.join('');
    if (k === 'lq') return `${dsp.lq}`;
    if (k === 'rec') return dsp.rec.join(',');
    if (k === 'sstat') return dsp.sstat.join(',');
    if (k === 'slens') return dsp.songs.map(x => x.rows.length).join(',');
    if ((m = k.match(/^srow:(\d+):(\d+)$/))) { const r = dsp.songs[+m[1]].rows[+m[2]];
        return r ? [r.scene, r.bars, r.reps, r.tempo, r.swing, r.mutes, ...r.clips].join(',') : null; }
    if ((m = k.match(/^sloop:(\d+)$/))) return `${dsp.songs[+m[1]].loop}`;
    if ((m = k.match(/^sdump:(\d+)$/))) return Buffer.from(JSON.stringify(dsp.songs[+m[1]].rows)).toString('hex');
    if ((m = k.match(/^cdump:(\d+):(\d+)$/))) return dsp.cdumps.get(`${m[1]}:${m[2]}`) ||
        (dsp.useJsonDumps ? Buffer.from(JSON.stringify(dsp.steps[+m[1]])).toString('hex') : '10043200' + '00'.repeat(20));
    if ((m = k.match(/^notes:(\d+)$/))) { const set = new Set();
        for (const st of dsp.steps[+m[1]]) if (st[0] & 1) for (const n of st.slice(1, 5)) if (n) set.add(n);
        return [...set].join(','); }
    if (k === 'mix') return dsp.mute.join('') + dsp.solo.join('');
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
globalThis.host_write_file = (p, text) => { globalThis.__files.set(p, `${text}`); return true; };
globalThis.host_read_file = p => globalThis.__files.has(p) ? globalThis.__files.get(p) : null;
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
const jogTo = target => { let n = 0; while (globalThis.__opgrid.state().page !== target && n++ < 30) midi(0xB0, 14, globalThis.__opgrid.state().page < target ? 1 : 127); };

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
const [nConds, nLens, nSpeeds, nDests, nWaves, nSync, nDtimes] = execFileSync(bin, ['--dump-seq']).toString().trim().split(' ').map(Number);
check(ui.DEST_NAMES.length === nDests, `dests C=${nDests} JS=${ui.DEST_NAMES.length}`);
check(ui.WAVES.length === nWaves, `waves C=${nWaves} JS=${ui.WAVES.length}`);
check(ui.SYNC_RATES.length === nSync, `sync rates C=${nSync} JS=${ui.SYNC_RATES.length}`);
check(ui.FX[0][1] + 1 === nDtimes, `delay times C=${nDtimes} JS=${ui.FX[0][1] + 1}`);
const fxDump = execFileSync(path.join(root, 'build/fx_test'), ['--dump-fx']).toString().trim().split('\n');
check(fxDump.length === ui.FX.length, `fx count C=${fxDump.length} JS=${ui.FX.length}`);
fxDump.forEach((line, i) => {
    const [def, max] = line.split(' ').map(Number);
    check(ui.FX[i][2] === def && ui.FX[i][1] === max, `fx ${i}: C ${def}/${max} JS ${ui.FX[i][2]}/${ui.FX[i][1]}`);
    for (const v of [0, Math.floor(max / 2), max]) check(typeof ui.FX[i][3](v) === 'string' && ui.FX[i][3](v).length <= 5, `fx fmt ${i} -> ${ui.FX[i][3](v)}`);
});
/* machine tables match og_machines.c */
const mdump = execFileSync(path.join(root, 'build/machines_test'), ['--dump-machines']).toString().trim().split('\n');
for (const line of mdump) {
    const [m, pid, def, max] = line.split(' ').map(Number);
    if (m === 0) continue;
    const e = ui.MACH_PARAMS[m][pid];
    check(e[2] === max && e[3] === def, `machine ${m} pid ${pid}: C ${def}/${max} JS ${e[3]}/${e[2]}`);
    for (const v of [0, Math.floor(max / 2), max]) {
        const t = e[4](v);
        check(typeof t === 'string' && t.length <= 5, `machine ${m} pid ${pid} formats ${v} as "${t}"`);
    }
}
/* full names (shown on knob touch) fit the 120 px readout even at 6 px a character */
for (const e of [...ui.PARAMS, ...ui.MACH_PARAMS.filter(Boolean).flat()]) check(e[1].length <= 20, `long name too long: "${e[1]}"`);
for (const e of ui.FX) check(e[4] && e[4].length <= 20, `fx long name: "${e[4]}"`);
/* every param formatter fits a 5-char cell, with sync on and off */
ui.PARAMS.forEach((p, i) => {
    for (const v of [0, Math.floor(p[2] / 2), p[2]]) for (const sync of [0, 1]) {
        const vals = new Array(64).fill(sync);
        const t = p[4](v, vals);
        check(typeof t === 'string' && t.length <= 5, `param ${i} (${p[0]}) formats ${v} as "${t}"`);
    }
});
check(ui.CONDS.length === nConds, `conds C=${nConds} JS=${ui.CONDS.length}`);
check(ui.LENS.length === nLens, `lens C=${nLens} JS=${ui.LENS.length}`);
check(ui.SPEEDS.length === nSpeeds, `speeds C=${nSpeeds} JS=${ui.SPEEDS.length}`);
check(ui.CONDS[7] === '1:2' && ui.CONDS[41] === '8:8' && ui.CONDS[42] === '5%' && ui.CONDS[60] === '95%', 'cond names line up with ids');

/* ---- startup ---- */
globalThis.init();
check(ui.values[0][5] === 5, 'pulled track 0 params from DSP');
check(ui.fxValues[4] === 80, 'pulled FX values');
ticks(20);
check(screenOps > 0, 'screen drawn');
check(globalThis.__leds.length > 0 && globalThis.__leds.length <= 20 * 12, 'LEDs sent progressively');

/* ---- keys mode ---- */
sets.length = 0;
midi(0x90, 68, 100);
check(sets[0]?.[0] === 'lnon' && sets[0][1] === '0 48 100', `pad note on: ${JSON.stringify(sets[0])}`);
midi(0x80, 68, 0);
check(sets[1]?.[0] === 'lnoff' && sets[1][1] === '0 48', `pad note off: ${JSON.stringify(sets[1])}`);

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
check(dsp.steps[0][4].slice(1, 5).includes(52) && dsp.steps[0][4].slice(1, 5).includes(53), `hold step + pads adds chord notes: ${dsp.steps[0][4].slice(1, 5)}`);
/* Move-style melodic steps: a step press adds the selected note; a quick tap takes just it away */
{
    const notes = st => dsp.steps[0][st].slice(1, 5).filter(n => n > 0).sort((x, y) => x - y);
    midi(0x90, 68 + 2, 90); midi(0x80, 68 + 2, 0);                     /* select pad 2 (E) */
    const e = ui.state().selNote[0];
    midi(0x90, 16 + 12, 100); midi(0x80, 16 + 12, 0);
    check(notes(12).join() === `${e}`, `step gets the selected note: ${notes(12)}`);
    midi(0x90, 68 + 4, 90); midi(0x80, 68 + 4, 0);                     /* select pad 4 (G) */
    const gn = ui.state().selNote[0];
    midi(0x90, 16 + 12, 100); midi(0x80, 16 + 12, 0);
    check(notes(12).join() === `${e},${gn}` && dsp.steps[0][12][0] & 1, `pressing a step with another note adds a chord: ${notes(12)}`);
    midi(0x90, 16 + 12, 100); midi(0x80, 16 + 12, 0);
    check(notes(12).join() === `${e}` && dsp.steps[0][12][0] & 1, `quick tap removes only the selected note: ${notes(12)}`);
    midi(0x90, 68 + 2, 90); midi(0x90, 16 + 12, 100); midi(0x80, 16 + 12, 0); midi(0x80, 68 + 2, 0);
    check(!(dsp.steps[0][12][0] & 1), 'removing the last note clears the step');
    midi(0x90, 68 + 0, 90); midi(0x90, 68 + 2, 90); midi(0x90, 68 + 4, 90);
    midi(0x90, 16 + 13, 100); midi(0x80, 16 + 13, 0);
    midi(0x80, 68 + 0, 0); midi(0x80, 68 + 2, 0); midi(0x80, 68 + 4, 0);
    check(notes(13).length === 3, `pads held + step = chord step: ${notes(13)}`);
    midi(0x90, 16 + 13, 100); midi(0x90, 68 + 2, 90); midi(0x80, 68 + 2, 0); midi(0x80, 16 + 13, 0);
    check(notes(13).length === 2 && dsp.steps[0][13][0] & 1, `hold step + pad toggles a note out: ${notes(13)}`);
    midi(0xB0, 119, 127); midi(0x90, 16 + 13, 100); midi(0x80, 16 + 13, 0); midi(0xB0, 119, 0);
}

/* STEP page: hold step, knob 7 (COND) */
jogTo(12);
check(ui.state().page === 12, `step page: ${ui.state().page}`);
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
check(dsp.steps[0][9][1] === ui.state().selNote[0] && ui.state().selNote[0] > 0, `new step uses selected note: ${dsp.steps[0][9][1]}`);

/* Delete + step clears */
midi(0xB0, 119, 127); midi(0x90, 25, 100); midi(0x80, 25, 0); midi(0xB0, 119, 0);
check(dsp.steps[0][9][0] === 0, 'delete + step clears');

/* CLIP page: length 32, then Shift+step 2 pages */
jogTo(13);
check(ui.state().page === 13, 'clip page');
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

/* ---- milestone 4: LFO page, FX pages, mixer ---- */
jogTo(4);
check(ui.state().page === 4, 'LFO 1 page');
sets.length = 0;
midi(0xB0, 71, 1);                                        /* DEST +1 */
check(lastSet('p')?.[1] === `${ui.state().track} 32 ${Math.min(36, dsp.params[ui.state().track][32])}`, `lfo dest edit: ${JSON.stringify(lastSet('p'))}`);
ticks(8);
jogTo(7);
check(ui.state().page === 7, 'DELAY page');
midi(0xB0, 71, 1);                                        /* delay time +1 (stepped) */
check(dsp.fx[0] === 8, `delay time: ${dsp.fx[0]}`);
sets.length = 0;
const sendBefore = ui.values[ui.state().track][56];
midi(0xB0, 78, 10);                                       /* knob 8 on an effect page = this track's send */
check(lastSet('p')?.[1] === `${ui.state().track} 56 ${Math.min(127, sendBefore + 10)}`, `delay send on the delay page: ${JSON.stringify(lastSet('p'))}`);
jogTo(8);
midi(0xB0, 71, 5);                                        /* REVERB page: SIZE +5 */
check(dsp.fx[4] === 85, `reverb size: ${dsp.fx[4]}`);
jogTo(10);
midi(0xB0, 75, 127);                                       /* MASTER page knob 5: volume -1 */
check(dsp.fx[15] === 99, `master vol: ${dsp.fx[15]}`);
/* effects are shared: holding a step does not lock them */
sets.length = 0;
jogTo(7);
midi(0x90, 20, 100); midi(0xB0, 72, 1); midi(0x80, 20, 0);
check(!sets.some(x => x[0] === 'lock' || x[0] === 'fx'), 'no lock on a shared effect');
ticks(8);

/* mixer: tap Mute */
midi(0xB0, 88, 127); midi(0xB0, 88, 0);
check(ui.state().mixer, 'mute tap enters mixer');
midi(0x90, 69, 100);                                       /* bottom pad 2: mute track 2 */
check(dsp.mute[1] === 1, 'mixer pad mutes');
midi(0xB0, 49, 127); midi(0x90, 70, 100); midi(0xB0, 49, 0);
check(dsp.solo[2] === 1, 'shift + mixer pad solos');
midi(0x90, 68 + 16 + 5, 100);                              /* top pad 6: select track 6 */
check(ui.state().track === 5, `mixer top pad selects track: ${ui.state().track}`);
sets.length = 0;
midi(0xB0, 73, 3);                                         /* knob 3 = track 3 volume */
check(lastSet('p')?.[1] === '2 30 13', `mixer volume: ${JSON.stringify(lastSet('p'))}`);
midi(0xB0, 63, 127);
check(ui.state().mixBank === 1, 'right = tracks 9-16');
midi(0xB0, 71, 1);
check(lastSet('p')?.[1].startsWith('8 30 '), `bank 2 knob 1 = track 9: ${JSON.stringify(lastSet('p'))}`);
ticks(40);
midi(0xB0, 88, 127); midi(0xB0, 88, 0);
check(!ui.state().mixer, 'mute tap leaves mixer');
/* Mute + pad in keys mode mutes but does not toggle the mixer */
midi(0xB0, 88, 127); midi(0x90, 68, 100); midi(0xB0, 88, 0);
check(!ui.state().mixer && dsp.mute[0] === 1, 'mute+pad mutes without entering mixer');
midi(0xB0, 88, 127); midi(0x90, 68, 100); midi(0xB0, 88, 0);
midi(0xB0, 62, 127);                                       /* back to page via left */

/* ---- milestone 5: clip launch grid ---- */
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
check(ui.state().clipMode, 'Loop enters clip mode');
ticks(20);
/* top-left pad = track 1, scene 1 (playing), pad 68 = bottom-left = track 1, scene 4 */
const padFor = (t, sc) => 68 + (3 - (sc % 4)) * 8 + (t % 8);
midi(0x90, padFor(2, 1), 100);                              /* empty slot: select for editing, no launch */
check(dsp.edits[2] === 1 && ui.state().track === 2, `empty slot selected for editing: ${dsp.edits}`);
check(dsp.clips[2][1] === 0, 'empty slot not launched');
dsp.clips[2][1] = 1;                                       /* now it has content */
midi(0x90, padFor(2, 1), 100);
check(dsp.clips[2][1] === 3 && dsp.clips[2][0] === 1, 'tapping a clip with content launches it');
midi(0xB0, 88, 127); midi(0x90, padFor(2, 0), 100); midi(0xB0, 88, 0);
check(dsp.clips[2][1] === 1 && !ui.state().mixer, 'Mute + pad stops the track (and does not open mixer)');
midi(0xB0, 49, 127); midi(0x90, padFor(5, 2), 100); midi(0xB0, 49, 0);
check(JSON.stringify(dsp.log.at(-1)) === '["scene",2]', `Shift + pad launches scene 3: ${JSON.stringify(dsp.log.at(-1))}`);
midi(0xB0, 60, 127); midi(0x90, padFor(2, 1), 100); midi(0x90, padFor(6, 3), 100); midi(0xB0, 60, 0);
check(JSON.stringify(dsp.log.at(-1)) === '["ccopy",2,1,6,3]', `Copy: ${JSON.stringify(dsp.log.at(-1))}`);
midi(0xB0, 119, 127); midi(0x90, padFor(6, 3), 100); midi(0xB0, 119, 0);
check(dsp.clips[6][3] === 0, 'Delete + pad clears');
midi(0xB0, 54, 127); midi(0xB0, 63, 127);
check(ui.state().sceneBank === 1 && ui.state().trackBank === 1, 'arrows page the grid');
midi(0x90, padFor(9, 5), 100);
check(ui.state().track === 9 && dsp.edits[9] === 5, `bank 2 pad selects track 10 clip 6: track ${ui.state().track} edit ${dsp.edits[9]}`);
midi(0xB0, 55, 127); midi(0xB0, 62, 127);
dsp.play = 1;
midi(0xB0, 49, 127); midi(0xB0, 85, 127); midi(0xB0, 49, 0);
check(JSON.stringify(dsp.log.at(-1)) === '["stopall"]', 'Shift + Play stops all clips');
dsp.play = 0;
for (let i = 0; i < 300; i++) globalThis.tick();          /* draw grid with blinking */
midi(0xB0, 71, 1); ticks(8);                               /* knob in clip mode shows the page briefly */
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
check(!ui.state().clipMode, 'Loop leaves clip mode');
jogTo(13);
midi(0xB0, 76, 1);                                          /* CLIP page knob 6 = launch quantize */
check(dsp.lq === 4, `launch quantize 2 bars: ${dsp.lq}`);

/* ---- milestone 6: recording ---- */
const logHas = (...e) => dsp.log.some(x => JSON.stringify(x) === JSON.stringify(e));
dsp.play = 0;
midi(0xB0, 86, 127); midi(0xB0, 86, 0);
check(ui.state().recArmed && dsp.rec[0] === 1, 'Rec arms recording');
check(lastSet('rtrack')?.[1] === `${ui.state().track}`, 'record track follows the selected track');
midi(0xB0, 49, 127); midi(0xB0, 86, 127); midi(0xB0, 49, 0);
check(ui.state().clickMode === 2 && dsp.rec[5] === 2, `Shift+Rec cycles click: ${ui.state().clickMode}`);
midi(0xB0, 85, 127);
check(logHas('countin') && ui.state().playing, 'Play while armed with click = count-in');
dsp.rec[1] = 0; dsp.play = 1;
ticks(32);
check(!ui.state().countingIn, 'count-in finished');
jogTo(2);
sets.length = 0;
midi(0x90, 70, 100); midi(0x80, 70, 0);
check(sets.some(x => x[0] === 'lnon'), 'pads send live notes (recordable)');
midi(0xB0, 71, 3);
check(lastSet('rp')?.[1].split(' ')[1] === '16', `knob while recording sends a recorded lock: ${JSON.stringify(lastSet('rp'))}`);
midi(0x90, 20, 100); sets.length = 0; midi(0x90, 72, 100); midi(0x80, 72, 0); midi(0x80, 20, 0);
check(sets.some(x => x[0] === 'non') && !sets.some(x => x[0] === 'lnon'), 'held-step audition is never recorded');
midi(0xB0, 56, 127);
check(logHas('undo'), 'Undo');
/* new clip in the grid while armed */
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
const t7 = 7;
midi(0x90, padFor(t7, 6 % 4 + (ui.state().sceneBank ? 0 : 0)), 100);
const sc = (ui.state().sceneBank * 4) + (6 % 4);
check(logHas('rnew', t7, sc), `empty slot while armed records a new clip: ${JSON.stringify(dsp.log.slice(-3))}`);
ticks(16);
check(ui.state().growTrack === t7, 'UI knows the clip is growing');
midi(0x90, padFor(t7, 6 % 4), 100);
check(logHas('rclose', t7), 'second tap fixes the length');
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
midi(0xB0, 86, 127);
check(!ui.state().recArmed && dsp.rec[0] === 0, 'Rec disarms');
midi(0xB0, 85, 127);                                        /* stop */
check(dsp.play === 0, 'stopped');

/* ---- milestone 7: song mode ---- */
dsp.play = 0;
midi(0xB0, 49, 127); midi(0xB0, 58, 127); midi(0xB0, 49, 0);
check(ui.state().songView, 'Shift+Loop opens the song view');
ticks(8);
midi(0xB0, 3, 127);                                          /* click "+ ADD ROW" on an empty song */
check(logHas('sins', 0, 0, 0) && ui.state().songRows.length === 1, `add first row from scene 1: ${JSON.stringify(dsp.log.slice(-2))}`);
midi(0x90, 68 + 24 + 2, 100);                                /* top-row pad 3 = scene 3 */
check(ui.state().songRows[0].scene === 2, 'top pads choose the scene');
midi(0xB0, 72, 3);                                           /* BARS +3 */
check(ui.state().songRows[0].bars === 5, `bars step one per detent: ${ui.state().songRows[0].bars}`);
midi(0xB0, 73, 1);                                           /* REPS +1 */
midi(0xB0, 74, 1);                                           /* TMPO from OFF -> current tempo */
check(ui.state().songRows[0].reps === 2 && ui.state().songRows[0].tempo >= 30, `reps/tempo: ${JSON.stringify(ui.state().songRows[0])}`);
midi(0x90, 68 + 4, 100);                                     /* bottom pad 5: mute track 5 in this row */
check(ui.state().songRows[0].mutes === 16, `row mutes: ${ui.state().songRows[0].mutes}`);
midi(0xB0, 60, 127); midi(0xB0, 60, 0);                      /* Copy = duplicate row */
check(ui.state().songRows.length === 2 && ui.state().songSel === 1, 'Copy duplicates the row');
midi(0xB0, 119, 127); midi(0xB0, 119, 0);                    /* Delete removes it */
check(ui.state().songRows.length === 1, 'Delete removes the row');
midi(0xB0, 77, 127);                                         /* LOOP off */
check(dsp.songs[0].loop === 0, 'loop off');
midi(0xB0, 14, 127);                                         /* jog back to row 1 */
ticks(8);
midi(0xB0, 85, 127);                                         /* Play = song from the selected row */
check(logHas('song', 1, 0, 0) && ui.state().playing, `play starts the song: ${JSON.stringify(dsp.log.slice(-3))}`);
dsp.play = 1; ticks(20);
check(ui.state().songOn, 'UI sees song mode running');
midi(0xB0, 3, 127);
check(logHas('sjump', 0), 'jog click jumps while the song plays');
/* clip grid launches are off while the song plays */
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
sets.length = 0;
midi(0x90, padFor(0, 1), 100);
check(!sets.some(x => x[0] === 'launch'), 'no launching from the grid in song mode');
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
midi(0xB0, 85, 127); dsp.play = 0; dsp.sstat[0] = 0;            /* stop */
/* capture into the first empty song */
midi(0xB0, 52, 127);
check(logHas('capture', 1, 1) && ui.state().capturing, `capture starts into song 2: ${JSON.stringify(dsp.log.slice(-2))}`);
ticks(20);
midi(0xB0, 52, 127);
check(logHas('capture', 0) && !ui.state().capturing && ui.state().songRows.length === 1, 'capture stops and shows the captured song');
midi(0xB0, 85, 127); midi(0xB0, 85, 127); dsp.play = 0;
midi(0xB0, 49, 127); midi(0xB0, 58, 127); midi(0xB0, 49, 0);
check(!ui.state().songView && logHas('song', 0), 'Shift+Loop leaves song mode');

/* machines: Shift + jog switches the track's machine */
jogTo(0);
sets.length = 0;
midi(0xB0, 49, 127); midi(0xB0, 14, 1); midi(0xB0, 49, 0);
check(lastSet('mach')?.[1] === `${ui.state().track} 1`, `Shift+jog selects FM Perc: ${JSON.stringify(lastSet('mach'))}`);
dsp.params[ui.state().track][61] = 0;                        /* fake DSP: back to FM Poly */

/* sound browser: jog click on a sound page; turning previews, click keeps, Back restores */
globalThis.__files.set('/data/UserData/schwung/modules/overtake/opgrid/sounds.json',
    fs.readFileSync(path.join(root, 'module/sounds.json'), 'utf8'));
jogTo(0);
ticks(400);
midi(0xB0, 3, 127);                                           /* jog click opens the browser */
check(ui.state().browser && ui.state().browser.cat === 'DRUMS', `browser opens on DRUMS: ${JSON.stringify(ui.state().browser)}`);
sets.length = 0;
midi(0xB0, 14, 1);                                            /* next sound: previews at once */
const tp = lastSet('tpset');
check(tp && tp[1].split(' ')[1].split(',').length === 64 && tp[1].split(' ')[1].split(',')[61] === '1',
      `turning loads the next drum sound (FM Perc): ${tp && tp[1].slice(0, 30)}`);
check(ui.state().soundName[ui.state().track] === 'KICK PUNCH', `track shows the sound name: ${ui.state().soundName[ui.state().track]}`);
midi(0xB0, 63, 127);                                          /* Right: next category */
check(ui.state().browser.cat === 'BASS' && ui.state().browser.item === 'FM BASS', `category: ${JSON.stringify(ui.state().browser)}`);
ticks(8);
midi(0xB0, 3, 127);                                           /* click keeps it */
check(!ui.state().browser && ui.state().soundName[ui.state().track] === 'FM BASS', 'kept FM BASS');
const keptParams = [...dsp.params[ui.state().track]];
midi(0xB0, 3, 127); midi(0xB0, 14, 1); midi(0xB0, 14, 1);     /* browse away ... */
check(ui.state().soundName[ui.state().track] !== 'FM BASS', 'previewing another sound');
midi(0xB0, 51, 127);                                          /* ... then Back: put FM BASS back */
check(!ui.state().browser && ui.state().soundName[ui.state().track] === 'FM BASS'
      && JSON.stringify(dsp.params[ui.state().track]) === JSON.stringify(keptParams), 'Back restores the previous sound exactly');
/* editing marks the sound as changed (shown with a *) */
midi(0xB0, 71, 1);
check(ui.state().soundEdited[ui.state().track], 'edit marks the sound changed');
/* save the track sound from the menu; it shows up under USER */
midi(0xB0, 50, 127);
check(ui.state().menuItems[0].startsWith('BROWSE SOUNDS') && ui.state().menuItems[1].startsWith('SAVE T'), `menu: ${ui.state().menuItems.slice(0, 3)}`);
midi(0xB0, 14, 1); midi(0xB0, 3, 127);
check([...globalThis.__files.keys()].some(k => k.startsWith('/data/UserData/schwung/opgrid/sounds/SOUND 01')), 'track sound saved');
midi(0xB0, 3, 127);                                           /* browse again */
for (let i = 0; i < 8; i++) midi(0xB0, 63, 127);
check(ui.state().browser.cat === 'USER' && ui.state().browser.item === 'SOUND 01', `user sound listed: ${JSON.stringify(ui.state().browser)}`);
midi(0xB0, 50, 127);                                          /* Menu cancels too */
check(!ui.state().browser, 'Menu closes the browser');
/* knob touch shows the full name; Back leaves modes */
midi(0x90, 0, 127); ticks(8);
check(ui.state().touchHeld === 0, 'knob touch tracked');
midi(0x80, 0, 0);
midi(0xB0, 58, 127); midi(0xB0, 58, 0);
check(ui.state().clipMode, 'clip grid');
midi(0xB0, 51, 127);
check(!ui.state().clipMode, 'Back leaves the clip grid');

/* ---- trig workflow: deleting, undo, pad lighting ---- */
dsp.useJsonDumps = true;
jogTo(2);
const T = ui.state().track;
dsp.steps[T] = dsp.steps[T].map(() => [0, 60, 0, 0, 0, 100, 4, 1, 0, 0]);
for (const st of [0, 4, 8]) { midi(0x90, 16 + st, 100); midi(0x80, 16 + st, 0); }
ticks(8);
check([0, 4, 8].every(st => dsp.steps[T][st][0] & 1), 'three steps placed');
/* step held, then Delete */
midi(0x90, 16 + 4, 100); midi(0xB0, 119, 127); midi(0xB0, 119, 0); midi(0x80, 16 + 4, 0);
ticks(8);
check(!(dsp.steps[T][4][0] & 1), 'hold step + Delete clears it');
check(!(ui.stepFlags[T][4] & 1), 'and it stays cleared on release');
/* Undo brings it back */
midi(0xB0, 56, 127);
check(dsp.steps[T][4][0] & 1, 'Undo restores the deleted step');
/* Delete held, then step */
midi(0xB0, 119, 127); midi(0x90, 16 + 8, 100); midi(0x80, 16 + 8, 0); midi(0xB0, 119, 0);
check(!(dsp.steps[T][8][0] & 1), 'Delete + step clears it');
/* Delete + pad removes that note everywhere */
const NOTE = dsp.steps[T][0][1];                              /* whatever new steps got */
const notePad = [...Array(32).keys()].find(i => ui.padToNote(i) === NOTE);
midi(0xB0, 119, 127); midi(0x90, 68 + notePad, 100); midi(0x80, 68 + notePad, 0); midi(0xB0, 119, 0);
check(![0, 4].some(st => dsp.steps[T][st][0] & 1) && logHas('nclr', T, NOTE), 'Delete + pad removes the note from the clip');
midi(0xB0, 56, 127);
check(dsp.steps[T][0][0] & 1, 'Undo after Delete + pad');
/* Shift + Delete clears the whole clip */
midi(0xB0, 49, 127); midi(0xB0, 119, 127); midi(0xB0, 119, 0); midi(0xB0, 49, 0);
check(logHas('csclr', T) && !dsp.steps[T].some(st => st[0] & 1), 'Shift + Delete clears the clip');
midi(0xB0, 56, 127);
check(dsp.steps[T].filter(st => st[0] & 1).length >= 2, 'Undo after clearing the clip');
/* pads: notes used in the clip bright, others dark */
ticks(60);
globalThis.__leds.length = 0;
globalThis.onResume();
ticks(30);
const ledOf = n => { let c; for (const [k, id, col] of globalThis.__leds) if (k === 'n' && id === n) c = col; return c; };
const usedCol = ledOf(68 + notePad);
const otherPad = [...Array(32).keys()].find(i => i !== notePad && ui.padToNote(i) !== ui.state().selNote[T] && ui.padToNote(i) % 12 !== 0 && ![0, 4].some(st => dsp.steps[T][st][1] === ui.padToNote(i)));
check(usedCol !== undefined && usedCol !== ledOf(68 + otherPad), `used-note pad lit differently (${usedCol} vs ${ledOf(68 + otherPad)})`);
check(ledOf(68 + otherPad) >= 65, `unused pads use a dark shade (${ledOf(68 + otherPad)})`);
dsp.useJsonDumps = false;

/* regression: FILTER page knob 1 (cutoff, 0-127) must use acceleration */
jogTo(2);
const before = dsp.params[ui.state().track][16];
midi(0xB0, 71, 5);
check(dsp.params[ui.state().track][16] === Math.min(127, before + 5), `cutoff knob accelerates (${before} -> ${dsp.params[ui.state().track][16]})`);

/* ---- milestone 6: projects ---- */
dsp.params[3][16] = 77; dsp.fx[4] = 55; dsp.tempo = 101; dsp.clips[3][2] = 1;
dsp.cdumps.set('3:2', 'deadbeef00');
midi(0xB0, 50, 127);
check(ui.state().menuOpen && ui.state().menuItems[2].startsWith('SAVE PROJECT'), `menu opens: ${ui.state().menuItems}`);
midi(0xB0, 14, 1); midi(0xB0, 14, 1);
midi(0xB0, 3, 127);                                          /* SAVE */
check(!ui.state().menuOpen && ui.state().job, 'save runs as a job');
ticks(200);
check(!ui.state().job, 'save finished');
const saved = [...globalThis.__files.keys()].find(k => k.includes('/projects/') && k.endsWith('.json'));
check(!!saved, `project file written: ${[...globalThis.__files.keys()]}`);
const pdata = saved ? JSON.parse(globalThis.__files.get(saved)) : null;
check(pdata && pdata.tracks.length === 16 && pdata.tracks[3].params[16] === 77 && pdata.fx[4] === 55 && pdata.tempo === 101,
      'saved params, fx and tempo');
check(pdata && pdata.tracks[3].clips['2'] === 'deadbeef00', 'saved clip data');
check(pdata && Object.keys(pdata.tracks[5].clips).length === 0, 'empty clips are not saved');
check(!ui.state().projectDirty, 'save clears the dirty flag');

/* change things, then load the project back */
midi(0xB0, 50, 127);
for (let i = 0; i < 4; i++) midi(0xB0, 14, 1);               /* NEW PROJECT */
check(ui.state().menuItems[4] === 'NEW PROJECT', `menu has NEW PROJECT: ${ui.state().menuItems}`);
midi(0xB0, 3, 127);
check(ui.state().menuOpen, 'destructive items need a second click');
midi(0xB0, 3, 127);
check(logHas('reset') && ui.state().soundName[0] === 'KICK DEEP' && ui.state().soundName[4] === 'FM BASS',
      `new project starts with the starter kit: ${ui.state().soundName.slice(0, 8)}`);
midi(0xB0, 50, 127);
for (let i = 0; i < 5; i++) midi(0xB0, 14, 1);               /* first LOAD entry */
check(ui.state().menuItems[5]?.startsWith('LOAD '), `load entry listed: ${ui.state().menuItems[5]}`);
midi(0xB0, 3, 127); midi(0xB0, 3, 127);
ticks(200);
check(dsp.params[3][16] === 77 && dsp.fx[4] === 55 && dsp.tempo === 101, `load restores params/fx/tempo: ${dsp.params[3][16]} ${dsp.fx[4]} ${dsp.tempo}`);
check(dsp.cdumps.get('3:2') === 'deadbeef00', 'load restores clips');
check(dsp.songs[0].rows.length === 1 && dsp.songs[1].rows.length === 1, `load restores songs: ${dsp.songs.map(x => x.rows.length)}`);

/* autosave after ~2 minutes when something changed, and on unload */
jogTo(0);
midi(0xB0, 72, 1);
check(ui.state().projectDirty, 'edits mark the project dirty');
ui.tick(344 * 120 + 400);
check(globalThis.__files.has('/data/UserData/schwung/opgrid/autosave.json'), 'autosave written');
globalThis.__files.delete('/data/UserData/schwung/opgrid/autosave.json');
midi(0xB0, 72, 1);
globalThis.onUnload();
check(globalThis.__files.has('/data/UserData/schwung/opgrid/autosave.json'), 'autosave on unload');
/* a fresh init restores the autosave */
dsp.log.length = 0;
globalThis.init();
ticks(200);
check(logHas('reset'), 'init loads the autosave');

/* shift + pad selects track and pulls its data */
dsp.params[4] = new Array(64).fill(10); dsp.params[4][61] = 0;
midi(0xB0, 49, 127); midi(0x90, 72, 100); midi(0xB0, 49, 0);
check(ui.state().track === 4, `track select: ${ui.state().track}`);
check(ui.values[4][5] === 10 && ui.values[4][0] === 7, `pulled (and clamped) track 4 params: ${ui.values[4][5]} ${ui.values[4][0]}`);
sets.length = 0;
midi(0x90, 70, 90);
check(sets[0]?.[1].startsWith('4 '), `pad plays selected track: ${JSON.stringify(sets[0])}`);
midi(0xB0, 55, 127);
midi(0x80, 70, 0);
check(lastSet('lnoff')?.[1] === '4 52', `held note released after octave change: ${JSON.stringify(lastSet('lnoff'))}`);

sets.length = 0;
midi(0x90, 3, 127);
check(sets.length === 0, 'knob touch ignored');
midi(0x80, 3, 0);

/* pads: in key by default, tap selects the note; Shift + arrows change the key */
{
    const o = ui.state().octave;
    midi(0xB0, 54, 127); midi(0xB0, 54, 0);                  /* undo the octave change above */
    const base = 36 + ui.state().octave * 12;
    check(ui.padToNote(0) === base && ui.padToNote(1) === base + 2 && ui.padToNote(8) === base + 5, `C major grid: ${ui.padToNote(1)} ${ui.padToNote(8)}`);
    midi(0x90, 68 + 2, 90); midi(0x80, 68 + 2, 0);
    check(ui.state().selNote[4] === base + 4, `pad tap selects note: ${ui.state().selNote[4]}`);
    midi(0xB0, 49, 127); midi(0xB0, 63, 127); midi(0xB0, 63, 0); midi(0xB0, 54, 127); midi(0xB0, 54, 0); midi(0xB0, 49, 0);
    check(ui.state().scaleRoot === 1 && ui.state().scaleType === 1 && ui.state().octave === o - 1, `Shift+Right/Down: root ${ui.state().scaleRoot} scale ${ui.state().scaleType}`);
    check(ui.padToNote(0) === base + 1 && ui.padToNote(1) === base + 3, `C# minor grid: ${ui.padToNote(1)}`);
    midi(0xB0, 49, 127); midi(0xB0, 62, 127); midi(0xB0, 62, 0); midi(0xB0, 55, 127); midi(0xB0, 55, 0); midi(0xB0, 49, 0);
    check(ui.state().scaleRoot === 0 && ui.state().scaleType === 0, 'key back to C major');
}

/* FM Perc track: drum grid. Left 4x4 = tracks, right 4x4 = velocities */
{
    dsp.params[4][61] = 1; ui.values[4][61] = 1;
    sets.length = 0;
    midi(0x90, 68 + 9, 100);                                  /* row 1, col 1 -> track 6 */
    check(sets.some(([k, v]) => k === 'lnon' && v.startsWith('5 ')) && ui.state().track === 5, `drum pad plays + selects track 6: ${JSON.stringify(sets[0])} t${ui.state().track}`);
    midi(0x80, 68 + 9, 0);
    check(lastSet('lnoff')?.[1].startsWith('5 '), 'drum pad releases its note');
    midi(0xB0, 49, 127); midi(0x90, 68 + 4, 100); midi(0xB0, 49, 0);   /* back to track 5 */
    dsp.params[4][61] = 1; ui.values[4][61] = 1;
    sets.length = 0;
    midi(0x90, 68 + 31, 100); midi(0x80, 68 + 31, 0);      /* top-right velocity pad = 127 */
    midi(0x90, 68 + 4, 100); midi(0x80, 68 + 4, 0);        /* bottom-left velocity pad = 7 */
    check(sets.some(([k, v]) => k === 'lnon' && v === `4 60 127`), `velocity pad plays at 127: ${JSON.stringify(sets)}`);
    midi(0x90, 16 + 1, 100); midi(0x80, 16 + 1, 0);
    check(dsp.steps[4][1][5] === 7, `new step takes the velocity pad: ${dsp.steps[4][1][5]}`);
    check(dsp.steps[4][1][1] === 60, `drum steps use the drum's own pitch (60): ${dsp.steps[4][1][1]}`);
    midi(0x90, 16 + 1, 100); midi(0xB0, 55, 127); midi(0xB0, 55, 0); midi(0xB0, 55, 127); midi(0xB0, 55, 0); midi(0x80, 16 + 1, 0);
    check(dsp.steps[4][1][1] === 62 && dsp.steps[4][1][0] & 1, `hold step + Up transposes it: ${dsp.steps[4][1][1]}`);
    jogTo(0);
    sets.length = 0;
    midi(0xB0, 71, 127);
    check(sets.some(([k, v]) => k === 'p' && v.startsWith('4 10 ')), `drum page knob 1 = PTCH: ${JSON.stringify(sets)}`);
    dsp.params[4][61] = 0; ui.values[4][61] = 0;
}

/* every page draws cleanly, with and without a held step */
for (let p = 0; p < ui.PAGES.length; p++) {
    midi(0xB0, 14, 1); ticks(8);
    midi(0x90, 16, 100); midi(0xB0, 71, 1); ticks(8); midi(0x80, 16, 0); ticks(8);
}
ticks(2000);
globalThis.onResume();
globalThis.onUnload();
check(sets.some(x => x[0] === 'panic'), 'panic on unload');

fs.rmSync(tmp, { recursive: true, force: true });
if (failures) { console.log(`${failures} UI test(s) FAILED`); process.exit(1); }
console.log('all UI tests passed');
