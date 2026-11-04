/*
 * OpGrid — full-screen UI (milestone 3: sequencer).
 *
 * Controls
 *   Steps             toggle steps on the current track; hold a step to edit it
 *   Hold step + knob  lock that parameter on the step (sound pages)
 *                     or edit the step's note/vel/len/... (STEP page)
 *   Hold step + pad   set the step's note; more pads add chord notes (up to 4)
 *   Delete + step     clear the step and its locks
 *   Shift + step 1-4  show steps 1-16 / 17-32 / 33-48 / 49-64
 *   Pads              play the selected track (rows a fourth apart)
 *   Shift + pad       select track (bottom 16 pads)
 *   Mute + pad        mute / unmute track (bottom 16 pads)
 *   Shift + Mute      fill mode while held
 *   Play              start / stop
 *   Knobs 1-8         edit the 8 parameters on the current page
 *   Jog / Left/Right  change page
 *   Up / Down         octave
 *   Back              leave (music keeps playing)
 */
import { setLED, setButtonLED, decodeDelta, decodeAcceleratedDelta, invalidateLedCache }
    from '/data/UserData/schwung/shared/input_filter.mjs';

/* ---------- hardware ids ---------- */
const PAD0 = 68, PADS = 32, STEP0 = 16, STEPS = 16;
const KNOB0 = 71, JOG = 14;
const CC_SHIFT = 49, CC_DOWN = 54, CC_UP = 55, CC_LEFT = 62, CC_RIGHT = 63;
const CC_PLAY = 85, CC_MUTE = 88, CC_DELETE = 119;
const WHITE = 120, GREY = 124, DARK = 119, OFF = 0, LED_BRIGHT = 127, LED_DIM = 16;

/* ---------- tracks ---------- */
const TRACKS = 16;
const TRACK_HUES = [1, 2, 3, 5, 8, 9, 11, 12, 14, 16, 17, 18, 19, 21, 23, 25];
const trackBright = t => TRACK_HUES[t];
const trackDim = t => 63 + 2 * TRACK_HUES[t];

/* ---------- sound parameters (must match og_engine.h) ---------- */
const RATIOS = ['.25', '.5', '.75', '1', '1.5', '2', '2.5', '3', '3.5', '4', '5',
    '6', '7', '8', '9', '10', '11', '12', '13', '14', '15', '16'];
const FTYPES = ['LP12', 'LP24', 'HP', 'BP', 'NTCH'];
const ALGO_NAMES = ['4>3>2>1', '3+4>2>1', '2+3+4>1', '4>3>1+2', '2>1 4>3', '4>123', '4>3 +12', '1+2+3+4'];

const num = v => `${v}`;
const signed = c => v => { const d = v - c; return d > 0 ? `+${d}` : `${d}`; };
const ratio = v => RATIOS[v] || '?';
const hold = v => v >= 127 ? 'HOLD' : `${v}`;
const pan = v => v === 64 ? 'C' : v < 64 ? `L${64 - v}` : `R${v - 64}`;

/* LFO destinations: OFF, PITCH, every sound param 0..31, then the 3 sends */
const DEST_NAMES = ['OFF', 'PTCH',
    'ALGO', 'RAT1', 'RAT2', 'RAT3', 'RAT4', 'LVL2', 'LVL3', 'LVL4',
    'FDBK', 'DEC2', 'DEC3', 'DEC4', 'DTUN', 'VELM', 'TUNE', 'GLID',
    'FREQ', 'RESO', 'FTYP', 'FENV', 'FATK', 'FDEC', 'FSUS', 'FREL',
    'AATK', 'ADEC', 'ASUS', 'AREL', 'DRIV', 'PAN', 'VOL', 'VELA',
    'SDLY', 'SREV', 'SCHO'];
const WAVES = ['SIN', 'TRI', 'SQR', 'SAW', 'RAMP', 'RND'];
const SYNC_RATES = ['8BAR', '6BAR', '4BAR', '3BAR', '2BAR', '6/4', '1BAR', '3/4', '1/2', '3/8',
    '1/4', '3/16', '1/8', '3/32', '1/16', '1/32', '1/64'];
const lfoHz = v => 0.05 * Math.pow(2, v / 127 * 10);
const hz = f => f < 1 ? f.toFixed(2) : f < 10 ? f.toFixed(1) : `${Math.round(f)}Hz`;
/* rate shows a note length when synced, Hz when free (formatter gets the track's values) */
const lfoRate = i => (v, vals) => vals && vals[32 + 8 * (i - 1) + 2]
    ? SYNC_RATES[Math.floor(v * SYNC_RATES.length / 128)] : hz(lfoHz(v));
