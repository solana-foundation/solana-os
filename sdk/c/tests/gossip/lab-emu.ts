// lab-emu.ts: the Lab in the headless emulator (web/static/emulator, `bun run
// emulator` in the parent repo), every screen against the states of states.ts,
// a screenshot of each, and `lua mem` after each run (the interpreter's most
// instructions in one callback, the app's heap peak).
//
//   bun firmware/sdk/c/tests/gossip/lab-emu.ts --shots <dir> [--app <gossip-lab dir>] [--emu <emulator dir>]
//
// The foundation's gossip module answers nothing yet, so this builds a test copy
// of the Lab whose lab_source.c is lab_source_feed.c: the module's tables come
// from feed.json on the emulated card, which the test rewrites to change the
// state. The shipped app is untouched. Exit 1 on an app error or a missing screen.
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { png } from './png';
import { STATES } from './states';

const args = Bun.argv.slice(2);
const opt = (k: string) => {
    const i = args.indexOf(k);
    return i >= 0 ? args[i + 1] : undefined;
};
const here = import.meta.dir;
const fw = resolve(here, '../../../..');
const app = resolve(opt('--app') ?? join(fw, '../server/content/apps/gossip-lab'));
const emuDir = resolve(opt('--emu') ?? join(fw, '../web/static/emulator'));
const shots = resolve(opt('--shots') ?? mkdtempSync(join(tmpdir(), 'lab-shots-')));
mkdirSync(shots, { recursive: true });
if (!existsSync(join(emuDir, 'solana-os.js'))) {
    console.error(`lab-emu: no emulator build in ${emuDir}`);
    process.exit(2);
}

// The test copy of the Lab.
const work = mkdtempSync(join(tmpdir(), 'lab-emu-'));
cpSync(app, join(work, 'gossip-lab'), { recursive: true });
cpSync(join(here, 'lab_source_feed.c'), join(work, 'gossip-lab', 'lab_source.c'));
const wasm = join(work, 'app.wasm');
const cc = Bun.spawnSync([join(fw, 'tools/cc/badge-cc'), join(work, 'gossip-lab'), '-o', wasm], { stdout: 'inherit', stderr: 'inherit' });
if (cc.exitCode !== 0) process.exit(1);

// The emulator, as firmware/main/lua/tests/c/run.ts boots it.
const lines: string[] = [];
const g = globalThis as any;
g.window = globalThis;
g.location = { origin: 'http://127.0.0.1:9', href: 'http://127.0.0.1:9/emulator', protocol: 'http:', host: '127.0.0.1:9', hostname: '127.0.0.1', pathname: '/emulator', search: '' };
const nvs = new Map<string, string>();
g.localStorage = {
    getItem: (k: string) => (nvs.has(k) ? nvs.get(k)! : null),
    setItem: (k: string, v: string) => void nvs.set(k, String(v)),
    removeItem: (k: string) => void nvs.delete(k),
    clear: () => nvs.clear(),
    key: (i: number) => [...nvs.keys()][i] ?? null,
    get length() { return nvs.size; },
};
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const fake = Bun.serve({
    port: 0,
    fetch(req) {
        const u = new URL(req.url);
        if (u.pathname === '/api/badge/hello') {
            return Response.json({ id: 'eTEST1', secret: 'test-secret', key: '00112233445566778899aabbccddeeff', claimed: false, claimUrl: 'http://127.0.0.1/claim?b=eTEST1&c=ABCD2345', poll: 600, mesh: 1, time: Date.now() });
        }
        return Response.json({ error: 'not here' }, { status: 404 });
    },
});
const SolanaOS = new Function(readFileSync(join(emuDir, 'solana-os.js'), 'utf8') + '; return SolanaOS;')();
const mod = await SolanaOS({
    locateFile: (p: string) => 'file://' + join(emuDir, p),
    print: (l: string) => void lines.push(l),
    printErr: (l: string) => void lines.push(l),
    server: `http://127.0.0.1:${fake.port}`,
});
const cmd = (l: string) => mod.ccall('emu_console', null, ['string'], [l]);
for (let i = 0; i < 200 && !lines.some((l) => l.includes('console ready')); i++) await sleep(50);
// The emulator's one network: a hello to the stand-in server gives the badge a clock
// (the timers count down, the strip says when it synced).
cmd('wifi connect "Emulator Guest"');
await sleep(4000);
if (process.env.VERBOSE) console.log(lines.join('\n'));

const ID = 'gossip-lab';
const dir = `/sdcard/hh/apps/${ID}`;
mod.FS.mkdirTree(`${dir}/files`);
mod.FS.writeFile(`${dir}/app.json`, readFileSync(join(app, 'app.json')));
mod.FS.writeFile(`${dir}/app.wasm`, readFileSync(wasm));
const feed = (state: string) => mod.FS.writeFile(`${dir}/files/feed.json`, JSON.stringify(STATES[state]()));

