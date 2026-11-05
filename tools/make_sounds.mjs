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
    perc: { id: 1, SWP: 0, SWPT: 1, RAT: 2, FM: 3, FMD: 4, NOIS: 5, NDEC: 6, COLR: 7, FDBK: 8, CLIK: 9, PTCH: 10 },
    morph: { id: 2, WAVA: 0, WAVB: 1, MIX: 2, PD: 3, TUNB: 4, RING: 5, NOIS: 6, DRFT: 7, SUBL: 8, SUBO: 9 },
    stack: { id: 3, WAVE: 0, VOIC: 1, DTUN: 2, ANIM: 3, SUBL: 4, SUBO: 5, NOIS: 6, MIX: 7 },
};
const FT = { LP12: 0, LP24: 1, HP: 2, BP: 3, NOTCH: 4 };
/* FM ratio table indices: .25 .5 .75 1 1.5 2 2.5 3 3.5 4 5 6 7 8 9 10 11 12 13 14 15 16 */
const R = { '0.5': 1, 1: 3, 1.5: 4, 2: 5, 3: 7, 3.5: 8, 4: 9, 5: 10, 6: 11, 7: 12, 8: 13, 9: 14, 11: 16, 12: 17, 14: 19 };
/* LFO destinations: 2 + param id; sync rates by index (steps per cycle) */
const DEST = id => 2 + id;
const RATE = idx => Math.floor(idx * 128 / 17) + 1;   /* 0 = 8 bars ... 14 = 1/16 */

/* browser category for each sound */
const CAT = {
    DRUMS: ['KICK DEEP', 'KICK PUNCH', 'KICK LONG', 'SNARE', 'CLAP', 'HAT CLOSED', 'HAT OPEN', 'TOM', 'RIM', 'METAL'],
    BASS: ['FM BASS', 'SUB BASS', 'MORPH BASS'],
    KEYS: ['E-PIANO', 'BELL', 'ORGAN', 'PLUCK', 'MARIMBA', 'RING KEYS'],
    LEAD: ['SQUELCH', 'BRASS', 'PD LEAD', 'HOLLOW', 'SUPERSAW', 'BIG STAB'],
    PAD: ['GLASS PAD', 'VOX PAD', 'STRINGS', 'SQUARE PAD', 'SOFT CHOIR'],
    FX: ['DRONE', 'RISER'],
    DUB: ['DUB KICK', 'DUB KICK SOFT', 'DUB RIM', 'DUB HAT', 'DUB SHAKER', 'DUB CLAP', 'DUB CONGA',
        'DUB CHORD', 'DUB STAB FM', 'DUB PAD', 'DUB SUB', 'DUB BELL', 'DUB FOG'],
};
const catOf = name => Object.keys(CAT).find(c => CAT[c].includes(name));

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
    const cat = catOf(name);
    if (!cat) throw new Error(`${name}: no category`);
    return { name, cat, params: p };
}