function lfoParams(i) {
    return [
        ['DEST', `LFO ${i} destination`, DEST_NAMES.length - 1, 0, v => DEST_NAMES[v]],
        ['WAVE', `LFO ${i} wave`, WAVES.length - 1, 0, v => WAVES[v]],
        ['SYNC', `LFO ${i} sync`, 1, 1, v => v ? 'SYNC' : 'FREE'],
        ['RATE', `LFO ${i} rate`, 127, 64, lfoRate(i)],
        ['DPTH', `LFO ${i} depth`, 127, 64, v => { const d = v - 64; return d > 0 ? `+${d}` : `${d}`; }],
        ['TRIG', `LFO ${i} trigger`, 1, 0, v => v ? 'TRIG' : 'FREE'],
        ['PHAS', `LFO ${i} phase`, 127, 0, v => `${Math.round(v * 360 / 128)}`],
        ['FADE', `LFO ${i} fade`, 127, 0, v => v ? `${v}` : 'OFF'],
    ];
}

/* [short name, long name, max, default, formatter] per param id */
const PARAMS = [
    ['ALGO', 'Algorithm', 7, 0, v => `${v + 1}`],
    ['RAT1', 'Op 1 ratio', 21, 3, ratio], ['RAT2', 'Op 2 ratio', 21, 3, ratio],
    ['RAT3', 'Op 3 ratio', 21, 5, ratio], ['RAT4', 'Op 4 ratio', 21, 3, ratio],
    ['LVL2', 'Op 2 level', 127, 60, num], ['LVL3', 'Op 3 level', 127, 30, num],
    ['LVL4', 'Op 4 level', 127, 0, num],

    ['FDBK', 'Feedback', 127, 0, num], ['DEC2', 'Op 2 decay', 127, 70, hold],
    ['DEC3', 'Op 3 decay', 127, 60, hold], ['DEC4', 'Op 4 decay', 127, 60, hold],
    ['DTUN', 'Detune', 127, 0, num], ['VELM', 'Vel > mod', 127, 64, num],
    ['TUNE', 'Tune', 48, 24, signed(24)], ['GLID', 'Glide', 127, 0, num],

    ['FREQ', 'Cutoff', 127, 110, num], ['RESO', 'Resonance', 127, 20, num],
    ['TYPE', 'Filter type', 4, 0, v => FTYPES[v]], ['ENV', 'Env depth', 127, 64, signed(64)],
    ['ATK', 'Filter attack', 127, 0, num], ['DEC', 'Filter decay', 127, 60, num],
    ['SUS', 'Filter sustain', 127, 127, num], ['REL', 'Filter release', 127, 50, num],

    ['ATK', 'Amp attack', 127, 0, num], ['DEC', 'Amp decay', 127, 70, num],
    ['SUS', 'Amp sustain', 127, 90, num], ['REL', 'Amp release', 127, 50, num],
    ['DRIV', 'Drive', 127, 0, num], ['PAN', 'Pan', 127, 64, pan],
    ['VOL', 'Volume', 127, 100, num], ['VEL', 'Vel > amp', 127, 64, num],
    ...lfoParams(1), ...lfoParams(2), ...lfoParams(3),
    ['DLY', 'Delay send', 127, 0, num], ['REV', 'Reverb send', 127, 0, num],
    ['CHO', 'Chorus send', 127, 0, num], ['MCH', 'MIDI channel', 16, 0, v => v ? `CH${v}` : 'OFF'],
    ['MODE', 'MIDI mode', 2, 0, v => ['INT', 'EXT', 'BOTH'][v]],
    ['', '', 0, 0, () => ''], ['', '', 0, 0, () => ''], ['', '', 0, 0, () => ''],
];
const NPARAMS = PARAMS.length;
const SOUND_PAGES = NPARAMS / 8;            /* FM, FM 2, FILTER, AMP, LFO 1-3, SEND */
const PAGES = ['FM', 'FM 2', 'FILTER', 'AMP', 'LFO 1', 'LFO 2', 'LFO 3', 'SEND', 'STEP', 'CLIP', 'FX 1', 'FX 2'];
const PAGE_STEP = 8, PAGE_CLIP = 9, PAGE_FX1 = 10, PAGE_FX2 = 11;
/* header titles for the global FX pages (they are not per track) */
const FX_TITLES = ['DELAY+REVERB', 'CHORUS+MASTR'];
const P_VOL = 30;

