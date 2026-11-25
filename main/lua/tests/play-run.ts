// Runs t-play (Solana OS 2.5.0: canvas parts and turns, changed-area redraws,
// ui.frame, on_key_up, collectgarbage, sys.budget, the main chunk's budget,
// ui.load's errors) in the emulator, headless, reading the screen from the
// emulated framebuffer and pressing buttons on the raw lines:
//   bun firmware/main/lua/tests/play-run.ts
// Loads web/static/emulator/solana-os.js (bun run emulator), as svc-run.ts.
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const here = import.meta.dir;
const emuDir = join(here, '../../../../web/static/emulator');

const lines: string[] = [];
const g = globalThis as any;
g.window = globalThis;
g.location = {
    origin: 'http://127.0.0.1:9', href: 'http://127.0.0.1:9/emulator', protocol: 'http:', host: '127.0.0.1:9',
    hostname: '127.0.0.1', pathname: '/emulator', search: '',
};
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

const src = readFileSync(join(emuDir, 'solana-os.js'), 'utf8');
const SolanaOS = new Function(src + '; return SolanaOS;')();
const mod = await SolanaOS({
    locateFile: (p: string) => 'file://' + join(emuDir, p),
    print: (l: string) => void lines.push(l),
    printErr: (l: string) => void lines.push(l),
    server: 'http://127.0.0.1:9',
});
const cmd = (l: string) => mod.ccall('emu_console', null, ['string'], [l]);
for (let i = 0; i < 200 && !lines.some((l) => l.includes('console ready')); i++) {
    await sleep(50);
}
await sleep(500);

// Copy an app folder onto the emulated card: app.json, main.lua (or app.wasm), files/.
const mkdirp = (p: string) => {
    let cur = '';
    for (const part of p.split('/').filter(Boolean)) {
        cur += '/' + part;
        try { mod.FS.mkdir(cur); } catch { /* exists */ }
    }
};
const put = (id: string, dir: string) => {
    const dest = `/sdcard/hh/apps/${id}`;
    const walk = (from: string, to: string) => {
        mkdirp(to);
        for (const name of readdirSync(from)) {
            const p = join(from, name);
            if (statSync(p).isDirectory()) {
                walk(p, `${to}/${name}`);
            } else {
                mod.FS.writeFile(`${to}/${name}`, readFileSync(p));
            }
        }
    };
    walk(dir, dest);
};

