/* Render OpGrid screenshots: the real UI driving the real engine, drawn as
 * the Move's 128x64 OLED plus its step buttons and pads.
 *   make screens   -> build/screens/*.png */
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import { startHost, root } from './realhost.mjs';

const OUT = path.join(root, 'build/screens');
fs.mkdirSync(OUT, { recursive: true });
const palette = JSON.parse(fs.readFileSync(path.join(root, 'tools/palette.json'), 'utf8'));
const font = JSON.parse(fs.readFileSync(path.join(root, 'tools/font5x7.json'), 'utf8')).glyphs;

/* ---- tiny PNG writer ---- */
const crcTable = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = b => { let c = 0xFFFFFFFF; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function chunk(type, data) {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
}
function png(w, h, rgb) {
    const raw = Buffer.alloc((w * 3 + 1) * h);
    for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3); }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
        chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}

/* ---- colours ---- */
const hex = h => { h = h.replace('#', ''); return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)]; };
function ledRGB(idx) {
    if (idx === undefined || idx === 0) return [28, 28, 30];
    if (palette[idx]) { const c = hex(palette[idx]); return c.every(v => v === 0) ? [28, 28, 30] : c; }
    if (idx >= 65 && idx <= 116) {                 /* dim partners: 63+2i (dark), 64+2i (very dark) */
        const bright = Math.floor((idx - 63) / 2) + ((idx - 63) % 2 ? 1 : 0);
        const base = palette[bright] ? hex(palette[bright]) : [128, 128, 128];
        const f = (idx - 63) % 2 ? 0.45 : 0.22;
        return base.map(v => Math.max(28, Math.round(v * f)));
    }
    return [128, 128, 128];
}
const whiteLed = v => { const g = v ? 60 + Math.round(v / 127 * 195) : 40; return [g, g, g]; };

/* ---- composite ---- */
const W = 600, H = 600, S = 4;
function render(host, name, caption) {
    const img = Buffer.alloc(W * H * 3);
    const put = (x, y, c) => { if (x >= 0 && x < W && y >= 0 && y < H) { const i = (y * W + x) * 3; img[i] = c[0]; img[i + 1] = c[1]; img[i + 2] = c[2]; } };
    const rect = (x, y, w, h, c) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) put(x + i, y + j, c); };
    const text = (x, y, t, c, sc = 2) => {
        let cx = x;
        for (const ch of t) { const g = font[ch] || font['?'];
            g.rows.forEach((row, r) => { for (let k = 0; k < g.w; k++) if (row[k] === '#') rect(cx + k * sc, y + r * sc, sc, sc, c); });
            cx += (g.w + 1) * sc; }
    };
    rect(0, 0, W, H, [18, 18, 20]);
    /* OLED */
    const ox = (W - 128 * S) / 2, oy = 20;
    rect(ox - 6, oy - 6, 128 * S + 12, 64 * S + 12, [5, 5, 6]);
    for (let y = 0; y < 64; y++) for (let x = 0; x < 128; x++)
        if (host.fb[y * 128 + x]) rect(ox + x * S, oy + y * S, S - 1, S - 1, [235, 240, 255]);
    /* buttons: shift, rec, play, loop, mute, copy, delete, undo, capture, menu */
    const btns = [['SHIFT', 49], ['MENU', 50], ['PLAY', 85], ['REC', 86], ['LOOP', 58], ['MUTE', 88], ['CAPT', 52], ['COPY', 60], ['DEL', 119], ['UNDO', 56]];
    btns.forEach(([label, cc], i) => {
        const x = 18 + i * 57, y = oy + 64 * S + 18;
        rect(x, y, 50, 14, whiteLed(host.leds.get('c' + cc)));
        text(x + 2, y + 18, label, [150, 150, 160], 1);
    });
    /* steps */
    const sy = oy + 64 * S + 58;
    for (let i = 0; i < 16; i++) rect(18 + i * 35, sy, 30, 16, ledRGB(host.leds.get('n' + (16 + i))));
    /* pads: row 0 = bottom */
    const py = sy + 30;
    for (let r = 0; r < 4; r++) for (let c = 0; c < 8; c++) {
        const idx = 68 + (3 - r) * 8 + c;
        rect(18 + c * 71, py + r * 46, 64, 40, ledRGB(host.leds.get('n' + idx)));
    }
    text(18, H - 24, caption, [200, 200, 210], 2);
    fs.writeFileSync(path.join(OUT, `${name}.png`), png(W, H, img));
    console.log(`wrote build/screens/${name}.png`);
}