/* global effects (must match og_fx.c): [name, max, default, formatter] */
const DTIMES = ['1/32', '1/16T', '1/16', '1/8T', '1/16D', '1/8', '1/4T', '1/8D', '1/4', '1/2T', '1/4D', '1/2', '1BAR'];
const FX = [
    ['TIME', 12, 7, v => DTIMES[v]], ['FDBK', 127, 50, num], ['PING', 127, 64, num],
    ['TONE', 127, 64, v => { const d = v - 64; return d === 0 ? 'FLAT' : d < 0 ? `LP${-d}` : `HP${d}`; }],
    ['SIZE', 127, 80, num], ['DCAY', 127, 70, num], ['PRE', 127, 10, v => `${Math.round(v / 127 * 200)}ms`],
    ['DAMP', 127, 60, num],
    ['DPTH', 127, 60, num], ['RATE', 127, 30, num], ['WIDE', 127, 100, num],
    ['THR', 127, 127, v => v >= 127 ? 'OFF' : `${Math.round(-40 + v / 127 * 40)}dB`],
    ['RAT', 127, 32, v => `${Math.round(1 + v / 127 * 19)}:1`],
    ['GAIN', 127, 0, v => `+${Math.round(v / 127 * 18)}dB`], ['DRIV', 127, 0, num], ['VOL', 127, 100, num],
];
const fxValues = FX.map(f => f[2]);

/* ---------- step fields (must match OG_F_* / og_seq.c) ---------- */
const F = { FLAGS: 0, NOTE0: 1, NOTE1: 2, NOTE2: 3, NOTE3: 4, VEL: 5, LEN: 6, RAT: 7, COND: 8, MICRO: 9 };
const FL_NOTE = 1, FL_LOCK = 2, FL_SLIDE = 4;
const LENS = ['1/8', '1/4', '1/2', '3/4', '1', '1.5', '2', '3', '4', '6', '8', '12', '16'];
const SPEEDS = ['1/8x', '1/4x', '1/2x', '3/4x', '1x', '1.5x', '2x'];
const CONDS = (() => {
    const c = ['--', 'FILL', '!FIL', 'PRE', '!PRE', '1ST', '!1ST'];
    for (let b = 2; b <= 8; b++) for (let a = 1; a <= b; a++) c.push(`${a}:${b}`);
    for (let p = 5; p <= 95; p += 5) c.push(`${p}%`);
    return c;
})();
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = n => n <= 0 ? '--' : `${NOTE_NAMES[n % 12]}${Math.floor(n / 12) - 2}`;

/* STEP page: [name, max, min, formatter] — knob 0 is TYPE, 7 is SLIDE */
const STEP_KNOBS = [
    ['TYPE', 1, 0, v => v ? 'LOCK' : 'NOTE'],
    ['NOTE', 127, 1, noteName],
    ['VEL', 127, 1, num],
    ['LEN', LENS.length - 1, 0, v => LENS[v]],
    ['MICR', 23, -23, v => v > 0 ? `+${v}` : `${v}`],
    ['RTCH', 8, 1, v => `${v}x`],
    ['COND', CONDS.length - 1, 0, v => CONDS[v]],
    ['SLID', 1, 0, v => v ? 'ON' : 'OFF'],
];
/* CLIP page */
const CLIP_KNOBS = [
    ['LEN', 64, 1, num],
    ['SPD', SPEEDS.length - 1, 0, v => SPEEDS[v]],
    ['SWNG', 80, 50, v => `${v}%`],
    ['BPM', 300, 30, num],
    ['SYNC', 1, 0, v => v ? 'MOVE' : 'OWN'],
];

/* ---------- state ---------- */
let track = 0, page = 0, octave = 1, stepPage = 0;
let shift = false, muteHeld = false, muteUsed = false, deleteHeld = false, fillOn = false;
let mixer = false, mixBank = 0;
const solo = new Array(16).fill(0);
let playing = false, tempo = 120, sync = 1;
let touchedKnob = -1, touchedUntil = 0, tickCount = 0;
let dirty = true, stepsDirty = true;
const values = Array.from({ length: TRACKS }, () => PARAMS.map(p => p[3]));
const padNote = new Array(PADS).fill(-1);       /* note sounding from each pad */
const stepFlags = Array.from({ length: TRACKS }, () => new Array(64).fill(0));
const clipInfo = Array.from({ length: TRACKS }, () => ({ len: 16, speed: 4, swing: 50, locks: 0, mute: 0 }));
let playheads = new Array(TRACKS).fill(-1);

/* held steps: step index -> { existed, edited, fields[], locks Map(pid->val), chordCount } */
const held = new Map();
const stepDefaults = { note: 60, vel: 100, len: 4 };

const ledCache = new Map();
const LEDS_PER_TICK = 12;

/* ---------- host helpers ---------- */
function setParam(key, val) {
    if (typeof host_module_set_param === 'function') host_module_set_param(key, `${val}`);
}
function getParam(key) {
    return typeof host_module_get_param === 'function' ? host_module_get_param(key) : null;
}
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const ints = s => `${s ?? ''}`.split(',').map(x => parseInt(x, 10));

