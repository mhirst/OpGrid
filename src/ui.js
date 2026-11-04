/*
 * OpGrid — full-screen UI (milestone 2: keys mode + sound editing).
 *
 * Controls
 *   Pads            play the selected track (rows a fourth apart)
 *   Shift + pad     select track (bottom 16 pads)
 *   Shift + step    select track 1-16
 *   Knobs 1-8       edit the 8 parameters on the current page
 *   Jog / Left/Right  change page
 *   Up / Down       octave
 *   Back            leave (sound keeps playing); Shift+Back exits
 */
import { setLED, setButtonLED, decodeDelta, decodeAcceleratedDelta, invalidateLedCache }
    from '/data/UserData/schwung/shared/input_filter.mjs';

/* ---------- hardware ids ---------- */
const PAD0 = 68, PADS = 32, STEP0 = 16, STEPS = 16;
const KNOB0 = 71, JOG = 14, JOG_CLICK = 3;
const CC_SHIFT = 49, CC_MENU = 50, CC_BACK = 51, CC_DOWN = 54, CC_UP = 55;
const CC_LEFT = 62, CC_RIGHT = 63;
const WHITE = 120, DIM_WHITE = 124, OFF = 0, LED_BRIGHT = 127, LED_DIM = 16;

/* ---------- tracks ---------- */
const TRACKS = 16;
const TRACK_HUES = [1, 2, 3, 5, 8, 9, 11, 12, 14, 16, 17, 18, 19, 21, 23, 25];
const trackBright = t => TRACK_HUES[t];
const trackDim = t => 63 + 2 * TRACK_HUES[t];

/* ---------- parameters (must match og_engine.h) ---------- */
const RATIOS = ['.25', '.5', '.75', '1', '1.5', '2', '2.5', '3', '3.5', '4', '5',
    '6', '7', '8', '9', '10', '11', '12', '13', '14', '15', '16'];
const FTYPES = ['LP12', 'LP24', 'HP', 'BP', 'NTCH'];
const ALGO_NAMES = ['4>3>2>1', '3+4>2>1', '2+3+4>1', '4>3>1+2', '2>1 4>3', '4>123', '4>3 +12', '1+2+3+4'];

const num = v => `${v}`;
const signed = c => v => { const d = v - c; return d > 0 ? `+${d}` : `${d}`; };
const ratio = v => RATIOS[v] || '?';
const hold = v => v >= 127 ? 'HOLD' : `${v}`;
const pan = v => v === 64 ? 'C' : v < 64 ? `L${64 - v}` : `R${v - 64}`;

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
];
const PAGES = ['FM', 'FM 2', 'FILTER', 'AMP'];
const NPARAMS = PARAMS.length;

/* ---------- state ---------- */
let track = 0, page = 0, octave = 1, shift = false;
let touchedKnob = -1, touchedUntil = 0, tickCount = 0;
let dirty = true;
const values = Array.from({ length: TRACKS }, () => PARAMS.map(p => p[3]));
const padNote = new Array(PADS).fill(-1);       /* note held by each pad */
const ledCache = new Map();
const LEDS_PER_TICK = 12;

/* ---------- host helpers ---------- */
function setParam(key, val) {
    if (typeof host_module_set_param === 'function') host_module_set_param(key, `${val}`);
}
function getParam(key) {
    return typeof host_module_get_param === 'function' ? host_module_get_param(key) : null;
}

function pullTrack(t) {
    const s = getParam(`tp:${t}`);
    if (!s) return;
    const parts = `${s}`.split(',').map(x => parseInt(x, 10));
    for (let i = 0; i < NPARAMS && i < parts.length; i++)
        if (Number.isFinite(parts[i])) values[t][i] = Math.max(0, Math.min(PARAMS[i][2], parts[i]));
}

function selectTrack(t) {
    if (t < 0 || t >= TRACKS || t === track) return;
    track = t;
    pullTrack(t);
    dirty = true;
}

function editParam(knob, delta) {
    const pid = page * 8 + knob;
    if (pid >= NPARAMS) return;
    const max = PARAMS[pid][2];
    const cur = values[track][pid];
    const next = Math.max(0, Math.min(max, cur + delta));
    touchedKnob = knob;
    touchedUntil = tickCount + 600;
    if (next === cur) { dirty = true; return; }
    values[track][pid] = next;
    setParam('p', `${track} ${pid} ${next}`);
    dirty = true;
}

/* ---------- pads ---------- */
function padToNote(i) {
    const row = Math.floor(i / 8), col = i % 8;
    return 36 + octave * 12 + col + row * 5;
}

