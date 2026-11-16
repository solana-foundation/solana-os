// Runs the background-service regression apps (t-svc*, and tests/c's t-c-svc
// when build.sh built it) in the emulator, headless, and checks what they
// print and what `svc` lists:
//   bun firmware/main/lua/tests/svc-run.ts
// Loads web/static/emulator/solana-os.js (bun run emulator) with the browser
// bits stubbed, like tests/c/run.ts. No server: server calls answer offline.
// The emulator's card is in memory, so a restart (services coming back at
// boot) is checked on a badge instead. Exit code 1 when anything is off.
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
const svcList = async () => {
    const mark = lines.length;
    cmd('svc');
    await waitFor(/services, \d+ running/, 3000, mark);
    return since(mark);
};
const svcLine = async (id: string) => (await svcList()).find((l) => l.startsWith(`app:${id} `)) ?? '';

for (const id of ['t-svc', 't-svc-a', 't-svc-b', 't-svc-limits', 't-svc-mesh', 't-svc-store', 't-svc-ref', 't-svc-rec', 't-svc-final']) {
    put(id, join(here, id));
}
// A badge that restarted while these two services were starting (a crash in their
// start): the boot ~10 s in must skip both, and turn the one on its third such
// restart off (lua_svc.c boot_next). Written before that boot runs.
put('t-svc-boot', join(here, 't-svc-boot'));
put('t-svc-boot2', join(here, 't-svc-boot'));
mod.FS.writeFile('/sdcard/hh/apps/t-svc-boot2/app.json', readFileSync(join(here, 't-svc-boot/app.json'), 'utf8').replace('"t-svc-boot"', '"t-svc-boot2"'));
mod.FS.writeFile('/sdcard/hh/services.json', JSON.stringify({
    't-svc-boot': { allowed: true, boot: true, fails: 0, starting: true, ver: '1.0.0' },
    't-svc-boot2': { allowed: true, boot: true, fails: 2, starting: true, ver: '1.0.0' },
}));
const bootMark = lines.length;
const cOut = join(here, 'c/out/t-c-svc');
const haveC = existsSync(join(cOut, 'app.wasm'));
if (haveC) put('t-c-svc', cOut);

// Open an app whose service.start asks the owner: SELECT on the badge's own dialog.
const openAndAllow = async (id: string, re: RegExp) => {
    const mark = lines.length;
    cmd(`lua run ${id}`);
    await sleep(1200);
    cmd('key select');                      // the OS's confirm: only a key the owner presses answers it
    return waitFor(re, 6000, mark);
};

// 1. Lifecycle, sandbox, messages, storage
let mark = lines.length;
const started = await openAndAllow('t-svc', /t-svc start: true/);
ok(!!started, 't-svc: service.start answered true after the owner allowed it');
ok(!!(await waitFor(/t-svc:svc forbidden: all nil/, 3000, mark)), 'service: ui, led, wifi, ble, ws, quest, spending, channel, sys.tick, mail.send, file writes are nil');
ok(!!(await waitFor(/t-svc:svc allowed: all there/, 3000, mark)), 'service: the allowed modules are there');
ok(!!(await waitFor(/t-svc second start: .*still waiting/, 3000, mark)), 'app: a second start while one waits is refused');
ok(!!(await waitFor(/t-svc big send: message over 1 KB/, 3000, mark)), 'app: service.send over 1 KB refused');
ok(!!(await waitFor(/t-svc:svc timers: .*too many timers \(4\)/, 3000, mark)), 'service: 4 timers at most');
const t = await waitFor(/t-svc:svc timer after (\d+) ms/, 4000, mark);
ok(!!t && Number(t!.match(/after (\d+) ms/)![1]) >= 990, `service: timers no faster than a second (${t})`);
mark = lines.length;
cmd('key select');
ok(!!(await waitFor(/t-svc from service: .*"echo":1/, 4000, mark)), 'app -> service -> app message round trip');
ok(!!(await waitFor(/flood queued [1-8] of 20/, 2000, mark)), 'app: a flood of service.send stops at the 8 waiting');
ok(!!(await waitFor(/t-svc:svc notify: true/, 6000)), 'service: a notice');
ok(!!(await waitFor(/t-svc:svc notify again: false/, 2000)), 'service: a second notice within 10 s is dropped');
await sleep(2500);
mark = lines.length;
cmd('key right');
const st = await waitFor(/t-svc storage: svc_ticks=(\d+)/, 2000, mark);
ok(!!st && Number(st!.match(/svc_ticks=(\d+)/)![1]) >= 3, `storage shared: the app reads the service's writes at once (${st})`);
mark = lines.length;
cmd('open home');
await sleep(2500);
let line = await svcLine('t-svc');
ok(/ on /.test(line) && /ticks \d+, app said hello/.test(line), `svc: still on with the app closed, the status the service set (${line})`);