const SOUNDS = [
    /* drums: FM Perc */
    sound('KICK DEEP', 'perc', { PTCH: 31, SWP: 75, SWPT: 62, FM: 10, FMD: 50 }, { ADEC: 86, VOL: 118 }),
    sound('KICK PUNCH', 'perc', { PTCH: 36, SWP: 92, SWPT: 52, FM: 40, FMD: 45, CLIK: 70 }, { ADEC: 80, VOL: 118 }),
    sound('KICK LONG', 'perc', { PTCH: 28, SWP: 60, SWPT: 70, FM: 6 }, { ADEC: 96, VOL: 115 }),
    sound('SNARE', 'perc', { SWP: 30, SWPT: 50, FM: 20, NOIS: 110, NDEC: 74, COLR: 92, CLIK: 80 }, { ADEC: 78, VOL: 100 }),
    sound('CLAP', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 72, COLR: 84, CLIK: 70 },
        { FTYP: FT.HP, FREQ: 82, RESO: 20, ADEC: 76, VOL: 127, SREV: 30 }),
    sound('HAT CLOSED', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 58, COLR: 124 },
        { FTYP: FT.HP, FREQ: 104, ADEC: 60, VOL: 90, PAN: 80 }),
    sound('HAT OPEN', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 80, COLR: 124 },
        { FTYP: FT.HP, FREQ: 102, ADEC: 84, VOL: 84, PAN: 80 }),
    sound('TOM', 'perc', { PTCH: 45, SWP: 40, SWPT: 74, FM: 15, FMD: 60 }, { ADEC: 86, VOL: 105 }),
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
    /* Dub techno: dark chords and drums soaked in delay and reverb */
    sound('DUB KICK', 'perc', { PTCH: 29, SWP: 55, SWPT: 58, FM: 4, FMD: 40 }, { ADEC: 92, VOL: 122, DRIV: 20, SREV: 8 }),
    sound('DUB KICK SOFT', 'perc', { PTCH: 31, SWP: 35, SWPT: 50, FM: 0 }, { ADEC: 88, FREQ: 90, VOL: 124 }),
    sound('DUB RIM', 'perc', { PTCH: 64, SWP: 20, SWPT: 40, RAT: R[4], FM: 90, FMD: 40, CLIK: 100 },
        { ADEC: 58, VOL: 80, PAN: 48, SDLY: 105, SREV: 50 }),
    sound('DUB HAT', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 56, COLR: 124 },
        { FTYP: FT.HP, FREQ: 102, ADEC: 60, VOL: 100, PAN: 78, SDLY: 55, SREV: 40 }),
    sound('DUB SHAKER', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 60, COLR: 110 },
        { FTYP: FT.HP, FREQ: 94, AATK: 6, ADEC: 64, VOL: 100, PAN: 84, SREV: 35 }),
    sound('DUB CLAP', 'perc', { SWP: 0, FM: 0, NOIS: 127, NDEC: 72, COLR: 84, CLIK: 70 },
        { FTYP: FT.HP, FREQ: 80, RESO: 20, ADEC: 76, VOL: 105, SREV: 110, SDLY: 40 }),
    sound('DUB CONGA', 'perc', { PTCH: 55, SWP: 20, SWPT: 50, FM: 10, FMD: 50 }, { ADEC: 74, VOL: 100, PAN: 42, SDLY: 85, SREV: 40 }),
    sound('DUB CHORD', 'stack', { WAVE: 0, VOIC: 3, DTUN: 15 },
        { FTYP: FT.LP24, FREQ: 74, RESO: 35, FENV: 60, FDEC: 55, ADEC: 78, ASUS: 0, AREL: 62,
          SDLY: 115, SREV: 75, SCHO: 50, VOL: 120, L1DEST: DEST(P.FREQ), L1RATE: RATE(0), L1DPTH: 30 }),
    sound('DUB STAB FM', 'poly', { ALGO: 0, RAT2: R[1], LVL2: 55, DEC2: 45, FDBK: 20 },
        { FTYP: FT.LP24, FREQ: 62, RESO: 30, ADEC: 66, ASUS: 0, AREL: 55, SDLY: 110, SREV: 70, SCHO: 40 }),
    sound('DUB PAD', 'stack', { WAVE: 2, VOIC: 5, DTUN: 40 },
        { FTYP: FT.LP12, FREQ: 60, AATK: 90, ASUS: 110, AREL: 100, SREV: 110, SCHO: 80, VOL: 84,
          L1DEST: DEST(P.FREQ), L1RATE: RATE(0), L1DPTH: 40 }),
    sound('DUB SUB', 'poly', { ALGO: 7, LVL2: 0, LVL3: 0, LVL4: 0 }, { FREQ: 110, ASUS: 100, AREL: 25, VOL: 118, DRIV: 15 }),
    sound('DUB BELL', 'poly', { ALGO: 0, RAT2: R[3.5], LVL2: 45, DEC2: 60, LVL3: 0, LVL4: 0 },
        { ADEC: 70, ASUS: 0, AREL: 70, SDLY: 120, SREV: 80, VOL: 84 }),
    sound('DUB FOG', 'stack', { WAVE: 0, VOIC: 7, DTUN: 90, NOIS: 60 },
        { FTYP: FT.BP, FREQ: 70, RESO: 50, AATK: 100, ASUS: 127, AREL: 110, SREV: 120, SDLY: 60, VOL: 76,
          L1DEST: DEST(P.FREQ), L1RATE: RATE(1), L1DPTH: 60 }),
];

