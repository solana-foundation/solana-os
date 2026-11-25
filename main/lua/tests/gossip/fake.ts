// A fake of the server's side of Gossip (docs/GOSSIP.md §3 and §7, the parent
// repo) for the badge's tests, with just enough of the badge API for emulated
// badges to say hello and come out claimed:
//   POST /api/badge/hello            an id, a secret and a mesh key per hw, claimed at once
//   GET  /api/device/gossip/key      the test signing key
//   POST /api/device/gossip/sync     §7 by simple rules (below)
// Rules, not the game's: every exposure whose tag checks with the source's
// mesh key and whose source holds the program is adopted (no odds, no forks,
// no immunity); `deploy` makes a root program with a nonce whose PDA it finds
// and signs; `class`, `active`, `place`, `drop` and `public` do the obvious;
// anything else answers ok: false. Records are signed with an Ed25519 test key
// over "sos-gossip1|a|par|gen|seed|bump|fl" (§3). Not a test of the server:
// server/src/gossip/ is, next door.
//
//   import { startFake } from './fake';  const f = startFake(3211);
import { createHmac, randomBytes } from 'node:crypto';

const sha256 = (...parts: Uint8Array[]): Uint8Array => {
	const h = new Bun.CryptoHasher('sha256');
	for (const p of parts) h.update(p);
	return new Uint8Array(h.digest());
};
const ascii = (s: string) => new TextEncoder().encode(s);
const hex = (b: Uint8Array) => Buffer.from(b).toString('hex');

const B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
export function base58(b: Uint8Array): string {
	let n = 0n;
	for (const x of b) n = n * 256n + BigInt(x);
	let s = '';
	while (n > 0n) {
		s = B58[Number(n % 58n)] + s;
		n /= 58n;
	}
	for (const x of b) {
		if (x !== 0) break;
		s = '1' + s;
	}
	return s;
}

// curve25519-dalek's decompression, as Solana's is_on_curve.
const P = 2n ** 255n - 19n;
const mod = (a: bigint) => ((a % P) + P) % P;
const pow = (b: bigint, e: bigint) => {
	let r = 1n;
	b = mod(b);
	while (e > 0n) {
		if (e & 1n) r = (r * b) % P;
		b = (b * b) % P;
		e >>= 1n;
	}
	return r;
};
const D = mod(-121665n * pow(121666n, P - 2n));
function onCurve(b: Uint8Array): boolean {
	let y = 0n;
	for (let i = 31; i >= 0; i--) y = (y << 8n) | BigInt(i === 31 ? b[i] & 0x7f : b[i]);
	y = mod(y);
	const y2 = mod(y * y);
	const x2 = mod(mod(y2 - 1n) * pow(mod(D * y2 + 1n), P - 2n));
	return x2 === 0n || pow(x2, (P - 1n) / 2n) === 1n;
}
function pda(seeds: Uint8Array[], program: Uint8Array) {
	for (let bump = 255; bump >= 0; bump--) {
		const h = sha256(...seeds, Uint8Array.of(bump), program, ascii('ProgramDerivedAddress'));
		if (!onCurve(h)) return { address: h, bump };
	}
	throw new Error('no bump');
}
export const GOSSIP_PROGRAM = sha256(ascii('solana-bp26-gossip'));

export type Rec = { a: string; par: string; gen: number; seed: string; bump: number; fl: number; sig: string; name?: string; nodes?: number };
type Node = {
	cls: string; clsAt: number; stake: number; rank: number; credits: number; slots: (string | null)[]; act: string;
	inbox: string[]; held: number; deployed: number; deploys: number; adopted: number; boost: number;
	fx: unknown[]; imm: unknown[]; items: unknown[]; itemsN: number;
	epoch: { n: number; ends: number; obj: unknown[] }; pub: boolean; earns: boolean; why?: string;
};
type Badge = { id: string; hw: string; secret: string; key: string; node: Node; seq: number; ever: Set<string> };

const tag = (keyHex: string, s: string) => createHmac('sha256', Buffer.from(keyHex, 'hex')).update(s).digest('hex').slice(0, 16);

