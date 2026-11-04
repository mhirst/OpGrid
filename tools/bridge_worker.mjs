/* Worker side of the synchronous bridge: forwards one request at a time from
 * the main thread (which blocks in Atomics.wait) to the C bridge process. */
import { workerData } from 'worker_threads';
import { spawn } from 'child_process';

const { sab, bin } = workerData;
const ctrl = new Int32Array(sab, 0, 2);      /* [state, length]: 1 = request, 2 = reply */
const data = new Uint8Array(sab, 8);
const child = spawn(bin, [], { stdio: ['pipe', 'pipe', 'inherit'] });
let pending = Buffer.alloc(0);
let waiter = null;
child.stdout.on('data', chunk => {
    pending = Buffer.concat([pending, chunk]);
    const nl = pending.indexOf(10);
    if (nl >= 0 && waiter) {
        const line = pending.subarray(0, nl);
        pending = pending.subarray(nl + 1);
        const w = waiter; waiter = null; w(line);
    }
});
const readLine = () => new Promise(res => {
    const nl = pending.indexOf(10);
    if (nl >= 0) { const line = pending.subarray(0, nl); pending = pending.subarray(nl + 1); res(line); }
    else waiter = res;
});
for (;;) {
    await Atomics.waitAsync(ctrl, 0, 0).value;
    if (Atomics.load(ctrl, 0) !== 1) continue;
    const req = Buffer.from(data.subarray(0, ctrl[1])).toString('latin1');
    if (req === 'QUIT') { child.stdin.end(); process.exit(0); }
    child.stdin.write(req + '\n');
    const line = await readLine();
    data.set(line, 0);
    ctrl[1] = line.length;
    Atomics.store(ctrl, 0, 2);
    Atomics.notify(ctrl, 0);
}