function pullTrack(t) {
    const s = getParam(`tp:${t}`);
    if (s) {
        const parts = ints(s);
        for (let i = 0; i < NPARAMS && i < parts.length; i++)
            if (Number.isFinite(parts[i])) values[t][i] = clamp(parts[i], 0, PARAMS[i][2]);
    }
    pullClip(t);
    pullSteps(t);
}

function pullClip(t) {
    const c = ints(getParam(`clip:${t}`));
    if (c.length >= 5 && c.every(Number.isFinite))
        Object.assign(clipInfo[t], { len: c[0], speed: c[1], swing: c[2], locks: c[3], mute: c[4] });
}

function pullSteps(t) {
    const s = getParam(`steps:${t}`);
    if (s && s.length >= 64)
        for (let i = 0; i < 64; i++) stepFlags[t][i] = s.charCodeAt(i) - 48;
    stepsDirty = false;
}

function pullFx() {
    const f = ints(getParam('fx'));
    if (f.length === FX.length && f.every(Number.isFinite)) f.forEach((v, i) => { fxValues[i] = clamp(v, 0, FX[i][1]); });
}

function pullMix() {
    const m = getParam('mix');
    if (!m || m.length < 32) return;
    for (let t = 0; t < TRACKS; t++) { clipInfo[t].mute = m.charCodeAt(t) - 48; solo[t] = m.charCodeAt(16 + t) - 48; }
}

function editFx(knob, delta) {
    const id = (page === PAGE_FX2 ? 8 : 0) + knob;
    const [, max] = FX[id];
    touch(knob);
    const next = clamp(fxValues[id] + delta, 0, max);
    if (next === fxValues[id]) return;
    fxValues[id] = next;
    setParam('fx', `${id} ${next}`);
}

function editMixer(knob, delta) {
    const t = mixBank * 8 + knob;
    touch(knob);
    const next = clamp(values[t][P_VOL] + delta, 0, 127);
    if (next === values[t][P_VOL]) return;
    values[t][P_VOL] = next;
    setParam('p', `${t} ${P_VOL} ${next}`);
}

function toggleMute(t) {
    clipInfo[t].mute = clipInfo[t].mute ? 0 : 1;
    setParam('mute', `${t} ${clipInfo[t].mute}`);
    dirty = true;
}

function toggleSolo(t) {
    solo[t] = solo[t] ? 0 : 1;
    setParam('solo', `${t} ${solo[t]}`);
    dirty = true;
}

function pullTransport() {
    const tp = parseFloat(getParam('tempo'));
    if (Number.isFinite(tp)) tempo = Math.round(tp);
    const sy = parseInt(getParam('sync'), 10);
    if (Number.isFinite(sy)) sync = sy;
}

function pullStepDetail(s) {
    const h = held.get(s);
    if (!h) return;
    const f = ints(getParam(`step:${track}:${s}`));
    if (f.length >= 10 && f.every(Number.isFinite)) h.fields = f;
    h.locks = new Map();
    const l = getParam(`locks:${track}:${s}`);
    if (l) for (const kv of `${l}`.split(',')) {
        const [p, v] = kv.split('=').map(x => parseInt(x, 10));
        if (Number.isFinite(p) && Number.isFinite(v)) h.locks.set(p, v);
    }
}

function selectTrack(t) {
    if (t < 0 || t >= TRACKS || t === track) return;
    releaseHeldSteps();
    track = t;
    pullTrack(t);
    dirty = true;
}

/* ---------- editing ---------- */
function touch(knob) {
    touchedKnob = knob;
    touchedUntil = tickCount + 600;
    dirty = true;
}

function firstHeld() {
    for (const [s, h] of held) return [s, h];
    return null;
}

function editSound(knob, delta) {
    const pid = page * 8 + knob;
    if (pid >= NPARAMS) return;
    const max = PARAMS[pid][2];
    touch(knob);
    if (held.size) {
        /* lock the parameter on every held step */
        for (const [s, h] of held) {
            const cur = h.locks.has(pid) ? h.locks.get(pid) : values[track][pid];
            const next = clamp(cur + delta, 0, max);
            h.locks.set(pid, next);
            h.edited = true;
            setParam('lock', `${track} ${s} ${pid} ${next}`);
        }
        stepsDirty = true;
        return;
    }
    const next = clamp(values[track][pid] + delta, 0, max);
    if (next === values[track][pid]) return;
    values[track][pid] = next;
    setParam('p', `${track} ${pid} ${next}`);
}

function stepKnobValue(h, knob) {
    const f = h.fields;
    switch (knob) {
    case 0: return (f[F.FLAGS] & FL_LOCK) && !(f[F.FLAGS] & FL_NOTE) ? 1 : 0;
    case 1: return f[F.NOTE0];
    case 2: return f[F.VEL];
    case 3: return f[F.LEN];
    case 4: return f[F.MICRO];
    case 5: return f[F.RAT];
    case 6: return f[F.COND];
    default: return f[F.FLAGS] & FL_SLIDE ? 1 : 0;
    }
}

