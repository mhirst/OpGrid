/* End to end: the real UI (src/ui.js) driving the real engine through
 * tools/bridge. Builds a little project, saves it, resets, loads it back
 * and checks the engine state round-trips. */
import { startHost } from '../tools/realhost.mjs';

let failures = 0;
const check = (c, m) => { if (!c) { failures++; console.log('FAIL', m); } };
const h = await startHost();
const { ui } = h;
const step = i => { h.note(16 + i); h.noteOff(16 + i); };
const jogTo = p => { let n = 0; while (ui.state().page !== p && n++ < 30) h.midi(0xB0, 14, ui.state().page < p ? 1 : 127); };
globalThis.init();
h.tick(40);

/* steps and a lock on track 1 */
for (const s of [0, 4, 8, 12]) step(s);
jogTo(2);
h.note(16 + 4); h.knob(0, -20); h.tick(4); h.noteOff(16 + 4);
h.tick(8);
check(h.call('G steps:0').slice(0, 13) === '1000900010001', `real step flags: ${h.call('G steps:0').slice(0, 16)}`);
check(h.call('G locks:0:4') === '16=90', `real lock: ${h.call('G locks:0:4')}`);

/* track 2 becomes FM Perc via the sound library */
h.files.set('/data/UserData/schwung/modules/overtake/opgrid/sounds.json',
    (await import('fs')).readFileSync(new URL('../module/sounds.json', import.meta.url), 'utf8'));
h.midi(0xB0, 49, 127); h.note(69); h.noteOff(69); h.midi(0xB0, 49, 0);       /* select track 2 */
jogTo(0); h.tick(400); h.midi(0xB0, 3, 127); h.midi(0xB0, 3, 127);             /* browse, keep the first drum */
check(h.call('G tp:1').split(',')[61] === '1', 'track 2 is now FM Perc in the engine');
step(2); step(10);

/* play: playheads move, notes sound */
h.cc(85);
let atStep3 = 0, maxVoices = 0;
for (let i = 0; i < 40; i++) {
    h.tick(10);
    const v = +h.call('G voices'), p0 = +h.call('G pos').split(',')[0];
    maxVoices = Math.max(maxVoices, v);
    if (p0 === 2) atStep3 = Math.max(atStep3, v);      /* only track 2 (FM Perc) has a step here */
}
const pos = h.call('G pos').split(',').map(Number);
check(pos[0] >= 0 && pos[1] >= 0, `playheads running: ${pos.slice(0, 2)}`);
check(maxVoices >= 1 && atStep3 >= 1, `notes sound while playing (max ${maxVoices}, track 2 at step 3: ${atStep3})`);
h.cc(85);

/* a song row, tempo and an effect, then save */
h.midi(0xB0, 49, 127); h.cc(58); h.midi(0xB0, 49, 0);
h.tick(4); h.midi(0xB0, 3, 127); h.midi(0xB0, 49, 127); h.cc(58); h.midi(0xB0, 49, 0);
jogTo(9); h.knob(0, 7);                                /* chorus depth */
jogTo(13); h.knob(3, 4);                               /* tempo 124 */
const before = {
    steps0: h.call('G steps:0'), steps1: h.call('G steps:1'), tp1: h.call('G tp:1'),
    locks: h.call('G locks:0:4'), fx: h.call('G fx'), tempo: h.call('G tempo'), slens: h.call('G slens'),
    sdump: h.call('G sdump:0'),
};
h.cc(50); h.midi(0xB0, 14, 1); h.midi(0xB0, 14, 1); h.midi(0xB0, 3, 127);   /* Menu > SAVE */
h.tick(300);
const file = [...h.files.keys()].find(k => k.includes('/projects/'));
check(!!file, 'project saved');

/* wipe the engine, then load the project back through the menu */
h.call('S reset\t1');
check(h.call('G steps:0').startsWith('0000'), 'engine reset');
h.cc(50);
for (let i = 0; i < 5; i++) h.midi(0xB0, 14, 1);
h.midi(0xB0, 3, 127); h.midi(0xB0, 3, 127);            /* LOAD, confirm */
h.tick(400);
const after = {
    steps0: h.call('G steps:0'), steps1: h.call('G steps:1'), tp1: h.call('G tp:1'),
    locks: h.call('G locks:0:4'), fx: h.call('G fx'), tempo: h.call('G tempo'), slens: h.call('G slens'),
    sdump: h.call('G sdump:0'),
};
for (const k of Object.keys(before)) check(before[k] === after[k], `${k} round-trips: ${before[k].slice(0, 40)} vs ${after[k].slice(0, 40)}`);

h.stop();
if (failures) { console.log(`${failures} end-to-end check(s) FAILED`); process.exit(1); }
console.log('end-to-end (real UI + real engine) passed');
