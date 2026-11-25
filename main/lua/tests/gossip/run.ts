// Gossip end to end on two emulated badges against the fake server (fake.ts),
// headless, on the emulator's radio (a BroadcastChannel shared by both):
//   bun firmware/main/lua/tests/gossip/run.ts
// Loads web/static/emulator/solana-os.js (bun run emulator). Badge A runs the
// test app as the Lab (`gossip-lab`), picks a class and deploys a program; its
// HELLO then carries `st` and `g`. Badge B hears it, gets exposed, syncs the
// record, is adopted, receives the signed record and checks it. Then the reads
// on B (t-gossip, not the Lab: its acting calls say "lab only"), the radar, the
// service's switch, and A going quiet when gossip is off. Exit code 1 when
// anything is off. PORT (default 3211) for the fake.
import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomBytes } from 'node:crypto';
import { base58, startFake } from './fake';

const here = import.meta.dir;
const emuDir = process.env.EMU_DIR ?? join(here, '../../../../../web/static/emulator');
const fake = startFake(Number(process.env.PORT ?? 3211), { thr: 60, next: 4000, coolOnce: true });
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

type Emu = {
	name: string;
	lines: string[];
	mod: any;
	cmd: (l: string) => void;
	since: (mark: number) => string[];
	waitFor: (re: RegExp, ms: number, mark?: number) => Promise<string | undefined>;
	put: (id: string, dir: string, as?: string) => void;
};

async function boot(name: string): Promise<Emu> {
	const lines: string[] = [];
	const nvs = new Map<string, string>();
	const url = new URL(fake.url);
	const loc = { origin: url.origin, href: `${url.origin}/emulator`, protocol: url.protocol, host: url.host, hostname: url.hostname, pathname: '/emulator', search: '' };
	const ls = {
		getItem: (k: string) => (nvs.has(k) ? nvs.get(k)! : null),
		setItem: (k: string, v: string) => void nvs.set(k, String(v)),
		removeItem: (k: string) => void nvs.delete(k),
		clear: () => nvs.clear(),
		key: (i: number) => [...nvs.keys()][i] ?? null,
		get length() { return nvs.size; },
	};
	const SolanaOS = new Function('window', 'location', 'localStorage', `${readFileSync(join(emuDir, 'solana-os.js'), 'utf8')}; return SolanaOS;`)(globalThis, loc, ls);
	const mod = await SolanaOS({
		locateFile: (p: string) => `file://${join(emuDir, p)}`,
		print: (l: string) => void lines.push(l),
		printErr: (l: string) => void lines.push(l),
		server: fake.url,
	});
	const since = (mark: number) => lines.slice(mark);
	const waitFor = async (re: RegExp, ms: number, mark = 0) => {
		for (let t = 0; t < ms; t += 50) {
			const hit = since(mark).find((l) => re.test(l));
			if (hit) return hit;
			await sleep(50);
		}
		return undefined;
	};
	const mkdirp = (p: string) => {
		let cur = '';
		for (const part of p.split('/').filter(Boolean)) {
			cur += '/' + part;
			try { mod.FS.mkdir(cur); } catch { /* exists */ }
		}
	};
	const put = (id: string, dir: string, as = id) => {
		const dest = `/sdcard/hh/apps/${as}`;
		mkdirp(dest);
		for (const f of readdirSync(dir)) {
			if (statSync(join(dir, f)).isFile()) {
				if (f.endsWith('.c')) continue;
				const raw = readFileSync(join(dir, f));
				mod.FS.writeFile(`${dest}/${f}`, f === 'app.json' ? raw.toString('utf8').replace(`"${id}"`, `"${as}"`) : raw);
			}
		}
	};
	const e: Emu = { name, lines, mod, cmd: (l) => mod.ccall('emu_console', null, ['string'], [l]), since, waitFor, put };
	if (!(await waitFor(/console ready/, 20000))) throw new Error(`${name}: no console`);
	return e;
}

