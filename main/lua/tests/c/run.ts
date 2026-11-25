// Runs the C app regression apps (build.sh's out/) in the emulator, headless,
// and checks what they print:
//   bun firmware/main/lua/tests/c/run.ts [id ...]
// Loads web/static/emulator/solana-os.js (bun run emulator) with the browser
// bits stubbed (window, location, localStorage: the emulated NVS), writes
// each app onto the emulated card, runs `lua run <id>`, presses keys where
// the app wants them, closes it and reads `lua mem`. No server: every server
// call answers offline. Exit code 1 when anything is off.
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const here = import.meta.dir;
const emuDir = join(here, '../../../../../web/static/emulator');
const outDir = join(here, 'out');

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

// A stand-in for the badge server, just enough for the store installer
// (server/API.md "Device API", "C apps"): hello registers the emulated badge,
// source answers an empty body with the manifest, code serves app.wasm (and
// 404 for app.aot unless asked to offer one).
const store: { id: string; json: any; wasm: Uint8Array; aot: boolean; asked: string[] } = {
    id: 't-c-store', json: null, wasm: new Uint8Array(), aot: false, asked: [],
};
const fake = Bun.serve({
    port: 0,
    fetch(req) {
        const u = new URL(req.url);
        store.asked.push(`${req.method} ${u.pathname}${u.search}`);
        if (u.pathname === '/api/badge/hello') {
            return Response.json({
                id: 'eTEST1', secret: 'test-secret', key: '00112233445566778899aabbccddeeff', claimed: false,
                claimUrl: 'http://127.0.0.1/claim?b=eTEST1&c=ABCD2345', poll: 600, mesh: 1, time: Date.now(),
            });
        }
        if (u.pathname === `/api/device/apps/${store.id}/source`) {
            return new Response('', { headers: { 'X-App-Manifest': Buffer.from(JSON.stringify(store.json)).toString('base64') } });
        }
        if (u.pathname === `/api/device/apps/${store.id}/code`) {
            return u.searchParams.get('f') === 'wasm' ? new Response(store.wasm) : new Response('not found', { status: 404 });
        }
        return Response.json({ error: 'not here' }, { status: 404 });
    },
});
const src = readFileSync(join(emuDir, 'solana-os.js'), 'utf8');
const SolanaOS = new Function(src + '; return SolanaOS;')();
const mod = await SolanaOS({
    locateFile: (p: string) => 'file://' + join(emuDir, p),
    print: (l: string) => void lines.push(l),
    printErr: (l: string) => void lines.push(l),
    server: `http://127.0.0.1:${fake.port}`,
});
const cmd = (l: string) => mod.ccall('emu_console', null, ['string'], [l]);
const current = () => mod.ccall('emu_current_app', 'string', [], []);

for (let i = 0; i < 200 && !lines.some((l) => l.includes('console ready')); i++) {
    await sleep(50);
}
await sleep(500);

const rmTree = (path: string) => {
    for (const name of mod.FS.readdir(path).filter((n: string) => n !== '.' && n !== '..')) {
        const p = `${path}/${name}`;
        if (mod.FS.isDir(mod.FS.stat(p).mode)) {
            rmTree(p);
        } else {
            mod.FS.unlink(p);
        }
    }
    mod.FS.rmdir(path);
};