/* kits: sounds for tracks 1-8, effect settings, tempo, key and an optional starting groove.
 * steps: [track, step, notes, vel, len index] */
const CM7 = [48, 51, 55, 58];
const DUB_FX = { 0: 7, 1: 100, 2: 110, 3: 34, 4: 115, 5: 105, 6: 20, 7: 95, 8: 60, 9: 20, 10: 110, 11: 92, 12: 30, 13: 20, 14: 10, 15: 100 };
const KITS = [
    { name: 'DUB TECHNO', tempo: 118, key: [0, 1], fx: DUB_FX,
      tracks: ['DUB KICK', 'DUB RIM', 'DUB HAT', 'DUB SHAKER', 'DUB SUB', 'DUB CHORD', 'DUB PAD', 'DUB CLAP'],
      steps: [
          ...[0, 4, 8, 12].map(s => [0, s, [60], 112, 4]),
          ...[7, 13].map(s => [1, s, [60], 82, 4]),
          ...[2, 6, 10, 14].map(s => [2, s, [60], 92, 4]),
          ...[1, 3, 5, 7, 9, 11, 13, 15].map(s => [3, s, [60], 60, 4]),
          [4, 0, [36], 110, 6], [4, 10, [36], 100, 6],
          [5, 3, CM7, 100, 2], [5, 10, CM7, 92, 2],
          [6, 0, CM7.map(n => n + 12), 70, 12],
          ...[4, 12].map(s => [7, s, [60], 64, 4]),
      ] },
    { name: 'DUB DRUMS', tempo: 120, key: [0, 1], fx: DUB_FX,
      tracks: ['DUB KICK SOFT', 'DUB CLAP', 'DUB HAT', 'DUB SHAKER', 'DUB CONGA', 'DUB RIM'],
      steps: [
          ...[0, 4, 8, 12].map(s => [0, s, [60], 110, 4]),
          ...[4, 12].map(s => [1, s, [60], 80, 4]),
          ...[2, 6, 10, 14].map(s => [2, s, [60], 90, 4]),
          ...[0, 2, 3, 5, 6, 8, 10, 11, 13, 14].map(s => [3, s, [60], 56, 4]),
          [4, 3, [60], 90, 4], [4, 9, [65], 80, 4], [4, 14, [58], 85, 4],
          [5, 7, [60], 80, 4], [5, 15, [60], 70, 4],
      ] },
    { name: 'CLASSIC', tempo: 120, key: [0, 1], fx: {},
      tracks: ['KICK DEEP', 'SNARE', 'HAT CLOSED', 'CLAP', 'FM BASS', 'E-PIANO', 'SUPERSAW', 'VOX PAD'], steps: [] },
];

const names = new Set();
for (const s of SOUNDS) { if (names.has(s.name)) throw new Error(`duplicate ${s.name}`); names.add(s.name); }
fs.writeFileSync(path.join(root, 'module/sounds.json'),
    JSON.stringify({ app: 'opgrid-sounds', format: 1, sounds: SOUNDS, kits: KITS }, null, 0) + '\n');
fs.mkdirSync(path.join(root, 'build'), { recursive: true });
fs.writeFileSync(path.join(root, 'build/sounds.txt'), SOUNDS.map(s => `${s.name}|${s.params.join(',')}`).join('\n') + '\n');
for (const k of KITS) for (const n of k.tracks) if (!names.has(n)) throw new Error(`kit ${k.name}: no sound ${n}`);
console.log(`${SOUNDS.length} starter sounds, ${KITS.length} kits -> module/sounds.json`);