let failures = 0;
const ok = (cond: boolean, what: string) => {
	console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`);
	if (!cond) failures++;
};
const status = async (e: Emu) => {
	const mark = e.lines.length;
	e.cmd('gossip status');
	await e.waitFor(/^ {2}heard:/, 3000, mark);
	return e.since(mark).join('\n');
};
const until = async (e: Emu, test: (s: string) => boolean, ms: number) => {
	for (let t = 0; t < ms; t += 1000) {
		const s = await status(e);
		if (test(s)) return s;
		await sleep(1000);
	}
	return status(e);
};

const a = await boot('A');
const b = await boot('B');
// F5: B's first answer with the server's key is lost (as when the OS queue stays full):
// it asks again after 30 s instead of waiting for it forever.
b.cmd('api lose /api/device/gossip/key');
for (const e of [a, b]) {
	e.cmd('wifi connect "Emulator Guest"');
}
for (const e of [a, b]) {
	const s = await until(e, (x) => /, claimed,/.test(x) && /clock yes/.test(x), 40000);
	ok(/, claimed,/.test(s) && /clock yes/.test(s), `${e.name}: claimed, synced, has the server's clock`);
}
const ids = [...fake.badges.values()].map((x) => x.id);
ok(ids.length === 2 && ids.every((id) => id.startsWith('e')), `two emulated badges registered: ${ids.join(', ')}`);

// 1. A acts as the Lab: a class, then its first program.
a.put('t-gossip', join(here, '../t-gossip'), 'gossip-lab');
let mark = a.lines.length;
a.cmd('lua run gossip-lab');
ok(!!(await a.waitFor(/t-gossip genome vector 3: ok/, 8000, mark)), 'A: gossip.genome decodes vector 3 (three traits)');
ok(!!(await a.waitFor(/t-gossip set_class: true/, 4000, mark)), 'A (the Lab): set_class queued');
ok(!!(await a.waitFor(/t-gossip deploy: true/, 4000, mark)), 'A (the Lab): deploy queued');
const res = await a.waitFor(/t-gossip ev: (?=.*"t":"result")(?=.*"act":"deploy")(?=.*"ok":true)/, 20000, mark);
ok(!!res, `A: the deploy's result came back as on_gossip ${res ? '' : '(none)'}`);
ok(!!(await a.waitFor(/t-gossip ev: .*"t":"node"/, 5000, mark)), 'A: on_gossip node after the sync');
ok(!!(await a.waitFor(/t-gossip ev: (?=.*"t":"event")(?=.*Deployed)/, 5000, mark)), 'A: on_gossip event (the server\'s ev)');
if (process.env.DEBUG) console.log(a.since(mark).filter((l) => /t-gossip|gossip|lua/.test(l)).join('\n'));
const prog = [...fake.records.keys()][0];
ok(!!prog, `the fake made program ${prog}`);
let sa = await until(a, (s) => s.includes(`"st":"${prog}"`), 15000);
const hello = /hello: (.*)/.exec(sa)?.[1] ?? '';
ok(new RegExp(`^"st":"${prog}","g":"r0\\.\\d+\\.[0-9a-f]{16}"$`).test(hello), `A's HELLO carries st and g: ${hello}`);
ok(hello.length + '{"k":"b","n":"Tester eXXXXX","lv":0,}'.length <= 232, `A's HELLO fits one fragment (~${hello.length + 37} bytes)`);
ok(/class r/.test(sa), 'A: class r from the node');
a.cmd('key home');