function editStep(knob, delta) {
    if (knob >= STEP_KNOBS.length) return;
    const [, max, min] = STEP_KNOBS[knob];
    touch(knob);
    if (!held.size) {
        /* no step held: edit the defaults used for new steps */
        if (knob === 1) stepDefaults.note = clamp(stepDefaults.note + delta, 1, 127);
        if (knob === 2) stepDefaults.vel = clamp(stepDefaults.vel + delta, 1, 127);
        if (knob === 3) stepDefaults.len = clamp(stepDefaults.len + delta, 0, LENS.length - 1);
        return;
    }
    for (const [s, h] of held) {
        if (!h.fields) continue;
        const f = h.fields;
        const next = clamp(stepKnobValue(h, knob) + delta, min, max);
        h.edited = true;
        let field, val;
        switch (knob) {
        case 0: field = F.FLAGS; val = (f[F.FLAGS] & FL_SLIDE) | (next ? FL_LOCK : FL_NOTE); break;
        case 1: field = F.NOTE0; val = next; break;
        case 2: field = F.VEL; val = next; break;
        case 3: field = F.LEN; val = next; break;
        case 4: field = F.MICRO; val = next; break;
        case 5: field = F.RAT; val = next; break;
        case 6: field = F.COND; val = next; break;
        default: field = F.FLAGS; val = (f[F.FLAGS] & ~FL_SLIDE) | (next ? FL_SLIDE : 0); break;
        }
        f[field] = val;
        setParam('sf', `${track} ${s} ${field} ${val}`);
    }
    stepsDirty = true;
}

function clipKnobValue(knob) {
    const c = clipInfo[track];
    return [c.len, c.speed, c.swing, tempo, sync][knob];
}

function editClip(knob, delta) {
    if (knob >= CLIP_KNOBS.length) return;
    const [, max, min] = CLIP_KNOBS[knob];
    touch(knob);
    const next = clamp(clipKnobValue(knob) + delta, min, max);
    const c = clipInfo[track];
    switch (knob) {
    case 0: c.len = next; setParam('clen', `${track} ${next}`); if (stepPage * 16 >= next) stepPage = 0; break;
    case 1: c.speed = next; setParam('cspd', `${track} ${next}`); break;
    case 2: c.swing = next; setParam('cswg', `${track} ${next}`); break;
    case 3: tempo = next; sync = 0; setParam('tempo', next); break;
    default: sync = next; setParam('sync', next); break;
    }
}

/* ---------- steps ---------- */
function onStep(i, on) {
    const s = stepPage * 16 + i;
    if (on && shift) {
        if (i < 4 && i * 16 < clipInfo[track].len) { stepPage = i; dirty = true; }
        return;
    }
    if (s >= clipInfo[track].len) return;
    if (on && deleteHeld) {
        setParam('sclr', `${track} ${s}`);
        stepFlags[track][s] = 0;
        dirty = true;
        return;
    }
    if (on) {
        const existed = (stepFlags[track][s] & (FL_NOTE | FL_LOCK)) !== 0;
        if (!existed) {
            setParam('sclr', `${track} ${s}`);
            setParam('sf', `${track} ${s} ${F.FLAGS} ${FL_NOTE}`);
            setParam('sf', `${track} ${s} ${F.NOTE0} ${stepDefaults.note}`);
            setParam('sf', `${track} ${s} ${F.VEL} ${stepDefaults.vel}`);
            setParam('sf', `${track} ${s} ${F.LEN} ${stepDefaults.len}`);
            stepFlags[track][s] = FL_NOTE;
        }
        held.set(s, { existed, edited: false, fields: null, locks: new Map(), chord: 0 });
        pullStepDetail(s);
    } else {
        const h = held.get(s);
        if (!h) return;
        held.delete(s);
        if (h.existed && !h.edited) {
            setParam('sclr', `${track} ${s}`);
            stepFlags[track][s] = 0;
        }
        stepsDirty = true;
    }
    dirty = true;
}

function releaseHeldSteps() {
    held.clear();
    stepsDirty = true;
}

/* ---------- pads ---------- */
function padToNote(i) {
    const row = Math.floor(i / 8), col = i % 8;
    return 36 + octave * 12 + col + row * 5;
}

