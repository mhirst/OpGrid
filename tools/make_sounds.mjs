/* Build the starter sound library: module/sounds.json (shipped) and
 * build/sounds.txt (for test/sounds_test.c). Each sound is a set of
 * overrides on its machine's defaults, taken from the engine itself. */
import fs from 'fs';
import path from 'path';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dump = execFileSync(path.join(root, 'build/engine_test'), ['--dump-params']).toString().trim().split('\n')
    .map(l => Number(l.split(' ')[0]));
const mdump = execFileSync(path.join(root, 'build/machines_test'), ['--dump-machines']).toString().trim().split('\n')
    .map(l => l.split(' ').map(Number));

/* shared param ids (og_engine.h) */
const P = {
    DTUN: 12, VELM: 13, TUNE: 14, GLID: 15,
    FREQ: 16, RESO: 17, FTYP: 18, FENV: 19, FATK: 20, FDEC: 21, FSUS: 22, FREL: 23,
    AATK: 24, ADEC: 25, ASUS: 26, AREL: 27, DRIV: 28, PAN: 29, VOL: 30, VELA: 31,
    L1DEST: 32, L1WAVE: 33, L1SYNC: 34, L1RATE: 35, L1DPTH: 36, L1TRIG: 37,
    L2DEST: 40, L2WAVE: 41, L2SYNC: 42, L2RATE: 43, L2DPTH: 44,
    SDLY: 56, SREV: 57, SCHO: 58, MACH: 61,
};
/* machine param names -> ids 0..11 */
const M = {
    poly: { id: 0, ALGO: 0, RAT1: 1, RAT2: 2, RAT3: 3, RAT4: 4, LVL2: 5, LVL3: 6, LVL4: 7, FDBK: 8, DEC2: 9, DEC3: 10, DEC4: 11 },
    perc: { id: 1, SWP: 0, SWPT: 1, RAT: 2, FM: 3, FMD: 4, NOIS: 5, NDEC: 6, COLR: 7, FDBK: 8, CLIK: 9 },
    morph: { id: 2, WAVA: 0, WAVB: 1, MIX: 2, PD: 3, TUNB: 4, RING: 5, NOIS: 6, DRFT: 7, SUBL: 8, SUBO: 9 },
    stack: { id: 3, WAVE: 0, VOIC: 1, DTUN: 2, ANIM: 3, SUBL: 4, SUBO: 5, NOIS: 6, MIX: 7 },
};
const FT = { LP12: 0, LP24: 1, HP: 2, BP: 3, NOTCH: 4 };
/* FM ratio table indices: .25 .5 .75 1 1.5 2 2.5 3 3.5 4 5 6 7 8 9 10 11 12 13 14 15 16 */
const R = { '0.5': 1, 1: 3, 1.5: 4, 2: 5, 3: 7, 3.5: 8, 4: 9, 5: 10, 6: 11, 7: 12, 8: 13, 9: 14, 11: 16, 12: 17, 14: 19 };
/* LFO destinations: 2 + param id; sync rates by index (steps per cycle) */
const DEST = id => 2 + id;
const RATE = idx => Math.floor(idx * 128 / 17) + 1;   /* 0 = 8 bars ... 14 = 1/16 */

function sound(name, machine, mp, shared = {}) {
    const m = M[machine];
    const p = [...dump];
    p[P.MACH] = m.id;
    for (let i = 0; i < 12; i++) p[i] = m.id ? mdump.find(r => r[0] === m.id && r[1] === i)[2] : dump[i];
    if (m.id === 1) Object.assign(p, { [P.AATK]: 0, [P.ADEC]: 78, [P.ASUS]: 0, [P.AREL]: 45 });
    for (const [k, v] of Object.entries(mp)) {
        if (!(k in m)) throw new Error(`${name}: ${k} is not a ${machine} param`);
        p[m[k]] = v;
    }
    for (const [k, v] of Object.entries(shared)) {
        if (!(k in P)) throw new Error(`${name}: unknown param ${k}`);
        p[P[k]] = v;
    }
    if (name.length > 14) throw new Error(`${name}: name longer than 14`);
    return { name, params: p };
}