type Expect = { error?: RegExp; title?: string; print?: RegExp[]; noError?: boolean; keys?: string[]; wait?: number; oks?: number };
// What each app must do. error: the console's "[lua:<id>] <title>: <message>" line.
const expect: Record<string, Expect> = {
    't-c-api': { noError: true, keys: ['down', 'select', 'right'], print: [/on_start done/, /\] done$/, /\] on_exit$/] },
    't-c-libc': { noError: true, print: [/\] printf 42 str 3\.14 ff$/, /\] done$/, /\] stderr line$/] },
    't-c-import': { title: 'App error', error: /env\.launch_rockets, which the badge doesn't have/ },
    // ui_frame and files_read (2.3.0): every check answers, misuse stops the app.
    't-c-frame': { noError: true, oks: 25, print: [/\] done$/, /\] still running$/] },
    // ui_frame timings (numbers for a badge; the emulator only shows it runs).
    't-c-fstress': { noError: true, print: [/\] t-c-fstress frames shown: 200$/], wait: 5000 },
    't-c-fbench': { noError: true, print: [/\] t-c-fbench shown of 20: 20$/, /\] done$/], wait: 4000 },
    't-c-fr-pointer': { title: 'App error', error: /sos_frame: the pointer 0x7ffffff0 \(1024 bytes\) is outside the app's memory/ },
    't-c-fr-side': { title: 'App error', error: /sos_frame: 2000 x 1 pixels, 1 to 1024 a side/ },
    't-c-fr-flags': { title: 'App error', error: /sos_frame: unknown flags 0x80/ },
    't-c-fr-big': { title: 'App error', error: /files_read: 300000 bytes, at most 262144 per read/ },
    't-c-fr-buffer': { title: 'App error', error: /files_read: the pointer 0x7ffff000 \(2048 bytes\) is outside the app's memory/ },
    't-c-fr-name': { title: 'App error', error: /files_read: the pointer 0x7ffffff0 \(32 bytes\) is outside the app's memory/ },
    't-c-fr-noperm': { title: 'App error', error: /files_read: add the "files" permission to app\.json/ },
    't-c-fr-huge': { title: 'App error', error: /sos_frame: the pointer 0x[0-9a-f]+ \(2097152 bytes\) is outside the app's memory/ },
    // A bad name pointer stops the app, whatever the length to read (it used to restart the badge with 0).
    't-c-fr-name0': { title: 'App error', error: /files_read: the pointer 0x7ffffff0 \(32 bytes\) is outside the app's memory/ },
    't-c-fr-nameread': { title: 'App error', error: /files_read: the pointer 0x7ffffff0 \(8 bytes\) is outside the app's memory/ },
    't-c-fr-namestraddle': { title: 'App error', error: /files_read: the pointer 0x[0-9a-f]+ \(16 bytes\) is outside the app's memory/ },
    // No inventory server here: the walk stops at the first answer's error, and the app keeps running
    // (server/test/invemu.test.ts walks it to the end against a real server).
    't-c-inv': { noError: true, print: [/\] t-c-inv items error: /] },
    't-c-abi': { title: 'App error', error: /needs a newer Solana OS \(its C ABI is 99, this badge's 1\)/ },
    't-c-f-loop': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 3000 },
    't-c-f-loopcb': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 3000 },
    't-c-f-shared': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 3000 },
    't-c-f-unshared': { noError: true, print: [/still running/], wait: 3000 },
    't-c-f-nest': { print: [/nesting stopped at depth \d+|callbacks nested too deeply/], wait: 3000 },
    't-c-f-trapcb': { title: 'App error', error: /unreachable/ },
    't-c-f-grow': { noError: true, print: [/memory.grow refused after pages: \d+/, /still running/], wait: 4000 },
    't-c-f-luamem': { title: 'Out of memory', error: /The app needs more than \d+ (MB|KB)/, wait: 5000 },
    't-c-f-index': { title: 'App error', error: /sos_to_int: index 5 is outside the frame \(1 values\)/ },
    't-c-f-pointer': { title: 'App error', error: /sos_push_str: the pointer 0x7ffffff0 \(100 bytes\) is outside/ },
    't-c-f-oob': { title: 'App error', error: /outside its memory \(out of bounds memory access\)/ },
    't-c-f-unreachable': { title: 'App error', error: /\(unreachable\)/ },
    't-c-f-div0': { title: 'App error', error: /divided by zero/ },
    't-c-f-stack': { title: 'App error', error: /ran out of stack/ },
    't-c-f-indirect': { title: 'App error', error: /function pointer of the wrong type/ },
    't-c-f-fail': { title: 'App error', error: /Custom stop from the app/ },
    't-c-f-exit': { noError: true, print: [/\] start$/] },
    't-c-f-pushfn': { title: 'App error', error: /sos_push_fn: no function there \(function 12345\)/ },
    't-c-f-frame': { title: 'App error', error: /more than 64 values in a frame/ },
    't-c-f-unref': { title: 'App error', error: /sos_unref: 77 is not a kept value/ },
    't-c-f-nres': { title: 'App error', error: /sos_call: 2147483647 results don't fit in a frame/ },
    't-c-f-nullfn': { title: 'App error', error: /bad function pointer|wrong type/ },
    't-c-f-fspin': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 5000 },
    // Every import costs a host step: a loop of imports meets that budget first.
    't-c-f-importloop': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000 },
    // Work charged by length (t-c-limits): bulk memory in wasm instructions,
    // what imports copy, write or fill in host steps.
    't-c-l-fill': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 5000 },
    't-c-l-copy': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 5000 },
    't-c-l-init': { title: 'App took too long', error: /Over 2000000 wasm instructions in one callback/, wait: 5000 },
    't-c-l-pushstr': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000 },
    't-c-l-getkey': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000 },
    't-c-l-fdwrite': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000,
        print: [/\] fd_write rc 0$/, /\] fd_write took bytes: 1024$/] },
    't-c-l-random': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000,
        print: [/\] random_get of 1 MB: 28$/] },
    't-c-l-log': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000 },
    't-c-l-print': { title: 'App took too long', error: /Over 200000 host steps in one callback/, wait: 5000 },
    // Modules the badge refuses before instantiating them (WAMR would run
    // these with no instruction limit), and native code named app.wasm.
    't-c-l-start': { title: 'App error', error: /The app has a start function, which the badge doesn't run \(rebuild it with the SDK\)/ },
    't-c-l-ctors': { title: 'App error', error: /The app exports __wasm_call_ctors, which the badge doesn't run \(rebuild it with the SDK\)/ },
    't-c-l-aotwasm': { title: 'App error', error: /app\.wasm is native code, not WebAssembly: only the store's signed app\.aot runs natively/ },
    // sdk/c/tests/host: every badge.h wrapper against this host, 92 checks, then "done".
    'host-probe': { noError: true, print: [/\] done$/], oks: 92, wait: 3000 },
    't-c-bench': { noError: true, print: [/bench: label text \d+ us a call/, /bench: burn \d+ ms/, /bench: done/], wait: 8000 },
};

// EXAMPLES=1 also runs the SDK's examples as built in sdk/c/examples/build/
// (each must start and run without an error; ping and sol-price only get
// offline answers here).
const sources: Record<string, { json: string; wasm: string }> = {};
for (const id of readdirSync(outDir)) {
    sources[id] = { json: join(outDir, id, 'app.json'), wasm: join(outDir, id, 'app.wasm') };
}
const exDir = join(here, '../../../../sdk/c/examples');
const examples: string[] = [];
if (process.env.EXAMPLES) {
    for (const name of readdirSync(join(exDir, 'build')).filter((f) => f.endsWith('.wasm'))) {
        const dir = join(exDir, name.replace(/\.wasm$/, ''));
        try {
            const id = JSON.parse(readFileSync(join(dir, 'app.json'), 'utf8')).id;
            sources[id] = { json: join(dir, 'app.json'), wasm: join(exDir, 'build', name) };
            examples.push(id);
        } catch {
            // a build without its app folder
        }
    }
}
const ids = process.argv.slice(2).length ? process.argv.slice(2) : [...readdirSync(outDir).sort(), ...examples];
let failures = 0;
for (const id of ids) {
    const want = expect[id] ?? { noError: true, keys: examples.includes(id) ? ['select', 'right'] : [] };
    mod.FS.mkdirTree(`/sdcard/hh/apps/${id}`);
    mod.FS.writeFile(`/sdcard/hh/apps/${id}/app.json`, readFileSync(sources[id].json));
    mod.FS.writeFile(`/sdcard/hh/apps/${id}/app.wasm`, readFileSync(sources[id].wasm));
    if (id.startsWith('t-c-fr') || id === 't-c-frame') {
        // files_read's tests read data.bin from the app's folder (LUA_API.md "files").
        mod.FS.mkdirTree(`/sdcard/hh/files/${id}`);
        mod.FS.writeFile(`/sdcard/hh/files/${id}/data.bin`, Uint8Array.from({ length: 1000 }, (_, i) => i % 251));
    }
    const mark = lines.length;
    cmd(`lua run ${id}`);
    await sleep(want.wait ?? 1500);
    for (const k of want.keys ?? []) {
        cmd(`key ${k}`);
        await sleep(600);
    }
    const app = current();
    cmd('open home');
    await sleep(800);
    cmd('lua mem');
    await sleep(300);
    const out = lines.slice(mark);
    const mine = process.env.VERBOSE ? out : out.filter((l) => l.startsWith(`[${id}] `) || l.startsWith(`[lua:${id}]`)
        || l.startsWith('lua_wasm') || /^(running|last run|  )/.test(l) || l.includes('lua: '));
    console.log(`\n=== ${id} (app on screen before closing: ${app})`);
    for (const l of mine) {
        console.log('  ' + l);
    }
    const problems: string[] = [];
    const errLine = out.find((l) => l.startsWith(`[lua:${id}] `));
    if (want.noError && errLine) {
        problems.push('unexpected error: ' + errLine);
    }
    if (want.error) {
        if (!errLine) {
            problems.push('no error line');
        } else {
            if (want.title && !errLine.startsWith(`[lua:${id}] ${want.title}: `)) {
                problems.push(`title isn't "${want.title}"`);
            }
            if (!want.error.test(errLine)) {
                problems.push(`message doesn't match ${want.error}`);
            }
        }
    }
    for (const re of want.print ?? []) {
        if (!out.some((l) => re.test(l))) {
            problems.push(`missing ${re}`);
        }
    }
    const fails = out.filter((l) => l.startsWith(`[${id}] FAIL`));
    problems.push(...fails);
    if (out.some((l) => /must not|pcall came back/.test(l))) {
        problems.push('the app went on after it should have stopped');
    }
    const left = out.find((l) => l.startsWith('last run'))?.match(/(\d+) left after close/);
    if (left && left[1] !== '0') {
        problems.push(`${left[1]} bytes of the app's heap left after close`);
    }
    const oks = out.filter((l) => l.startsWith(`[${id}] ok `)).length;
    if (want.oks !== undefined && oks !== want.oks) {
        problems.push(`${oks} checks passed, not ${want.oks}`);
    }
    console.log(problems.length ? `  -> FAIL (${problems.join('; ')})` : `  -> pass${oks ? ` (${oks} checks)` : ''}`);
    failures += problems.length ? 1 : 0;
    rmTree(`/sdcard/hh/apps/${id}`);    // a card of a real size: lua list shows 40 apps at most
}
// The console: lua put / sums / list / run / rm with a C app, and app.aot
// (native code) that only the store installer may write.
{
    console.log('\n=== console');
    const id = 't-c-con';
    const exists = (f: string) => mod.FS.analyzePath(`/sdcard/hh/apps/${id}/${f}`).exists;
    const put = async (file: string, data: Uint8Array) => {
        cmd(`lua put ${id} ${file} begin`);
        for (let i = 0; i < data.length; i += 96) {
            cmd(`lua put ${id} ${file} ${Buffer.from(data.subarray(i, i + 96)).toString('base64')}`);
        }
        cmd(`lua put ${id} ${file} end`);
        await sleep(100);
    };
    const problems: string[] = [];
    const say = async (c: string, ms = 300) => {
        const m = lines.length;
        cmd(c);
        await sleep(ms);
        const got = lines.slice(m);
        console.log(`  > ${c}`);
        for (const l of got.filter((l) => !/^[IWD] \(/.test(l))) {
            console.log('    ' + l);
        }
        return got;
    };
    mod.FS.mkdirTree(`/sdcard/hh/apps/${id}`);
    mod.FS.writeFile(`/sdcard/hh/apps/${id}/app.aot`, 'not really native code');
    const json = `{"id":"${id}","name":"Console C","version":"1","author":"x","lang":"c","permissions":[]}`;
    const wasm = readFileSync(join(outDir, 't-c-f-exit', 'app.wasm'));
    await put('app.json', new TextEncoder().encode(json));
    if (exists('app.aot')) {
        problems.push('app.aot survived a console write');
    }
    mod.FS.writeFile(`/sdcard/hh/apps/${id}/app.aot`, 'not really native code');
    await put('app.wasm', new Uint8Array(wasm));
    if (exists('app.aot')) {
        problems.push('app.aot survived writing app.wasm');
    }
    if ((await say(`lua put ${id} app.aot begin`)).every((l) => !l.startsWith('usage:'))) {
        problems.push('lua put accepted app.aot');
    }
    const sha = new Bun.CryptoHasher('sha256').update(wasm).digest('hex');
    if (!(await say(`lua sums ${id}`)).some((l) => l === `${sha} ${wasm.length} app.wasm`)) {
        problems.push('lua sums lacks app.wasm');
    }
    if (!(await say('lua list')).some((l) => l.startsWith(id) && /  C$/.test(l))) {
        problems.push('lua list doesn\'t mark it C');
    }
    if (!(await say(`lua run ${id}`, 1500)).some((l) => l === `[${id}] start`)) {
        problems.push('it didn\'t run');
    }
    await say('open home', 500);
    await say(`lua rm ${id} app.wasm`);
    if (!(await say(`lua run ${id}`, 500)).some((l) => l.includes('That app isn\'t installed'))) {
        problems.push('ran without app.wasm');
    }
    await say(`lua rm ${id}`);
    console.log(problems.length ? `  -> FAIL (${problems.join('; ')})` : '  -> pass');
    failures += problems.length ? 1 : 0;
}

// The store installer: a C app's empty source and app.wasm, checked against
// the manifest; an AOT for this badge's target isn't fetched while the AOT
// seam says no; a second install keeps the unchanged app.wasm.
{
    console.log('\n=== store install');
    const problems: string[] = [];
    const wasm = new Uint8Array(readFileSync(join(outDir, 't-c-f-exit', 'app.wasm')));
    const sha = new Bun.CryptoHasher('sha256').update(wasm).digest('hex');
    const target = readFileSync(join(here, '../../../../sdk/c/toolchain/versions'), 'utf8').match(/^AOT_TARGET=(.*)$/m)![1];
    store.wasm = wasm;
    store.json = {
        id: store.id, name: 'Store C', version: '1.0.0', author: 'x', lang: 'c', permissions: [],
        code: { wasm: { sha256: sha, size: wasm.length }, aot: { target, sha256: sha, size: wasm.length } },
    };
    cmd('wifi connect "Emulator Guest"');
    for (let i = 0; i < 100 && !lines.some((l) => /hello ok|registered|eTEST1/.test(l)); i++) {
        await sleep(100);
    }
    await sleep(1000);
    const install = async () => {
        const m = lines.length;
        cmd(`lua install ${store.id}`);
        for (let i = 0; i < 100 && !lines.slice(m).some((l) => /lua_install: t-c-store (1\.0\.0|.*:)/.test(l)); i++) {
            await sleep(100);
        }
        const got = lines.slice(m).filter((l) => l.includes('lua_install') || l.includes('installing'));
        for (const l of got) {
            console.log('    ' + l);
        }
        return got;
    };
    const first = await install();
    const dir = `/sdcard/hh/apps/${store.id}`;
    const has = (f: string) => mod.FS.analyzePath(`${dir}/${f}`).exists;
    if (!has('app.wasm') || has('main.lua') || has('app.aot')) {
        problems.push(`files after install: app.wasm ${has('app.wasm')}, main.lua ${has('main.lua')}, app.aot ${has('app.aot')}`);
    }
    if (store.asked.some((a) => a.includes('f=aot'))) {
        problems.push('asked for app.aot while AOT is not usable');
    }
    if (!store.asked.some((a) => a === `GET /api/device/apps/${store.id}/code?v=1.0.0&f=wasm`)) {
        problems.push('never asked for code?v=1.0.0&f=wasm');
    }
    if (!first.some((l) => /app\.wasm, \d+ bytes$/.test(l))) {
        problems.push('no download line');
    }
    const second = await install();
    if (!second.some((l) => /app\.wasm, \d+ bytes \(kept\)/.test(l))) {
        problems.push('a second install downloaded app.wasm again');
    }
    const m = lines.length;
    cmd(`lua run ${store.id}`);
    await sleep(1500);
    if (!lines.slice(m).some((l) => l === `[${store.id}] start`)) {
        problems.push('the installed app didn\'t run');
    }
    // A wrong SHA-256: refused, the installed version stays.
    store.json = { ...store.json, version: '1.0.1', code: { wasm: { sha256: '0'.repeat(64), size: wasm.length } } };
    const bad = await install();
    if (!bad.some((l) => l.includes("didn't download right"))) {
        problems.push('a wrong SHA-256 was accepted');
    }
    if (!has('app.wasm')) {
        problems.push('the failed update removed the installed app');
    }
    console.log('    asked: ' + store.asked.filter((a) => a.includes('/apps/')).join(', '));
    console.log(problems.length ? `  -> FAIL (${problems.join('; ')})` : '  -> pass');
    failures += problems.length ? 1 : 0;
}

// The badge still works after all that: a Lua app runs.
mod.FS.mkdirTree('/sdcard/hh/apps/t-lua-after');
mod.FS.writeFile('/sdcard/hh/apps/t-lua-after/app.json',
    '{"id":"t-lua-after","name":"Lua after","version":"1","author":"x","permissions":[]}');
mod.FS.writeFile('/sdcard/hh/apps/t-lua-after/main.lua', 'print("lua still runs", sys.memory().used > 0)');
const mark = lines.length;
cmd('lua run t-lua-after');
await sleep(1000);
const luaOk = lines.slice(mark).some((l) => l === '[t-lua-after] lua still runs\ttrue');
console.log(`\n=== a Lua app afterwards: ${luaOk ? 'pass' : 'FAIL'}`);
failures += luaOk ? 0 : 1;
console.log(`\n${ids.length + 3 - failures} of ${ids.length + 3} passed`);
mod.ccall('emu_halt', null, [], []);
fake.stop(true);
process.exit(failures ? 1 : 0);
