/* help.json: Schwung reads only `children`, draws each line at x=4 with no
 * wrapping, and has glyphs for ASCII only. Measure every line in pixels. */
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const help = JSON.parse(fs.readFileSync(path.join(root, 'module/help.json'), 'utf8'));
const font = JSON.parse(fs.readFileSync(path.join(root, 'tools/font5x7.json'), 'utf8')).glyphs;
const width = t => [...t].reduce((w, ch) => w + (font[ch] ? font[ch].w : 0) + 1, 0);
let failures = 0, lines = 0;
const fail = m => { failures++; console.log('FAIL', m); };
if (!Array.isArray(help.children) || !help.children.length) fail('top level needs children');
(function walk(nodes, where) {
    for (const n of nodes) {
        if (typeof n.title !== 'string') fail(`${where}: node without title`);
        if (width(n.title) > 120) fail(`${where}/${n.title}: title too wide`);
        if (n.children) walk(n.children, `${where}/${n.title}`);
        else if (Array.isArray(n.lines)) for (const l of n.lines) {
            lines++;
            if (!/^[\x20-\x7e]*$/.test(l)) fail(`${n.title}: non-ASCII in "${l}"`);
            if (4 + width(l) > 128) fail(`${n.title}: "${l}" is ${4 + width(l)} px wide`);
        } else fail(`${where}/${n.title}: neither children nor lines`);
    }
})(help.children, '');
if (failures) { console.log(`${failures} help check(s) FAILED`); process.exit(1); }
console.log(`help.json: ${lines} lines, all fit the screen`);