// 2. B hears A, is exposed, syncs, is adopted and checks the record.
mark = b.lines.length;
const sb = await until(b, (s) => new RegExp(`${prog} (exposed|incubating)`).test(s) || s.includes(`act ${prog}`), 60000);
ok(new RegExp(`${prog} (hearing|exposed|incubating)`).test(sb) || sb.includes(`act ${prog}`), 'B: hears A\'s program and gets exposed');
ok(!!(await b.waitFor(/gossip: exposed to/, 60000, 0)), 'B: the exposure logged');
const adopted = await until(b, (s) => s.includes(`act ${prog}`), 30000);
ok(adopted.includes(`act ${prog}`), 'B: the fake adopted it; B\'s node holds it');
const bsync = fake.syncs.find((x) => x.id !== fake.syncs[0]?.id && x.body.rec?.length);
const rec = fake.syncs.flatMap((x) => x.body.rec ?? []).find((r: any) => r.p === prog);
ok(!!rec && rec.f && /^r0\.\d+\.[0-9a-f]{16}$/.test(rec.g) && rec.s > 0 && rec.r < 0 && rec.t > 1.7e9, `B's exposure record: ${JSON.stringify(rec)}`);
void bsync;
const cool = fake.syncs.findIndex((x) => x.answer.res?.some((r: any) => r.r === 'cool'));
ok(cool >= 0 && fake.syncs.slice(cool + 1).some((x) => x.body.rec?.some((r: any) => r.p === prog && r.n === fake.syncs[cool].answer.res[0].n)),
	'B: a cooled record stays unacknowledged and goes again with a later sync');
let progs = '';
for (let t = 0; t < 50 && !progs.includes('verified'); t++) {
	mark = b.lines.length;
	b.cmd('gossip progs');
	await b.waitFor(/gossip progs:/, 3000, mark);
	await sleep(300);
	progs = b.since(mark).join('\n');
	if (!progs.includes(`${prog} verified`)) await sleep(1000);
}
ok(progs.includes(`${prog} verified`), 'B: the record\'s PDA and signature check (the checker task), after a lost key answer');
ok(b.lines.some((l) => /dropped the answer to GET \/api\/device\/gossip\/key/.test(l)), 'B: its first key answer was the one dropped');