const SOUNDS = [
    /* drums: FM Perc */
    sound('KICK DEEP', 'perc', { SWP: 75, SWPT: 62, FM: 10, FMD: 50 }, { ADEC: 86, VOL: 118 }),
    sound('KICK PUNCH', 'perc', { SWP: 92, SWPT: 52, FM: 40, FMD: 45, CLIK: 70 }, { ADEC: 80, VOL: 118 }),
    sound('KICK LONG', 'perc', { SWP: 60, SWPT: 70, FM: 6 }, { ADEC: 96, VOL: 115 }),
    sound('SNARE', 'perc', { SWP: 30, SWPT: 50, FM: 20, NOIS: 110, NDEC: 74, COLR: 92, CLIK: 80 }, { ADEC: 78, VOL: 100 }),
    sound('CLAP', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 72, COLR: 84, CLIK: 70 },
        { FTYP: FT.HP, FREQ: 82, RESO: 20, ADEC: 76, VOL: 127, SREV: 30 }),
    sound('HAT CLOSED', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 58, COLR: 124 },
        { FTYP: FT.HP, FREQ: 104, ADEC: 60, VOL: 90, PAN: 80 }),
    sound('HAT OPEN', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 80, COLR: 124 },
        { FTYP: FT.HP, FREQ: 102, ADEC: 84, VOL: 84, PAN: 80 }),
    sound('TOM', 'perc', { SWP: 40, SWPT: 74, FM: 15, FMD: 60 }, { ADEC: 86, VOL: 105 }),
    sound('RIM', 'perc', { SWP: 20, SWPT: 40, RAT: R[4], FM: 90, FMD: 40, CLIK: 100 }, { ADEC: 60, VOL: 100 }),
    sound('METAL', 'perc', { SWP: 0, RAT: R[14], FM: 90, FMD: 76, FDBK: 40 },
        { FTYP: FT.HP, FREQ: 70, RESO: 30, ADEC: 80, VOL: 120 }),
    /* FM Poly */
    sound('E-PIANO', 'poly', { ALGO: 4, RAT1: R[1], RAT2: R[12], LVL2: 35, DEC2: 50, RAT3: R[1], LVL3: 90, RAT4: R[1], LVL4: 40, DEC4: 60 },
        { ADEC: 86, ASUS: 40, AREL: 60, VELM: 100, SCHO: 40 }),
    sound('BELL', 'poly', { ALGO: 0, RAT1: R[1], RAT2: R[3.5], LVL2: 70, DEC2: 92, LVL3: 0, LVL4: 0 },
        { ADEC: 100, ASUS: 0, AREL: 92, SREV: 60, VOL: 95 }),
    sound('FM BASS', 'poly', { ALGO: 0, RAT2: R[1], LVL2: 80, DEC2: 50, RAT3: R[2], LVL3: 30, FDBK: 30 },
        { FTYP: FT.LP24, FREQ: 70, FENV: 90, FDEC: 45, ASUS: 70, AREL: 25 }),
    sound('SUB BASS', 'poly', { ALGO: 7, LVL2: 0, LVL3: 0, LVL4: 0 }, { FREQ: 127, ASUS: 110, AREL: 30, VOL: 115 }),
    sound('SQUELCH', 'poly', { ALGO: 0, RAT2: R[1], LVL2: 100, FDBK: 80, LVL3: 40 },
        { FTYP: FT.LP24, FREQ: 50, RESO: 100, FENV: 110, FDEC: 40, GLID: 20, ASUS: 90 }),
    sound('BRASS', 'poly', { ALGO: 3, RAT2: R[1], LVL2: 70, RAT3: R[1], LVL3: 60, DEC2: 100, DEC3: 100 },
        { AATK: 22, FENV: 80, FATK: 30, FREQ: 80, ASUS: 100 }),
    sound('ORGAN', 'poly', { ALGO: 7, RAT1: R[1], RAT2: R[2], RAT3: R[4], RAT4: R[8], LVL2: 90, LVL3: 60, LVL4: 40 },
        { ASUS: 127, AREL: 12, SCHO: 70, VOL: 85 }),
    sound('PLUCK', 'poly', { ALGO: 2, LVL2: 90, DEC2: 30, FDBK: 30, LVL3: 20, LVL4: 30, DEC4: 25 },
        { ADEC: 52, ASUS: 0, AREL: 40, SDLY: 45 }),
    sound('MARIMBA', 'poly', { ALGO: 0, RAT2: R[4], LVL2: 60, DEC2: 25, LVL3: 0 }, { ADEC: 56, ASUS: 0, AREL: 50 }),
    sound('GLASS PAD', 'poly', { ALGO: 5, RAT4: R[2], LVL4: 50, LVL2: 90, LVL3: 70, RAT2: R[2], RAT3: R[3] },
        { AATK: 70, AREL: 92, ASUS: 110, SREV: 85, VOL: 85, L1DEST: DEST(7), L1RATE: RATE(2), L1DPTH: 90 }),
    /* Morph */
    sound('MORPH BASS', 'morph', { WAVA: 36, PD: 60, SUBL: 90 },
        { FTYP: FT.LP24, FREQ: 55, FENV: 95, FDEC: 40, RESO: 60, ASUS: 70, AREL: 25 }),
    sound('VOX PAD', 'morph', { WAVA: 109, WAVB: 91, MIX: 60, DRFT: 60 },
        { AATK: 62, AREL: 86, ASUS: 110, SCHO: 60, SREV: 70, VOL: 90, L1DEST: DEST(0), L1RATE: RATE(1), L1DPTH: 84 }),
    sound('PD LEAD', 'morph', { WAVA: 50, PD: 100, TUNB: 75, MIX: 40 }, { GLID: 15, SDLY: 40, ASUS: 100 }),
    sound('RING KEYS', 'morph', { WAVA: 0, WAVB: 0, TUNB: 83, RING: 90 }, { ADEC: 72, ASUS: 30, AREL: 60 }),
    sound('HOLLOW', 'morph', { WAVA: 54, WAVB: 72, MIX: 50, DRFT: 40 }, { FREQ: 85, ASUS: 100, AREL: 50 }),
    /* Stack */
    sound('SUPERSAW', 'stack', { WAVE: 0, VOIC: 6, DTUN: 60, ANIM: 40 }, { FREQ: 95, SCHO: 40, SREV: 40, ASUS: 110, VOL: 88 }),
    sound('STRINGS', 'stack', { WAVE: 0, VOIC: 5, DTUN: 35 }, { AATK: 72, AREL: 86, FREQ: 80, SREV: 80, ASUS: 115, VOL: 88 }),
    sound('BIG STAB', 'stack', { WAVE: 1, VOIC: 7, DTUN: 80, SUBL: 80 }, { ADEC: 62, ASUS: 20, FENV: 90, FDEC: 40, FREQ: 70 }),
    sound('SQUARE PAD', 'stack', { WAVE: 1, VOIC: 3, DTUN: 25 }, { FREQ: 70, AATK: 50, AREL: 80, ASUS: 110 }),
    sound('SOFT CHOIR', 'stack', { WAVE: 2, VOIC: 5, DTUN: 30 }, { FREQ: 90, AATK: 80, AREL: 90, SREV: 90, ASUS: 110 }),
    sound('DRONE', 'stack', { WAVE: 0, VOIC: 7, DTUN: 100, ANIM: 100, SUBL: 60, NOIS: 20 },
        { ASUS: 127, AATK: 90, AREL: 100, VOL: 85 }),
    sound('RISER', 'stack', { WAVE: 0, VOIC: 3, DTUN: 70, NOIS: 127 },
        { FTYP: FT.LP24, FREQ: 85, RESO: 80, AATK: 70, ASUS: 127, AREL: 70, VOL: 120,
          L1DEST: DEST(P.FREQ), L1WAVE: 3, L1RATE: RATE(6), L1DPTH: 100, L1TRIG: 1 }),   /* one-bar sweep up */
];

const names = new Set();
for (const s of SOUNDS) { if (names.has(s.name)) throw new Error(`duplicate ${s.name}`); names.add(s.name); }
fs.writeFileSync(path.join(root, 'module/sounds.json'),
    JSON.stringify({ app: 'opgrid-sounds', format: 1, sounds: SOUNDS }, null, 0) + '\n');
fs.mkdirSync(path.join(root, 'build'), { recursive: true });
fs.writeFileSync(path.join(root, 'build/sounds.txt'), SOUNDS.map(s => `${s.name}|${s.params.join(',')}`).join('\n') + '\n');
console.log(`${SOUNDS.length} starter sounds -> module/sounds.json`);