// 2. Three at once, the fourth refused
await openAndAllow('t-svc-a', /t-svc-a start: true/);
cmd('open home');
await openAndAllow('t-svc-b', /t-svc-b start: true/);
cmd('open home');
mark = lines.length;
cmd('lua run t-svc-mesh');
await sleep(1200);
cmd('key right');                           // t-svc-mesh's service.start: refused before asking anyone
ok(!!(await waitFor(/t-svc-mesh start: false full/, 6000, mark)), 'a fourth service is refused: "full" (no dialog)');
cmd('open home');
const list = await svcList();
ok(list.filter((l) => /^app:\S+ +on /.test(l)).length === 3, `svc: three app services on\n${list.join('\n')}`);
cmd('heap');
await sleep(300);

// 3. Stops: the owner, the app
mark = lines.length;
cmd('svc stop app:t-svc-a');
await sleep(1500);
line = await svcLine('t-svc-a');
ok(/ off /.test(line) && / - /.test(line), `owner's stop: off, and no longer starts at boot (${line})`);
mark = lines.length;
cmd('lua run t-svc');
await sleep(1200);
cmd('key left');
ok(!!(await waitFor(/t-svc:svc on_stop after/, 4000, mark)), "app's service.stop: on_stop ran");
cmd('open home');
cmd('svc stop app:t-svc-b');
await sleep(1500);

// 4. Hostile services: each ends with its reason; the app on screen keeps running
const hostile: [string, string, RegExp][] = [
    ['up', 'loop', /Failed: service\.lua:\d+: Service took too long/],
    ['down', 'alloc', /Failed: The service needs more than 1 MB/],
    ['left', 'pattern', /Failed: service\.lua:\d+: Service took too long/],
];
await openAndAllow('t-svc-limits', /t-svc-limits spam: start true/);
await sleep(2500);
line = await svcLine('t-svc-limits');
ok(/spammed [0-8]\b/.test(line), `spam: a service's sends to the app stop at the app's queue (${line})`);
for (const [key, mode, re] of hostile) {
    mark = lines.length;
    cmd(`key ${key}`);
    await waitFor(new RegExp(`${mode}: start true`), 6000, mark);
    const t0 = Date.now();
    await waitFor(/Failed: /, 8000, mark);
    line = await svcLine('t-svc-limits');
    ok(re.test(line) && /failed/.test(line), `${mode}: ${line} (${Date.now() - t0} ms)`);
}
mark = lines.length;
cmd('key select');                          // stopper: on_stop never returns
await waitFor(/stopper: start true/, 6000, mark);
const t1 = Date.now();
cmd('svc stop app:t-svc-limits');
await waitFor(/t-svc-limits:svc on_stop: looping forever/, 3000, mark);
for (let i = 0; i < 40 && / stopping /.test(await svcLine('t-svc-limits')); i++) await sleep(100);
line = await svcLine('t-svc-limits');
ok(/ off /.test(line), `stopper: on_stop that never returns still ends (${Date.now() - t1} ms): ${line}`);
const alive = since(mark).some((l) => /lua:t-svc-limits/.test(l));
ok(!alive, 'the app on screen never failed meanwhile');
cmd('open home');

// Work in C that VM instructions don't count: the second per callback stops it.
mod.FS.writeFile('/sdcard/hh/apps/t-svc-limits/data.json', '{"mode":"rep"}');
mark = lines.length;
await openAndAllow('t-svc-limits', /rep: start true/);   // the owner stopped it above: it asks again
const t2 = Date.now();
await waitFor(/Failed: /, 8000, mark);
line = await svcLine('t-svc-limits');
ok(/Service took too long/.test(line), `rep: ${line} (${Date.now() - t2} ms)`);
cmd('open home');

if (process.argv.includes('--minute')) {
    await openAndAllow('t-svc-limits', /start true/);
    mark = lines.length;
    cmd('key right');
    await waitFor(/Failed: /, 70000, mark);
    line = await svcLine('t-svc-limits');
    ok(/used too much of the processor/.test(line), `minute: ${line}`);
    cmd('open home');
}