// 3. The reads on B (not the Lab).
b.put('t-gossip', join(here, '../t-gossip'));
mark = b.lines.length;
b.cmd('lua run t-gossip');
const me = await b.waitFor(/t-gossip me: /, 8000, mark);
ok(!!me && me.includes(`"act":"${prog}"`) && me.includes('"on":true') && /"synced":\d{10}[,}]/.test(me), `B: gossip.me() ${me?.slice(0, 160)}`);
const pr = await b.waitFor(/t-gossip program: /, 4000, mark);
ok(!!pr && pr.includes('"verified":true') && pr.includes(`"a":"${prog}"`) && /"traits":\[/.test(pr) && /"nodes":2/.test(pr), `B: gossip.program(act) ${pr?.slice(0, 200)}`);
const ps = await b.waitFor(/t-gossip programs: /, 4000, mark);
ok(!!ps && ps.includes('"slots":[{') && ps.includes('false'), `B: gossip.programs() ${ps?.slice(0, 120)}`);
const nb = await b.waitFor(/t-gossip nearby: /, 4000, mark);
const aId = ids.find((id) => b.lines.join('\n').includes(id) && id !== /, claimed/.exec('')?.[0]) ?? '';
ok(!!nb && nb.includes(`"st":"${prog}"`) && nb.includes('"class":"r"') && nb.includes('"kind":"badge"'), `B: gossip.nearby() lists A ${nb?.slice(0, 200)}`);
ok(!!(await b.waitFor(/t-gossip set_class: nil lab only/, 4000, mark)), 'B: acting outside the Lab: nil, "lab only"');
void aId;
b.cmd('key home');

// 3b. The same through badge.h, when the C toolchain is there (bun run cc).
const cc = join(here, '../../../../tools/cc/badge-cc');
if (existsSync(join(here, '../../../../tools/cc/.toolchain'))) {
	const out = join(tmpdir(), `t-c-gossip-${process.pid}`);
	mkdirSync(out, { recursive: true });
	const r = Bun.spawnSync([cc, join(here, 't-c-gossip'), '-o', join(out, 'app.wasm'), '-q']);
	ok(r.exitCode === 0, `t-c-gossip builds ${r.stderr.toString().slice(0, 200)}`);
	copyFileSync(join(here, 't-c-gossip/app.json'), join(out, 'app.json'));
	await sleep(1500);
	mark = b.lines.length;
	b.put('t-c-gossip', out);
	b.cmd('lua run t-c-gossip');
	ok(!!(await b.waitFor(/t-c-gossip genome: ok colour f040ff body 3 traits 3:2,4,0 vir 5 rarity 1/, 15000, mark)), 'C: gossip_genome (vector 3)');
	ok(!!(await b.waitFor(/t-c-gossip bad genome: refused bad address/, 4000, mark)), 'C: gossip_genome refuses a bad address');
	ok(!!(await b.waitFor(new RegExp(`t-c-gossip me: id e\\w+ on 1 .* act ${prog} synced \\d{10}`), 4000, mark)), 'C: gossip_me');
	ok(!!(await b.waitFor(new RegExp(`t-c-gossip slot 0: ${prog} verified 1 nodes 2`), 4000, mark)), 'C: gossip_slot');
	ok(!!(await b.waitFor(new RegExp(`t-c-gossip program: ${prog} gen 0 verified 1`), 4000, mark)), 'C: gossip_program');
	ok(!!(await b.waitFor(new RegExp(`t-c-gossip nearby: e\\w+ badge class r rssi -\\d+ st ${prog}`), 4000, mark)), 'C: gossip_nearby');
	ok(!!(await b.waitFor(/t-c-gossip set_class: lab only/, 4000, mark)), 'C: acting outside the Lab: lab only');
	if (process.env.DEBUG) console.log(b.since(mark).filter((l) => /t-c-gossip|error|lua/.test(l)).join('\n'));
	b.cmd('key home');
} else {
	console.log('skip C wrappers: no toolchain (bun run cc)');
}

// 4. Off: A's HELLO loses its fields, one last sync says on: false.
const before = fake.syncs.length;
a.cmd('gossip off');
sa = await until(a, (s) => s.includes('(no gossip fields)'), 8000);
ok(sa.includes('(no gossip fields)') && /gossip: off \(Off\)/.test(sa), 'A off: no st/g in the HELLO, status Off');
await sleep(1500);
ok(fake.syncs.slice(before).some((x) => x.body.on === false), 'A off: a last sync with on: false');
mark = a.lines.length;
a.cmd('svc');
await a.waitFor(/services, \d+ running/, 3000, mark);
ok(a.since(mark).some((l) => /^gossip .*stopped|^gossip .*Off/i.test(l)), `Services lists gossip as stopped: ${a.since(mark).find((l) => l.startsWith('gossip')) ?? '(no line)'}`);
a.cmd('gossip on');
sa = await until(a, (s) => s.includes(`"st":"${prog}"`), 15000);
ok(sa.includes(`"st":"${prog}"`), 'A on again: st and g back in the HELLO');

// 5. F5: a lost sync answer (the OS queue stayed full) does not stop syncing.
{
	const bId = fake.syncs.find((x) => x.body.rec?.length)?.id ?? '';   // B sent the records; A never
	const n0 = fake.syncs.filter((x) => x.id === bId).length;
	b.cmd('api lose /api/device/gossip/sync');
	let n = n0;
	for (let t = 0; t < 55 && n < n0 + 2; t++) {
		await sleep(1000);
		n = fake.syncs.filter((x) => x.id === bId).length;
	}
	ok(n >= n0 + 2, `B: syncs again after a lost sync answer (${n - n0} since)`);
}

// 6. F3: a hostile badge cycling through eight made-up programs holds one hearing
// entry of B's, not all eight; another badge's program is still heard; once B's
// record from it comes back bad, B ignores it.
{
	const crock = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
	const mk = (p: string) => ({ id: p + Array.from(randomBytes(5), (x) => crock[x & 31]).join(''), mac: `0e${randomBytes(4).toString('hex')}00` });
	const X = mk('X'), Y = mk('Y');
	const air = new BroadcastChannel('solana-os-mesh');
	let m16 = 0;
	const send = (who: { id: string; mac: string }, json: string) => {
		const data = Buffer.from(json), h = Buffer.alloc(16);
		h.write('HG', 0); h[2] = 1; h[3] = 1; h.write(who.id, 4); h.writeUInt16LE(m16++ & 0xffff, 10); h[12] = 0; h[13] = 1; h.writeUInt16LE(data.length, 14);
		air.postMessage({ from: who.mac, to: 'ffffffffffff', ch: 1, data: Buffer.concat([h, data]).toString('base64') });
	};
	const hello = (who: { id: string; mac: string }, st: string) =>
		send(who, JSON.stringify({ k: 'b', n: who.id, lv: 1, st, g: `r9.${Math.floor(Date.now() / 30000)}.${randomBytes(8).toString('hex')}` }));
	const fakes = Array.from({ length: 8 }, () => base58(randomBytes(32)));
	const yProg = base58(randomBytes(32));
	// Hearing entries (one exposed and waiting for its answer is not one: §5 keeps it until then).
	const entries = async (who: string) => (await status(b)).split('\n').filter((l) => l.includes(` from ${who} `) && / hearing /.test(l)).length;
	// Any state: at the test's low threshold its one entry is exposed within a second or two.
	const anyFrom = async (who: string) => (await status(b)).split('\n').filter((l) => l.includes(` from ${who} `) && / (hearing|exposed|incubating) /.test(l)).length;
	let seenX = 0;
	const recsFrom = (who: string) => fake.syncs.flatMap((x) => x.body.rec ?? []).filter((r: any) => r.f === who).length;
	const t0 = Date.now();
	let maxX = 0, sawY = false, i = 0;
	// 12 s: X says hello five times a second, a new program each time; Y says hello every 2 s.
	while (Date.now() - t0 < 12_000) {
		hello(X, fakes[i++ % fakes.length]);
		if (i % 10 === 0) hello(Y, yProg);
		await sleep(200);
		if (i % 10 === 5) {
			maxX = Math.max(maxX, await entries(X.id));
			seenX = Math.max(seenX, await anyFrom(X.id));
			sawY ||= (await status(b)).split('\n').some((l) => l.includes(` from ${Y.id} `));   // any state: it is exposed in seconds here
		}
	}
	ok(maxX <= 1 && seenX >= 1, `F3: a hostile badge cycling 8 programs holds at most one of B's hearing entries (at most ${maxX} hearing, ${seenX} in all with one exposed awaiting its answer)`);
	ok(sawY, 'F3: another badge\'s program is still heard meanwhile');
	// Keep going until B's record from X comes back (the fake answers bad: X has no key there).
	let bad = false;
	for (let t = 0; t < 90 && !bad; t++) {
		hello(X, fakes[0]);
		await sleep(1000);
		bad = fake.syncs.some((x) => x.answer.res?.some((r: any) => r.r === 'bad' && x.body.rec?.some((e: any) => e.f === X.id && e.n === r.n)));
	}
	ok(bad, 'F3: B made a record from the hostile badge and the server said bad');
	const before = recsFrom(X.id);
	await sleep(6000);
	const t1 = Date.now();
	let after = 0;
	while (Date.now() - t1 < 25_000) {
		hello(X, fakes[(i++) % fakes.length]);
		await sleep(250);
		if (i % 20 === 0) after = Math.max(after, await entries(X.id));
	}
	ok(after === 0 && recsFrom(X.id) === before, `F3: after the bad answer B ignores that badge (entries ${after}, records ${recsFrom(X.id) - before} more)`);
	air.close();
}

console.log(failures ? `${failures} FAILED` : 'all passed');
for (const e of [a, b]) {
	try { e.mod.ccall('emu_halt', null, [], []); } catch { /* old build */ }
}
fake.stop();
process.exit(failures ? 1 : 0);