function onPad(i, vel, on) {
    if (mixer) {
        /* bottom 16 pads: mute (Shift = solo); top 16: select track */
        if (!on) return;
        if (i < 16) { if (shift) toggleSolo(i); else toggleMute(i); }
        else selectTrack(i - 16);
        return;
    }
    if (on && shift) { if (i < 16) selectTrack(i); return; }
    if (on && muteHeld) {
        muteUsed = true;
        if (i < 16) toggleMute(i);
        return;
    }
    if (on) {
        const n = padToNote(i);
        if (n > 127) return;
        if (held.size) {
            /* set the held steps' notes: first pad replaces, more pads add a chord */
            for (const [s, h] of held) {
                const slot = Math.min(h.chord, 3);
                if (slot === 0) for (let k = 1; k < 4; k++) setParam('sf', `${track} ${s} ${F.NOTE0 + k} 0`);
                setParam('sf', `${track} ${s} ${F.NOTE0 + slot} ${n}`);
                if (h.fields) h.fields[F.NOTE0 + slot] = n;
                h.chord++;
                h.edited = true;
            }
            dirty = true;
        }
        if (padNote[i] >= 0) setParam('noff', `${track} ${padNote[i]}`);
        padNote[i] = n;
        setParam('non', `${track} ${n} ${Math.max(1, vel)}`);
    } else if (padNote[i] >= 0) {
        setParam('noff', `${track} ${padNote[i]}`);
        padNote[i] = -1;
    }
}

/* ---------- MIDI in ---------- */
globalThis.onMidiMessageInternal = function (data) {
    if (!data || data.length < 3) return;
    const status = data[0] & 0xF0, d1 = data[1], d2 = data[2];

    if (status === 0x90 || status === 0x80) {
        const on = status === 0x90 && d2 > 0;
        if (d1 < 10) return;                                   /* knob touch */
        if (d1 >= PAD0 && d1 < PAD0 + PADS) { onPad(d1 - PAD0, d2, on); return; }
        if (d1 >= STEP0 && d1 < STEP0 + STEPS) onStep(d1 - STEP0, on);
        return;
    }
    if (status !== 0xB0) return;

    if (d1 >= KNOB0 && d1 < KNOB0 + 8) {
        const knob = d1 - KNOB0;
        let small;
        if (mixer) small = false;
        else if (page < SOUND_PAGES) { const pid = page * 8 + knob; small = pid < NPARAMS && PARAMS[pid][2] < 40; }
        else if (page === PAGE_STEP) small = knob !== 1 && knob !== 2;
        else if (page === PAGE_CLIP) small = knob === 1 || knob === 4;
        else small = FX[(page === PAGE_FX2 ? 8 : 0) + knob][1] < 40;
        const raw = small ? decodeDelta(d2) : decodeAcceleratedDelta(d2, knob);
        const delta = shift && !small ? Math.sign(raw) : raw;
        if (!delta) return;
        if (mixer) editMixer(knob, delta);
        else if (page < SOUND_PAGES) editSound(knob, delta);
        else if (page === PAGE_STEP) editStep(knob, delta);
        else if (page === PAGE_CLIP) editClip(knob, delta);
        else editFx(knob, delta);
        return;
    }
    const press = d2 > 0;
    switch (d1) {
    case CC_SHIFT: shift = press; updateFill(); dirty = true; break;
    case CC_MUTE:
        if (press) { muteHeld = true; muteUsed = shift; }
        else {
            muteHeld = false;
            /* a tap on Mute (no pads, no Shift) toggles mixer mode */
            if (!muteUsed && !shift) { mixer = !mixer; if (mixer) pullMix(); }
        }
        updateFill(); dirty = true; break;
    case CC_DELETE: deleteHeld = press; break;
    case CC_PLAY:
        if (press) { playing = !playing; setParam('play', playing ? 1 : 0); dirty = true; }
        break;
    case JOG: { const dl = decodeDelta(d2); if (dl) changePage(dl); break; }
    case CC_LEFT: if (press) { if (mixer) { mixBank = 0; dirty = true; } else changePage(-1); } break;
    case CC_RIGHT: if (press) { if (mixer) { mixBank = 1; dirty = true; } else changePage(1); } break;
    case CC_UP: if (press) { octave = Math.min(6, octave + 1); dirty = true; } break;
    case CC_DOWN: if (press) { octave = Math.max(-2, octave - 1); dirty = true; } break;
    default: break;
    }
};

globalThis.onMidiMessageExternal = function () { /* DSP handles external notes */ };

function updateFill() {
    const f = shift && muteHeld;
    if (f) muteUsed = true;
    if (f !== fillOn) { fillOn = f; setParam('fill', f ? 1 : 0); }
}

function changePage(d) {
    page = (page + d + PAGES.length) % PAGES.length;
    touchedKnob = -1;
    if (page === PAGE_CLIP) { pullClip(track); pullTransport(); }
    if (page === PAGE_FX1 || page === PAGE_FX2) pullFx();
    dirty = true;
}