/* ---- scenarios ---- */
const h = await startHost();
const { ui } = h;
globalThis.init();
h.tick(60);
const jogTo = p => { let n = 0; while (ui.state().page !== p && n++ < 30) h.midi(0xB0, 14, ui.state().page > p ? 127 : 1); };
const pad = (i, v = 100) => { h.note(68 + i, v); h.noteOff(68 + i); };
const step = i => { h.note(16 + i); h.noteOff(16 + i); };
const settle = () => h.tick(80);

/* a small groove on tracks 1-3 */
for (const s of [0, 4, 8, 12]) step(s);
for (const s of [3, 6, 10, 14]) step(s);
h.knob(0, 2); h.knob(5, 20);                                   /* algo 3, more op2 */
h.hold(49, true); pad(1); h.hold(49, false);                   /* track 2 */
for (const s of [2, 6, 10, 14]) step(s);
h.hold(49, true); pad(0); h.hold(49, false);                   /* back to track 1 */
h.cc(85);                                                      /* play */
settle();
jogTo(0); h.knob(0, 1); h.tick(8);
render(h, '1-fm-page', 'FM page, playing (algo shown while turned)');

h.hold(49, true); h.cc(63); h.cc(63); h.cc(54); h.hold(49, false);  /* key: D minor */
pad(10);                                                       /* select a note */
h.tick(12);
render(h, '11-note-pads', 'Pads in key (D minor): root, used, selected');
h.hold(49, true); h.cc(62); h.cc(62); h.cc(55); h.hold(49, false);

jogTo(2);
h.note(16 + 4);                                                /* hold step 5 */
h.knob(0, -30); h.knob(1, 40);
h.tick(12);
render(h, '2-step-lock', 'Holding step 5: filter locks on that step');
h.noteOff(16 + 4);

jogTo(8);
h.note(16 + 6);
for (let i = 0; i < 7; i++) h.knob(6, 1);                     /* condition 1:2 */
h.knob(5, 2); h.knob(4, 6);                                    /* ratchet, micro */
h.tick(12);
render(h, '3-step-page', 'STEP page: note, length, micro, ratchet, condition');
h.noteOff(16 + 6);
h.tick(60);

jogTo(4);
h.knob(0, 18); h.knob(1, 1); h.knob(4, 30);
h.tick(12);
render(h, '4-lfo-page', 'LFO 1 on cutoff, synced');

h.cc(58);                                                      /* clip grid */
const slot = (t, sc) => 24 - sc * 8 + t;                        /* pad index for track t, scene sc (top row = scene 1) */
pad(slot(2, 1));                                                /* track 3, scene 2: empty -> select for editing */
h.cc(58); step(0); step(8); h.cc(58);                           /* give it two steps */
pad(slot(2, 1));                                                /* now launch it (queued to the bar) */
pad(slot(0, 0));                                                /* relaunch track 1 */
h.tick(700);
render(h, '5-clip-grid', 'Clip grid: 8 tracks x 4 scenes');
h.cc(58);

h.cc(88);                                                      /* mixer */
h.knob(1, -30); h.knob(2, -12);
h.tick(30);
render(h, '6-mixer', 'Mixer: pads mute/select, knobs = volume');
h.cc(88);

h.hold(49, true); h.cc(58); h.hold(49, false);                 /* song view */
h.tick(20);
h.midi(0xB0, 3, 127);                                          /* add row */
h.midi(0xB0, 60, 127); h.midi(0xB0, 60, 0);                    /* duplicate */
h.knob(1, -2); h.midi(0x90, 68 + 4, 100);
h.midi(0xB0, 60, 127); h.midi(0xB0, 60, 0);
h.knob(1, 4); h.knob(3, 1);
h.tick(700);
render(h, '7-song', 'Song view: rows of scenes, bars x repeats');
h.hold(49, true); h.cc(58); h.hold(49, false);

h.cc(50);                                                      /* menu */
h.tick(12);
render(h, '8-menu', 'Project menu');
/* sound list: starter sounds from the module dir */
h.files.set('/data/UserData/schwung/modules/overtake/opgrid/sounds.json',
    fs.readFileSync(path.join(root, 'module/sounds.json'), 'utf8'));
h.cc(50); h.cc(50);
h.midi(0xB0, 3, 127);                                          /* SOUNDS */
h.midi(0xB0, 14, 1); h.midi(0xB0, 14, 1);
h.midi(0xB0, 3, 127);                                          /* load the 3rd starter sound */
h.tick(12);
render(h, '9-sounds', 'Sound list: click loads, list stays open');
h.cc(50); h.tick(4); if (h.ui.state().menuOpen) h.cc(50);
/* the loaded sound is FM Perc: its own page */
jogTo(0);
h.knob(0, 1);
h.tick(700);
render(h, '10-perc-page', 'FM Perc: pads become a drum grid');

h.stop();
