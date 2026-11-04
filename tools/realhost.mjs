/* Run src/ui.js against the real C engine (via tools/bridge), with a 128x64
 * framebuffer using Schwung's 5x7 font, captured LEDs and an in-memory
 * filesystem. Used by tools/screens.mjs and test/e2e_test.mjs. */
import fs from 'fs';
import os from 'os';
import path from 'path';
import { Worker } from 'worker_threads';
import { fileURLToPath, pathToFileURL } from 'url';

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const font = JSON.parse(fs.readFileSync(path.join(root, 'tools/font5x7.json'), 'utf8')).glyphs;

export async function startHost() {
    const bin = path.join(root, 'build/bridge');
    const sab = new SharedArrayBuffer(8 + (1 << 18));
    const ctrl = new Int32Array(sab, 0, 2), data = new Uint8Array(sab, 8);
    const worker = new Worker(path.join(root, 'tools/bridge_worker.mjs'), { workerData: { sab, bin } });
    const call = req => {
        const b = Buffer.from(req, 'latin1');
        data.set(b, 0); ctrl[1] = b.length;
        Atomics.store(ctrl, 0, 1); Atomics.notify(ctrl, 0);
        while (Atomics.load(ctrl, 0) !== 2) Atomics.wait(ctrl, 0, 1, 5000);
        const out = Buffer.from(data.subarray(0, ctrl[1])).toString('latin1');
        Atomics.store(ctrl, 0, 0);
        return out;
    };

    /* framebuffer */
    const fb = new Uint8Array(128 * 64);
    const px = (x, y, v) => { if (x >= 0 && x < 128 && y >= 0 && y < 64) fb[y * 128 + x] = v ? 1 : 0; };
    const glyph = ch => font[ch] || font['?'];
    globalThis.clear_screen = () => fb.fill(0);
    globalThis.set_pixel = px;
    globalThis.fill_rect = (x, y, w, h, v) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) px(x + i, y + j, v); };
    globalThis.draw_rect = (x, y, w, h, v) => {
        for (let i = 0; i < w; i++) { px(x + i, y, v); px(x + i, y + h - 1, v); }
        for (let j = 0; j < h; j++) { px(x, y + j, v); px(x + w - 1, y + j, v); }
    };
    globalThis.text_width = t => [...`${t}`].reduce((w, ch) => w + glyph(ch).w + 1, 0);
    globalThis.print = (x, y, t, color) => {
        let cx = x;
        for (const ch of `${t}`) {
            const g = glyph(ch);
            g.rows.forEach((row, r) => { for (let c = 0; c < g.w; c++) if (row[c] === '#') px(cx + c, y + r, color); });
            cx += g.w + 1;
        }
    };

    /* host functions */
    const files = new Map();
    const leds = new Map();
    globalThis.__files = files;
    globalThis.__leds = leds;
    globalThis.host_module_set_param = (k, v) => { call(`S ${k}\t${v}`); };
    globalThis.host_module_get_param = k => { const r = call(`G ${k}`); return r === '\x01' ? null : r; };
    globalThis.host_write_file = (p, t) => { files.set(p, `${t}`); return true; };
    globalThis.host_read_file = p => files.has(p) ? files.get(p) : null;

    /* stub shared modules */
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'opgrid-real-'));
    fs.writeFileSync(path.join(tmp, 'input_filter.mjs'), `
export function setLED(n, c) { globalThis.__leds.set('n' + n, c); }
export function setButtonLED(n, c) { globalThis.__leds.set('c' + n, c); }
export function decodeDelta(v) { return v === 0 ? 0 : v < 64 ? 1 : -1; }
export function decodeAcceleratedDelta(v) { return v === 0 ? 0 : v < 64 ? v : v - 128; }
export function invalidateLedCache() {}
`);
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
        .replace('/data/UserData/schwung/shared/input_filter.mjs', pathToFileURL(path.join(tmp, 'input_filter.mjs')).href)
        .replace("from 'os'", `from '${pathToFileURL(path.join(tmp, 'os.mjs')).href}'`);
    fs.writeFileSync(path.join(tmp, 'ui.mjs'), src);
    await import(pathToFileURL(path.join(tmp, 'ui.mjs')).href);

    const host = {
        call, fb, leds, files, ui: globalThis.__opgrid,
        midi: (...d) => globalThis.onMidiMessageInternal(d),
        /* one UI tick per audio block, like the device */
        tick: (n = 1) => { for (let i = 0; i < n; i++) { call('R 1'); globalThis.tick(); } },
        cc: (n, v = 127) => { globalThis.onMidiMessageInternal([0xB0, n, v]); if (v) globalThis.onMidiMessageInternal([0xB0, n, 0]); },
        hold: (n, on) => globalThis.onMidiMessageInternal([0xB0, n, on ? 127 : 0]),
        note: (n, v = 100) => globalThis.onMidiMessageInternal([0x90, n, v]),
        noteOff: n => globalThis.onMidiMessageInternal([0x80, n, 0]),
        knob: (k, d) => globalThis.onMidiMessageInternal([0xB0, 71 + k, d >= 0 ? d : 128 + d]),
        stop: () => {
            const b = Buffer.from('QUIT'); data.set(b, 0); ctrl[1] = b.length;
            Atomics.store(ctrl, 0, 1); Atomics.notify(ctrl, 0);        /* no reply comes back */
            worker.terminate();
            fs.rmSync(tmp, { recursive: true, force: true });
        },
    };
    return host;
}