/* ---------- LEDs ---------- */
function stepColor(i) {
    const s = stepPage * 16 + i;
    const len = clipInfo[track].len;
    if (shift) {
        if (i >= 4) return OFF;
        if (i * 16 >= len) return OFF;
        return i === stepPage ? WHITE : GREY;
    }
    if (s >= len) return OFF;
    if (held.has(s)) return WHITE;
    if (playing && playheads[track] === s) return WHITE;
    const f = stepFlags[track][s];
    if (f & FL_NOTE) return trackBright(track);
    if (f & FL_LOCK) return GREY;
    return i % 4 === 0 ? DARK : OFF;
}

function padColor(i) {
    if (mixer) {
        if (i < 16) return solo[i] ? WHITE : clipInfo[i].mute ? trackDim(i) : trackBright(i);
        const t = i - 16;
        return t === track ? WHITE : trackDim(t);
    }
    if (shift) return i < 16 ? (i === track ? trackBright(i) : trackDim(i)) : OFF;
    if (muteHeld) return i < 16 ? (clipInfo[i].mute ? trackDim(i) : trackBright(i)) : OFF;
    if (padNote[i] >= 0) return WHITE;
    const n = padToNote(i);
    return n > 127 ? OFF : (n % 12 === 0 ? trackBright(track) : trackDim(track));
}

function desiredLeds() {
    const out = [];
    for (let i = 0; i < STEPS; i++) out.push(['n', STEP0 + i, stepColor(i)]);
    for (let i = 0; i < PADS; i++) out.push(['n', PAD0 + i, padColor(i)]);
    out.push(['c', CC_PLAY, playing ? LED_BRIGHT : LED_DIM]);
    out.push(['c', CC_LEFT, LED_DIM], ['c', CC_RIGHT, LED_DIM]);
    out.push(['c', CC_UP, octave < 6 ? LED_DIM : OFF], ['c', CC_DOWN, octave > -2 ? LED_DIM : OFF]);
    out.push(['c', CC_SHIFT, shift ? LED_BRIGHT : LED_DIM]);
    out.push(['c', CC_MUTE, muteHeld || mixer ? LED_BRIGHT : LED_DIM]);
    out.push(['c', CC_DELETE, deleteHeld ? LED_BRIGHT : LED_DIM]);
    return out;
}

function flushLeds() {
    let sent = 0;
    for (const [kind, id, color] of desiredLeds()) {
        const key = `${kind}${id}`;
        if (ledCache.get(key) === color) continue;
        if (kind === 'n') setLED(id, color); else setButtonLED(id, color);
        ledCache.set(key, color);
        if (++sent >= LEDS_PER_TICK) break;
    }
}

/* ---------- screen (128x64, 1-bit) ---------- */
function drawCell(k, name, text, frac, hot, marked) {
    const x = (k % 4) * 32, y = k < 4 ? 12 : 38;
    if (hot) fill_rect(x, y, 32, 25, 1);
    const ink = hot ? 0 : 1;
    print(x + 2, y + 1, name, ink);
    if (marked) fill_rect(x + 27, y + 2, 3, 3, ink);
    print(x + 2, y + 11, `${text ?? '?'}`, ink);
    if (frac !== null) {
        draw_rect(x + 2, y + 21, 28, 3, ink);
        const w = Math.round(clamp(frac, 0, 1) * 28);
        if (w > 0) fill_rect(x + 2, y + 21, w, 3, ink);
    }
}

function drawHeader() {
    fill_rect(0, 0, 128, 10, 1);
    const hs = firstHeld();
    let left = `T${String(track + 1).padStart(2, '0')}`;
    left += hs ? ` STEP ${hs[0] + 1}` : (clipInfo[track].mute ? ' MUTED' : ' FM POLY');
    if (!mixer && (page === PAGE_FX1 || page === PAGE_FX2)) left = FX_TITLES[page - PAGE_FX1];
    if (mixer) left = `TRACKS ${mixBank * 8 + 1}-${mixBank * 8 + 8}`;
    print(2, 1, left, 0);
    const right = `${playing ? '>' : '.'} ${mixer ? 'MIX' : (page === PAGE_FX1 || page === PAGE_FX2) ? '' : PAGES[page]}`;
    print(126 - text_width(right), 1, right, 0);
}