const problems: string[] = [];
const shotList: string[] = [];
function shot(name: string) {
    const ptr = mod.ccall('emu_framebuffer', 'number', [], []) as number;
    const d = mod.HEAPU8.subarray(ptr, ptr + 320 * 240 * 4);
    const file = join(shots, `${name}.png`);
    writeFileSync(file, png(320, 240, (x, y) => {
        const k = (y * 320 + x) * 4;
        return [d[k], d[k + 1], d[k + 2]];
    }));
    shotList.push(file);
}
const screen = () => {
    for (let k = lines.length - 1; k >= 0; k--) {
        const m = lines[k].match(/\] lab: (\S+)/);
        if (m) return m[1];
    }
    return '';
};
async function keys(...ks: string[]) {
    for (const k of ks) {
        cmd(`key ${k}`);
        await sleep(350);
    }
}
async function at(name: string, file?: string, wait = 900) {
    await sleep(wait);
    if (screen() !== name) problems.push(`expected screen ${name}, on ${screen()}`);
    if (file) shot(file);
}
const mems: string[] = [];
async function run(state: string, walk: () => Promise<void>) {
    feed(state);
    homeRow = 0;
    const mark = lines.length;
    cmd(`lua run ${ID}`);
    await sleep(1500);
    await walk();
    cmd('open home');
    await sleep(800);
    cmd('lua mem');
    await sleep(400);
    const out = lines.slice(mark);
    for (const l of out.filter((x) => x.startsWith(`[lua:${ID}]`))) problems.push(`${state}: ${l}`);
    const mem = out.filter((l) => /most wasm|peak|heap|running|last run/.test(l)).map((l) => l.trim());
    mems.push(`${state}: ${mem.join(' | ')}`);
}

// The home list keeps its row when a screen above it closes (the Lab remembers
// selections), so moves are relative to where it was left. Each run starts at 0.
let homeRow = 0;
const open = async (row: number) => {
    const d = row - homeRow;
    homeRow = row;
    await keys(...Array(Math.abs(d)).fill(d > 0 ? 'down' : 'up'), 'select');
};

await run('unlinked', async () => {
    await at('unlinked', 'unlinked');
});
await run('fresh', async () => {
    await at('intro', 'intro-1-gossip');
    await keys('select');
    await at('intro', 'intro-2-class');
    await keys('down', 'select');
    await at('intro', 'intro-3-deploy');
    await keys('select');
    await at('home', 'home-fresh');
});
await run('empty', async () => {
    await at('home', 'home-empty');
    await open(1);
    await at('slots', 'slots-empty');
    await keys('back');
    await open(2);
    await at('radar', 'radar-empty');
    await keys('back');
    await open(4);
    await at('items', 'items-empty');
    await keys('back');
    await open(6);
    await at('objectives', 'objectives-empty');
    await keys('back');
});
await run('busy', async () => {
    await at('home', 'home-busy');
    await open(0);
    await at('node', 'node', 2500);
    await keys('back');
    await open(1);
    await at('slots', 'slots', 3000); // the creatures render over a few ticks
    await keys('select');
    await at('program', 'program', 2000);
    await keys('select');
    await at('program', 'program-actions');
    await keys('back', 'back', 'down', 'select');
    await at('program', 'program-named-later', 1500);
    await keys('back', 'back');
    await open(4);
    await at('items', 'items');
    await keys('down', 'down', 'down', 'down', 'down', 'select');
    await at('item', 'item-cpi');
    await keys('select');
    await at('pick', 'pick-cpi');
    await keys('back', 'back', 'back');
    await open(5);
    await at('timers', 'timers');
    await keys('back');
    await open(6);
    await at('objectives', 'objectives');
    await keys('back');
    await open(7);
    await at('trophies', 'trophies');
    await keys('back');
    await open(8);
    await at('settings', 'settings');
    await keys('back');
});
await run('nearby', async () => {
    await open(2);
    await at('radar', 'radar-20', 2500);
    await keys('down', 'down', 'down');
    await at('radar', 'radar-20-moved', 1200);
    await keys('select'); // the fourth strongest: the Leader
    await at('peer', 'peer-role', 2500);
    await keys('back', 'up', 'up', 'up', 'select');
    await at('peer', 'peer-badge', 2500);
    await keys('back', 'back');
    await open(3);
    await at('hearing', 'hearing', 2000);
    await keys('back');
});
await run('exposure', async () => {
    await open(3);
    await at('hearing', 'hearing-exposed', 2000);
    await keys('back');
});
await run('offline', async () => {
    await at('home', 'home-offline', 1500);
});
// The cost of one screen: a run that opens it alone and stays 5 s (the most
// WebAssembly instructions any callback took, from `lua mem`; the home screen's
// own launch is the baseline).
const costs: string[] = [];
const peak = () => {
    const l = mems[mems.length - 1].match(/most wasm instructions in one callback (\d+)/);
    return l ? Number(l[1]) : -1;
};
for (const [label, state, path] of [
    ['launch and home', 'busy', []],
    ['node card', 'busy', [0]],
    ['slots (5 creatures, 8 rows)', 'busy', [1]],
    ['a program (creature at 2x)', 'busy', [1, 'select']],
    ['radar, 20 nodes, list moving', 'nearby', [2, 'down', 'down', 'down', 'down', 'down', 'down', 'down', 'down']],
    ['a nearby node with its genome', 'nearby', [2, 'down', 'select']],
    ['hearing, 6 meters', 'exposure', [3]],
    ['items, 12', 'busy', [4]],
    ['timers', 'busy', [5]],
    ['objectives', 'busy', [6]],
] as [string, string, (number | string)[]][]) {
    await run(state, async () => {
        for (const step of path) {
            if (typeof step === 'number') await open(step);
            else await keys(step);
            await sleep(500);
        }
        await sleep(5000);
    });
    costs.push(`${label}: ${peak()}`);
}

mod.ccall('emu_halt', null, [], []);
fake.stop(true);
console.log('lab-emu: lua mem after each run:');
for (const m of mems) console.log(`  ${m}`);
console.log('lab-emu: the most WebAssembly instructions in one callback (cap 2000000), per screen:');
for (const c of costs) console.log(`  ${c}`);
console.log(`lab-emu: ${shotList.length} shots in ${shots}`);
for (const p of problems) console.log(`lab-emu: PROBLEM ${p}`);
process.exit(problems.length ? 1 : 0);