function onPad(i, vel, on) {
    if (on && shift) {
        if (i < 16) selectTrack(i);
        return;
    }
    if (on) {
        const n = padToNote(i);
        if (n > 127) return;
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
        if (d1 < 10) return;                                  /* knob touch */
        if (d1 >= PAD0 && d1 < PAD0 + PADS) { onPad(d1 - PAD0, d2, on); return; }
        if (d1 >= STEP0 && d1 < STEP0 + STEPS && on && shift) selectTrack(d1 - STEP0);
        return;
    }
    if (status !== 0xB0) return;

    if (d1 >= KNOB0 && d1 < KNOB0 + 8) {
        const knob = d1 - KNOB0;
        const pid = page * 8 + knob;
        const small = pid < NPARAMS && PARAMS[pid][2] < 32;
        const delta = small ? decodeDelta(d2) : decodeAcceleratedDelta(d2, knob);
        editParam(knob, shift && !small ? Math.sign(delta) : delta);
        return;
    }
    switch (d1) {
    case CC_SHIFT: shift = d2 > 0; dirty = true; break;
    case JOG: { const dl = decodeDelta(d2); if (dl) changePage(dl); break; }
    case CC_LEFT: if (d2 > 0) changePage(-1); break;
    case CC_RIGHT: if (d2 > 0) changePage(1); break;
    case CC_UP: if (d2 > 0) { octave = Math.min(6, octave + 1); dirty = true; } break;
    case CC_DOWN: if (d2 > 0) { octave = Math.max(-2, octave - 1); dirty = true; } break;
    default: break;
    }
};

globalThis.onMidiMessageExternal = function () { /* DSP handles external notes */ };

function changePage(d) {
    page = (page + d + PAGES.length) % PAGES.length;
    touchedKnob = -1;
    dirty = true;
}

/* ---------- LEDs ---------- */
function desiredLeds() {
    const out = [];
    for (let i = 0; i < PADS; i++) {
        let c;
        if (shift) {
            c = i < 16 ? (i === track ? trackBright(i) : trackDim(i)) : OFF;
        } else if (padNote[i] >= 0) {
            c = WHITE;
        } else {
            const n = padToNote(i);
            c = n > 127 ? OFF : (n % 12 === 0 ? trackBright(track) : trackDim(track));
        }
        out.push(['n', PAD0 + i, c]);
    }
    for (let i = 0; i < STEPS; i++) {
        const c = shift ? (i === track ? trackBright(i) : trackDim(i)) : OFF;
        out.push(['n', STEP0 + i, c]);
    }
    out.push(['c', CC_LEFT, LED_DIM], ['c', CC_RIGHT, LED_DIM]);
    out.push(['c', CC_UP, octave < 6 ? LED_DIM : OFF], ['c', CC_DOWN, octave > -2 ? LED_DIM : OFF]);
    out.push(['c', CC_SHIFT, shift ? LED_BRIGHT : LED_DIM]);
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
function drawScreen() {
    clear_screen();
    fill_rect(0, 0, 128, 10, 1);
    const hdr = `T${String(track + 1).padStart(2, '0')} FM POLY`;
    print(2, 1, hdr, 0);
    const pg = `${PAGES[page]} ${page + 1}/${PAGES.length}`;
    print(126 - text_width(pg), 1, pg, 0);

    for (let k = 0; k < 8; k++) {
        const pid = page * 8 + k;
        if (pid >= NPARAMS) continue;
        const [name, , max, , fmt] = PARAMS[pid];
        const v = Math.max(0, Math.min(max, values[track][pid] | 0));
        const x = (k % 4) * 32, y = k < 4 ? 12 : 38;
        const hot = k === touchedKnob && tickCount < touchedUntil;
        if (hot) fill_rect(x, y, 32, 25, 1);
        const ink = hot ? 0 : 1;
        print(x + 2, y + 1, name, ink);
        print(x + 2, y + 11, `${fmt(v) ?? '?'}`, ink);
        const w = Math.round((v / max) * 28);
        draw_rect(x + 2, y + 21, 28, 3, ink);
        if (w > 0) fill_rect(x + 2, y + 21, w, 3, ink);
    }

    if (touchedKnob >= 0 && tickCount < touchedUntil && page === 0 && touchedKnob === 0) {
        /* show the algorithm routing while it is being changed */
        fill_rect(0, 54, 128, 10, 0);
        print(2, 55, `ALGO ${ALGO_NAMES[values[track][0]]}`, 1);
    }
}

/* ---------- lifecycle ---------- */
globalThis.init = function () {
    for (let t = 0; t < TRACKS; t++) pullTrack(t);
    ledCache.clear();
    dirty = true;
};

globalThis.onResume = function () {
    ledCache.clear();
    if (typeof invalidateLedCache === 'function') invalidateLedCache();
    pullTrack(track);
    dirty = true;
};

globalThis.onUnload = function () {
    setParam('panic', 1);
};

globalThis.tick = function () {
    tickCount++;
    if (touchedKnob >= 0 && tickCount === touchedUntil) dirty = true;
    flushLeds();
    if (dirty && tickCount % 4 === 0) {
        drawScreen();
        dirty = false;
    }
};

/* exported for tests */
globalThis.__opgrid = { PARAMS, PAGES, values, state: () => ({ track, page, octave, shift }) };