// coolOnce: the first sync carrying records answers the first `cool` and
// settles nothing (ack stays below it), as a node in its cooldown (§6 step 3).
export function startFake(port: number, opts: { thr?: number; next?: number; coolOnce?: boolean } = {}) {
	let cooled = false;
	const badges = new Map<string, Badge>();         // by id
	const records = new Map<string, Rec>();
	const syncs: { id: string; body: any; answer: any }[] = [];
	const seed = new Uint8Array(32).fill(7);
	const pkcs8 = new Uint8Array([0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20, ...seed]);
	const keyP = crypto.subtle.importKey('pkcs8', pkcs8, { name: 'Ed25519' }, true, ['sign']);
	let pubHex = '';
	const cfg = { thr: opts.thr ?? 100, decay: 2, wt: [[-50, 10], [-60, 6], [-70, 3], [-80, 1]], offpeak: null, close: -55, roles: [], spam: [] };
	const CFG_REV = 1;
	let rng = 1;
	const sign = async (r: Omit<Rec, 'sig'>): Promise<Rec> => {
		const k = await keyP;
		const msg = `sos-gossip1|${r.a}|${r.par}|${r.gen}|${r.seed}|${r.bump}|${r.fl}`;
		return { ...r, sig: Buffer.from(await crypto.subtle.sign('Ed25519', k, ascii(msg))).toString('hex') };
	};
	const makeRoot = async () => {
		const nonce = new Uint8Array(8);
		new DataView(nonce.buffer).setUint32(4, rng++);
		const { address, bump } = pda([ascii('root'), nonce], GOSSIP_PROGRAM);
		const r = await sign({ a: base58(address), par: '', gen: 0, seed: hex(nonce), bump, fl: 0 });
		records.set(r.a, { ...r, nodes: 1 });
		return r;
	};
	const freshNode = (): Node => ({
		cls: '', clsAt: 0, stake: 0, rank: 0, credits: 0, slots: [null, null, null], act: '', inbox: [], held: 0,
		deployed: 0, deploys: 1, adopted: 0, boost: 0, fx: [], imm: [], items: [], itemsN: 0,
		epoch: { n: 0, ends: 0, obj: [] }, pub: false, earns: false, why: 'An emulated badge earns nothing',
	});
	const holds = (b: Badge, p: string) => b.ever.has(p);
	const place = (b: Badge, p: string) => {
		const free = b.node.slots.indexOf(null);
		if (free >= 0) b.node.slots[free] = p;
		else b.node.inbox = [...b.node.inbox, p].slice(-3);
		if (!b.node.act) b.node.act = p;
		b.ever.add(p);
		b.node.held = b.node.slots.filter(Boolean).length + b.node.inbox.length;
	};

	const server = Bun.serve({
		port,
		hostname: process.env.HOST ?? '127.0.0.1',
		async fetch(req) {
			const url = new URL(req.url);
			const json = (o: unknown, status = 200) => Response.json(o, { status });
			if (url.pathname === '/api/badge/hello' && req.method === 'POST') {
				const body: any = await req.json();
				let b = [...badges.values()].find((x) => x.hw === body.hw);
				if (!b) {
					const id = 'e' + randomBytes(5).toString('hex').toUpperCase().replace(/[^0-9A-Z]/g, '0').slice(0, 5).replace(/[ILOU]/g, '7');
					b = { id, hw: body.hw, secret: randomBytes(16).toString('hex'), key: randomBytes(16).toString('hex'), node: freshNode(), seq: 1, ever: new Set() };
					badges.set(id, b);
				}
				return json({ id: b.id, secret: b.secret, key: b.key, claimed: true, user: 'u' + b.id.slice(1).toLowerCase(), owner: { name: 'Tester ' + b.id }, time: Date.now(), poll: 300, mesh: 1 });
			}
			const bearer = req.headers.get('authorization')?.replace(/^Bearer /, '') ?? '';
			const me = [...badges.values()].find((x) => x.secret === bearer);
			if (url.pathname === '/api/device/gossip/key') {
				if (!pubHex) {
					const jwk = await crypto.subtle.exportKey('jwk', await keyP);
					pubHex = Buffer.from(jwk.x!, 'base64url').toString('hex');
				}
				return json({ key: pubHex });
			}
			if (url.pathname === '/api/device/gossip/sync' && req.method === 'POST') {
				if (!me) return json({ error: 'Badge not linked' }, 403);
				const text = await req.text();
				if (text.length > 4096) return json({ error: 'too big' }, 413);
				let body = JSON.parse(text);
				const answer: any = { time: Date.now(), next: opts.next ?? 5000, ack: 0, aack: 0 };
				const res: any[] = [], ares: any[] = [], progs: Rec[] = [], ev: any[] = [];
				let changed = false;
				for (const a of body.acts ?? []) {
					answer.aack = Math.max(answer.aack, a.n);
					let ok = true, error: string | undefined;
					if (a.t === 'class' && 'rvib'.includes(a.c)) me.node.cls = a.c;
					else if (a.t === 'deploy' && me.node.deploys > 0) {
						const r = await makeRoot();
						me.node.deploys--;
						me.node.deployed++;
						place(me, r.a);
						me.node.act = r.a;
						progs.push(r);
						ev.push({ t: 'drop', text: 'Deployed: a new program', col: '9945FF' });
					} else if (a.t === 'active' && holds(me, a.p)) me.node.act = a.p;
					else if (a.t === 'drop' && a.slot >= 0 && a.slot < me.node.slots.length) me.node.slots[a.slot] = null;
					else if (a.t === 'public') me.node.pub = !!a.on;
					else {
						ok = false;
						error = 'not in the fake';
					}
					ares.push(ok ? { n: a.n, ok } : { n: a.n, ok, error });
					changed ||= ok;
				}
				if (opts.coolOnce && !cooled && body.rec?.length) {
					cooled = true;
					const first = body.rec[0];
					answer.ack = first.n - 1;
					res.push({ n: first.n, p: first.p, r: 'cool' });
					body = { ...body, rec: [] };
				}
				for (const e of body.rec ?? []) {
					answer.ack = Math.max(answer.ack, e.n);
					const m = /^([rvibn])(\d)\.(\d+)\.([0-9a-f]{16})$/.exec(e.g ?? '');
					const src = badges.get(e.f);
					let r: string;
					if (!m || !src) r = 'bad';
					else if (src === me) r = 'own';
					else if (tag(src.key, `gossip|${src.id}|${e.p}|${m[3]}|${m[1]}${m[2]}`) !== m[4]) r = 'bad';
					else if (!records.has(e.p)) r = 'unknown';
					else if (!holds(src, e.p)) r = 'missed';
					else if (holds(me, e.p)) r = 'seen';
					else {
						r = 'adopted';
						place(me, e.p);
						src.node.adopted++;
						src.node.stake += 10;
						src.seq++;
						me.node.stake += 2;
						const rec = records.get(e.p)!;
						rec.nodes = (rec.nodes ?? 0) + 1;
						progs.push(rec);
						changed = true;
					}
					res.push({ n: e.n, p: e.p, r, ...(r === 'adopted' ? { stake: 2 } : {}) });
				}
				for (const a of body.ask ?? []) {
					const rec = records.get(a);
					if (rec && !progs.includes(rec) && progs.length < 6) progs.push(rec);
				}
				if (changed) me.seq++;
				answer.seq = me.seq;
				answer.cfg = CFG_REV;
				if (body.seq !== me.seq) answer.node = me.node;
				if (body.cfg !== CFG_REV) answer.conf = cfg;
				if (res.length) answer.res = res;
				if (ares.length) answer.ares = ares;
				if (progs.length) answer.progs = progs.slice(0, 6);
				if (ev.length) answer.ev = ev;
				syncs.push({ id: me.id, body, answer });
				return json(answer);
			}
			return json({ error: 'not in the fake' }, 404);
		},
	});
	return { server, badges, records, syncs, url: `http://127.0.0.1:${port}`, stop: () => server.stop(true) };
}

// Standalone, for a badge on the LAN: HOST=0.0.0.0 PORT=3211 bun fake.ts, then
// on the badge `server http://<this machine>:3211`.
if (import.meta.main) {
	const f = startFake(Number(process.env.PORT ?? 3211), { thr: Number(process.env.THR ?? 100), next: Number(process.env.NEXT ?? 5000) });
	console.log(`gossip fake on ${f.server.url}`);
	setInterval(() => {
		for (const s of f.syncs.splice(0)) console.log(`sync ${s.id}: ${JSON.stringify(s.body).slice(0, 300)} -> ${JSON.stringify(s.answer).slice(0, 300)}`);
	}, 1000);
}