// 5. A C app's service
if (haveC) {
    mark = lines.length;
    const cs = await openAndAllow('t-c-svc', /ok t-c-svc service.start answered true/);
    ok(!!cs, 'C: service_start answered true while the app holds the runtime');
    line = await svcLine('t-c-svc');
    ok(/Starts when the C app closes/.test(line), `C: waits for the app to close (${line})`);
    cmd('open home');
    ok(!!(await waitFor(/t-c-svc svc: started/, 5000, mark)), 'C: the service instance started at on_service_start once the app closed');
    ok(!!(await waitFor(/ok t-c-svc svc: the screen, spending and the radio's channel are nil/, 3000, mark)), 'C: forbidden modules are nil');
    const callErr = since(mark).find((l) => /\[t-c-svc\] (?!ok |FAIL )/.test(l) && /nil/.test(l)) ?? '';
    ok(!!(await waitFor(/ok t-c-svc svc: calling lamps.spend fails/, 3000, mark)) && /attempt to call a nil value/.test(callErr), `C: calling a missing module fails as a call of nothing (${callErr})`);
    // Buttons held while the service ticks (at the launcher): it must read none, draw nothing.
    mod.ccall('emu_buttons_set', null, ['number'], [0x3f]);
    await sleep(2500);
    mod.ccall('emu_buttons_set', null, ['number'], [0]);
    ok(!since(mark).some((l) => /t-c-svc svc: FAIL/.test(l)), 'C: a service reads no buttons (sos_keys 0) and draws no frame (-1)');
    line = await svcLine('t-c-svc');
    ok(/ on /.test(line) && /C ticks \d+/.test(line), `C: running with the app closed (${line})`);
    // A C app opened while a C service runs: the service pauses, then comes back.
    mark = lines.length;
    cmd('lua run t-c-svc');
    await waitFor(/Paused: a C app is open|t-c-svc app:/, 5000, mark);
    await sleep(1500);
    line = await svcLine('t-c-svc');
    ok(/Paused: a C app is open|Starts when the C app closes/.test(line), `C: paused while a C app holds the runtime (${line})`);
    cmd('open home');
    await sleep(3000);
    line = await svcLine('t-c-svc');
    ok(/ on /.test(line), `C: resumed after the C app closed (${line})`);
    cmd('svc stop app:t-c-svc');
    await sleep(1500);
    ok(!!since(mark).find((l) => /t-c-svc svc: on_stop after ticks/.test(l)), 'C: on_stop ran');
} else {
    console.log('skip C (bash firmware/main/lua/tests/c/build.sh first)');
}

// 6. Removal forgets it
// The app and its service writing different keys at once: none lost.
{
    const m0 = lines.length;
    await openAndAllow('t-svc-store', /t-svc-store start: true/);
    const res = await waitFor(/t-svc-store a=200 s=200/, 30000, m0);
    const last = since(m0).filter((l) => /t-svc-store a=/.test(l)).at(-1) ?? '';
    ok(!!res, `storage: app and service each wrote 200 keys, all kept (${last})`);
    cmd('open home');
    await sleep(500);
    cmd('svc stop app:t-svc-store');
    await sleep(500);
}

// The boot's crash bookkeeping (set up at the start).
// Storage references: the app holds its shared copy while its service fails before it
// loaded storage (service.lua doesn't compile). The failed state must release nothing
// of the app's: the app's set() after that reaches data.json.
{
    const m0 = lines.length;
    const done = await openAndAllow('t-svc-ref', /t-svc-ref b set/);
    ok(!!done && since(m0).some((l) => /t-svc-ref start: false/.test(l)), 'storage refs: the service failed to load, the app set b after');
    cmd('open home');
    await sleep(1500);
    let data: any = {};
    try { data = JSON.parse(new TextDecoder().decode(mod.FS.readFile('/sdcard/hh/apps/t-svc-ref/data.json'))); } catch { /* none */ }
    ok(data.a === 1 && data.b === 2, `storage refs: a failed service leaves the app's storage working (${JSON.stringify(data)})`);
    cmd('lua rm t-svc-ref');
    await sleep(500);
}

// Last saves: each round the service pads storage and stops itself (its last save)
// while the app sets "r" and exits; the next launch reads both back. The emulator's
// tasks take turns, so the two last saves never overlap here: this checks the path
// and the bookkeeping. The overlap itself is a badge test (t-svc-final/main.lua,
// driven a few hundred rounds with the timing swept; see its header).
{
    let m0 = lines.length;
    await openAndAllow('t-svc-final', /t-svc-final round n=1 /);
    await waitFor(/t-svc-final: ended/, 4000, m0);
    let okAll = true;
    let last = '';
    for (let i = 0; i < 4; i++) {
        await sleep(800);
        m0 = lines.length;
        cmd('lua run t-svc-final');
        const c = await waitFor(/t-svc-final check n=(\d+) r=(\d+) s=(\d+)/, 4000, m0);
        last = c ?? 'no check';
        // The app's key every round; the service's after an even round (its last save
        // overlapped the app's): both shutdown orders, as the badge procedure checks.
        const m = c?.match(/n=(\d+) r=(\d+) s=(\d+)/);
        if (!m || m[1] !== m[2] || (Number(m[1]) % 2 === 0 && m[3] !== m[1])) okAll = false;
        await waitFor(/t-svc-final: ended/, 4000, m0);
    }
    ok(okAll, `last saves: the app's last key every round and the service's after even rounds are on the card (${last})`);
    cmd('open home');
    await sleep(800);
    cmd('lua rm t-svc-final');
    await sleep(500);
}

// Start records: with services.json unwritable (a directory where its .tmp goes),
// the app's own service.start runs the service (true); a start of a running one
// stops nothing; a start nobody on screen asked for (here `svc start`; boot and
// resumes take the same path) is refused with "card error" and never runs.
{
    mkdirp('/sdcard/hh/services.json.tmp');
    let m0 = lines.length;
    const st = await openAndAllow('t-svc-rec', /t-svc-rec start:/);
    ok(/t-svc-rec start: true/.test(st ?? ''), `start record: the app's own start runs without a record (${st})`);
    ok(!!(await waitFor(/\[t-svc-rec:svc\] up/, 3000, m0)), 'start record: its code ran');
    cmd('open home');
    await sleep(800);
    m0 = lines.length;
    cmd('svc start app:t-svc-rec');
    await sleep(1500);
    ok((await svcLine('t-svc-rec')).includes(' on '), `start record: starting a running service stops nothing (${await svcLine('t-svc-rec')})`);
    cmd('svc stop app:t-svc-rec');
    await waitFor(/t-svc-rec: ended/, 4000, m0);
    m0 = lines.length;
    cmd('svc start app:t-svc-rec');
    const ans = await waitFor(/svc: start app:t-svc-rec/, 3000, m0);
    await sleep(1500);
    ok(/can't record it/.test(ans ?? ''), `start record: an unasked start is refused, with why (${ans})`);
    ok(!since(m0).some((l) => /\[t-svc-rec:svc\] up/.test(l)), 'start record: its code never ran');
    ok((await svcLine('t-svc-rec')).includes('Not started: the card'), `start record: Services says why (${await svcLine('t-svc-rec')})`);
    mod.FS.rmdir('/sdcard/hh/services.json.tmp');
    m0 = lines.length;
    cmd('svc start app:t-svc-rec');
    ok(!!(await waitFor(/\[t-svc-rec:svc\] up/, 4000, m0)), 'start record: starts once the card can record it');
    const sj2 = JSON.parse(new TextDecoder().decode(mod.FS.readFile('/sdcard/hh/services.json')));
    ok(!!sj2['t-svc-rec'], 'start record: and it is in services.json');
    cmd('svc stop app:t-svc-rec');
    await waitFor(/t-svc-rec: ended/, 4000, m0);
    cmd('lua rm t-svc-rec');
    await sleep(500);
}

ok(!since(bootMark).some((l) => /t-svc-boot2?:svc started/.test(l)), 'boot: services whose last start never finished are not started');
ok((await svcLine('t-svc-boot')).includes('Skipped: the badge restarted as it started'), `boot: skipped once (${await svcLine('t-svc-boot')})`);
ok((await svcLine('t-svc-boot2')).includes('Off: it kept restarting the badge'), `boot: off after the third (${await svcLine('t-svc-boot2')})`);
const sj = JSON.parse(new TextDecoder().decode(mod.FS.readFile('/sdcard/hh/services.json')));
ok(sj['t-svc-boot'].fails === 1 && !sj['t-svc-boot'].starting && sj['t-svc-boot'].boot === true, `services.json: one more failure, no attempt pending (${JSON.stringify(sj['t-svc-boot'])})`);
ok(sj['t-svc-boot2'].boot === false && sj['t-svc-boot2'].why === 'Off: it kept restarting the badge', 'services.json: starting at boot off, and why (shown after a restart too)');

cmd('svc stop app:t-svc');
await sleep(1500);
cmd('lua rm t-svc');
await sleep(500);
line = await svcLine('t-svc');
ok(line === '', 'lua rm: the service is forgotten');

if (process.argv.includes('--log')) console.log(lines.join('\n'));
console.log(failures ? `${failures} FAILED` : 'all ok');
process.exit(failures ? 1 : 0);