let failures = 0;
const ok = (cond: boolean, what: string) => {
    console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`);
    if (!cond) failures++;
};
const since = (mark: number) => lines.slice(mark);
const waitFor = async (re: RegExp, ms: number, mark = 0) => {
    for (let t = 0; t < ms; t += 50) {
        const hit = since(mark).find((l) => re.test(l));
        if (hit) return hit;
        await sleep(50);
    }
    return undefined;
};
const grab = async (re: RegExp, ms = 3000, mark = 0) => (await waitFor(re, ms, mark))?.match(re) ?? null;

// The screen: RGBA words (0xAABBGGRR), 320 x 240; the app area starts 22 rows down.
const W = 320, TOP = 22;
const fb = () => new Uint32Array(mod.HEAPU8.buffer, mod.ccall('emu_framebuffer', 'number', [], []), W * 240);
const px = (x: number, y: number) => {
    const v = fb()[(TOP + y) * W + x];
    return ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);   // 0xRRGGBB
};
const hex = (v: number) => '0x' + v.toString(16).padStart(6, '0');
const buttons = (bits: number) => mod.ccall('emu_buttons_set', null, ['number'], [bits]);
const BTN_DOWN = 1 << 3, BTN_BACK = 1 << 5;     // PB4, PB6 (os_keys.h, uncalibrated)

put('t-play', join(here, 't-play'));
let mark = lines.length;
cmd('lua run t-play');
ok(!!(await waitFor(/t-play ready/, 8000, mark)), 't-play started');
const left = await grab(/t-play main budget left (\d+)/, 1000, mark);
ok(!!left && Number(left[1]) > 1000000 && Number(left[1]) < 1500000, `main chunk ran 600 000 instructions with room left (${left?.[1]})`);
const tick = await grab(/t-play tick budget (\d+)/, 2000, mark);
ok(!!tick && Number(tick[1]) <= 200000 && Number(tick[1]) > 150000, `a tick has the usual 200 000 (${tick?.[1]})`);
ok(!!(await waitFor(/t-play gc 0 0 false true number/, 100, mark)), 'collectgarbage: collect, step, isrunning, count');
ok(!!(await waitFor(/t-play gc bad: .*invalid option 'setpause'/, 100, mark)), 'collectgarbage: an unknown option is still an error');
ok(!!(await waitFor(/t-play load empty path: .*the path is empty/, 100, mark)), 'ui.load(""): says the path is empty');
ok(!!(await waitFor(/t-play load empty rows: .*every pixel art row is empty/, 100, mark)), 'ui.load({""}): says the rows are empty');
ok(!!(await waitFor(/t-play load no rows: .*table of pixel art rows is empty/, 100, mark)), 'ui.load({}): says there are no rows');
ok(!!(await waitFor(/t-play turns 24 cases, 0 wrong/, 100, mark)), 'c:image part x rotate x flip_x x flip_y: every pixel where it belongs');
ok(!!(await waitFor(/t-play turn clipped: true/, 100, mark)), 'c:image turned and clipped at the top-left');
ok(!!(await waitFor(/t-play removed mid-call: /, 100, mark)), 'c:image on a canvas its options removed: the badge carries on');
ok(!!(await waitFor(/t-play part outside: .*isn't inside the 4 x 3 image/, 100, mark)), 'c:image: a part outside the image is an error');
ok(!!(await waitFor(/t-play bad rotate: .*rotate is 0, 90, 180 or 270/, 100, mark)), 'c:image: rotate 45 is an error');

// Changed-area redraws: each step must reach the screen, and the earlier ones stay.
await sleep(600);
const cx = (x: number) => 200 + x * 4 + 1, cy = (y: number) => 40 + y * 4 + 1;
ok(px(cx(2), cy(2)) === 0x000000, `canvas starts black (${hex(px(cx(2), cy(2)))})`);
for (let step = 1; step <= 3; step++) {
    mark = lines.length;
    cmd('key right');
    await waitFor(new RegExp(`t-play drew ${step}`), 2000, mark);
    await sleep(300);
}
ok(px(cx(2), cy(2)) === 0x00ff00, `pixel drawn in step 1 on screen (${hex(px(cx(2), cy(2)))})`);
ok(px(cx(16), cy(16)) === 0x0000ff, `rect drawn in step 2 on screen (${hex(px(cx(16), cy(16)))})`);
ok(px(cx(1), cy(1)) === 0x000000, `untouched canvas pixels stay black (${hex(px(cx(1), cy(1)))})`);
const a = px(cx(7), cy(6)), b = px(cx(6), cy(6));
ok(a !== 0 && b !== 0 && a !== b, `mirrored part on screen: ${hex(b)} then ${hex(a)}`);

// on_key_up: a real press held ~300 ms, then a console tap.
mark = lines.length;
buttons(BTN_DOWN);
await sleep(320);
buttons(0);
const up = await grab(/t-play up down (\d+)/, 2000, mark);
ok(!!(await waitFor(/t-play key down$/, 100, mark)), 'on_key("down") on the press');
ok(!!up && Number(up[1]) >= 250 && Number(up[1]) < 700, `on_key_up("down", ${up?.[1]}) after a ~320 ms hold`);
mark = lines.length;
cmd('key down');
ok(!!(await waitFor(/t-play up down 0/, 2000, mark)), 'a console key is a press and its release (ms 0)');

// ui.frame: shown, outside, an image object; then under a dialog.
mark = lines.length;
cmd('key select');
ok(!!(await waitFor(/t-play frame shown: true/, 2000, mark)), 'ui.frame of a hidden canvas: true');
ok(!!(await waitFor(/t-play frame outside: .*isn't inside the app/, 200, mark)), 'ui.frame outside the app area: an error');
ok(!!(await waitFor(/t-play frame image: true/, 200, mark)), 'ui.frame of a ui.load image: true');
await sleep(300);
ok(px(20, 160) === 0xff0000, `the frame is on screen at 2x (${hex(px(20, 160))})`);
ok(px(49, 169) === 0xff0000 && px(51, 171) !== 0xff0000, `and ends where 20 x 10 at 2x does (${hex(px(49, 169))}, ${hex(px(51, 171))})`);
mark = lines.length;
cmd('key left');
ok(!!(await waitFor(/t-play frame under dialog: false/, 2000, mark)), 'ui.frame under ui.confirm: false');
await sleep(300);
mark = lines.length;
buttons(BTN_BACK);
await sleep(150);
buttons(0);
ok(!!(await waitFor(/t-play confirm false/, 2000, mark)), 'CANCEL answered the dialog');
await sleep(400);
ok(!since(mark).some((l) => /t-play (key|up) back/.test(l)), 'the dialog\'s CANCEL reached neither on_key nor on_key_up');

mark = lines.length;
cmd('key up');
await sleep(800);
cmd('lua mem');
const mem = await grab(/steps|peak/, 2000, mark);
console.log(since(mark).filter((l) => /t-play|lua/.test(l)).slice(-6).join('\n'));
const errs = lines.filter((l) => /t-play.*(error|took too long)/i.test(l) && !/^t-play (gc bad|load|part|bad|frame outside)/.test(l.replace(/^\[t-play\] /, '')));
ok(errs.length === 0, `no script errors (${errs.join(' | ')})`);
console.log(failures ? `${failures} FAILED` : 'all ok');
process.exit(failures ? 1 : 0);