function drawScreen() {
    clear_screen();
    drawHeader();
    const hs = firstHeld();
    const hot = k => k === touchedKnob && tickCount < touchedUntil;

    if (mixer) { drawMixer(hot); return; }
    if (page < SOUND_PAGES) {
        for (let k = 0; k < 8; k++) {
            const pid = page * 8 + k;
            const [name, , max, , fmt] = PARAMS[pid];
            if (!name) continue;
            const locked = hs && hs[1].locks.has(pid);
            const v = clamp(locked ? hs[1].locks.get(pid) : values[track][pid] | 0, 0, max);
            drawCell(k, name, fmt(v, values[track]), v / max, hot(k), locked);
        }
        if (hot(0) && page === 0) {
            fill_rect(0, 54, 128, 10, 0);
            print(2, 55, `ALGO ${ALGO_NAMES[values[track][0]]}`, 1);
        }
    } else if (page === PAGE_STEP) {
        if (!hs) {
            drawCell(1, 'NOTE', noteName(stepDefaults.note), null, hot(1), false);
            drawCell(2, 'VEL', `${stepDefaults.vel}`, stepDefaults.vel / 127, hot(2), false);
            drawCell(3, 'LEN', LENS[stepDefaults.len], null, hot(3), false);
            print(2, 42, 'New-step defaults.', 1);
            print(2, 52, 'Hold a step to edit', 1);
        } else if (hs[1].fields) {
            STEP_KNOBS.forEach(([name, max, min, fmt], k) => {
                const v = stepKnobValue(hs[1], k);
                drawCell(k, name, fmt(v), max - min > 1 ? (v - min) / (max - min) : null, hot(k), false);
            });
            const f = hs[1].fields, chord = [f[F.NOTE1], f[F.NOTE2], f[F.NOTE3]].filter(n => n > 0);
            if (chord.length && !hot(1)) {
                fill_rect(0, 54, 128, 10, 0);
                print(2, 55, `+ ${chord.map(noteName).join(' ')}`, 1);
            }
        }
    } else if (page === PAGE_FX1 || page === PAGE_FX2) {
        const base = page === PAGE_FX2 ? 8 : 0;
        for (let k = 0; k < 8; k++) {
            const [name, max, , fmt] = FX[base + k];
            const v = fxValues[base + k];
            drawCell(k, name, fmt(v), v / max, hot(k), false);
        }
    } else {
        CLIP_KNOBS.forEach(([name, max, min, fmt], k) => {
            const v = clipKnobValue(k);
            drawCell(k, name, fmt(v), max - min > 1 ? (v - min) / (max - min) : null, hot(k), false);
        });
        print(2 + 32, 52 - 2, `LOCKS ${clipInfo[track].locks}/64`, 1);
    }
}

function drawMixer(hot) {
    for (let k = 0; k < 8; k++) {
        const t = mixBank * 8 + k;
        const x = k * 16, v = values[t][P_VOL];
        if (t === track) fill_rect(x, 11, 16, 9, 1);
        print(x + 2, 12, `${t + 1}`, t === track ? 0 : 1);
        const h = Math.round(v / 127 * 30);
        draw_rect(x + 4, 22, 8, 32, 1);
        if (h > 0) fill_rect(x + 5, 53 - h, 6, h, 1);
        const flag = solo[t] ? 'S' : clipInfo[t].mute ? 'M' : '';
        if (flag) print(x + 5, 56, flag, 1);
        if (hot(k)) draw_rect(x + 2, 21, 12, 34, 1);
    }
}

/* ---------- lifecycle ---------- */
globalThis.init = function () {
    for (let t = 0; t < TRACKS; t++) pullTrack(t);
    pullTransport();
    pullFx();
    pullMix();
    playing = getParam('play') === '1';
    ledCache.clear();
    dirty = true;
};

globalThis.onResume = function () {
    ledCache.clear();
    if (typeof invalidateLedCache === 'function') invalidateLedCache();
    pullTrack(track);
    pullTransport();
    playing = getParam('play') === '1';
    dirty = true;
};

globalThis.onUnload = function () {
    setParam('panic', 1);
};

globalThis.tick = function () {
    tickCount++;
    if (touchedKnob >= 0 && tickCount === touchedUntil) dirty = true;
    if (tickCount % 8 === 0 && playing) {
        const p = ints(getParam('pos'));
        if (p.length === TRACKS && p[track] !== playheads[track]) dirty = true;
        if (p.length === TRACKS) playheads = p;
    }
    if (stepsDirty && tickCount % 4 === 0) { pullSteps(track); pullClip(track); }
    if (sync && tickCount % 256 === 0) { const old = tempo; pullTransport(); if (old !== tempo) dirty = true; }
    flushLeds();
    if (dirty && tickCount % 4 === 0) {
        drawScreen();
        dirty = false;
    }
};

/* exported for tests */
globalThis.__opgrid = {
    PARAMS, PAGES, CONDS, LENS, SPEEDS, FX, DEST_NAMES, WAVES, SYNC_RATES, fxValues, solo,
    values, stepFlags, clipInfo, held,
    state: () => ({ track, page, octave, shift, playing, stepPage, tempo, sync, mixer, mixBank }),
};
