// A small fake of the server for the role tests (docs/GOSSIP.md §7 and §8,
// the role side; server/API.md's badge hello): it gives each emulated badge an
// id and a mesh key, makes role sessions with an id and a key, and checks the
// tags badges make with them. Nothing here is the server's real logic.
import { createHmac, randomBytes } from 'node:crypto';

/** tag(key, s): the first 8 bytes of HMAC-SHA256 as 16 lowercase hex (docs/MESH.md "Tags"). */
export function tag(keyHex: string, s: string): string {
	return createHmac('sha256', Buffer.from(keyHex, 'hex')).update(s).digest('hex').slice(0, 16);
}

type Badge = { id: string; key: string; secret: string; hw: string };
const CROCK = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
const rid = (first = '') => first + Array.from(randomBytes(6 - first.length), (b) => CROCK[b & 31]).join('');

export class FakeServer {
	private srv: ReturnType<typeof Bun.serve>;
	private badges = new Map<string, Badge>();
	url: string;

	constructor(port: number) {
		this.srv = Bun.serve({
			port,
			hostname: '127.0.0.1',
			fetch: async (req) => {
				const u = new URL(req.url);
				if (u.pathname === '/api/badge/hello' && req.method === 'POST') {
					const body = (await req.json().catch(() => ({}))) as { hw?: string };
					const hw = String(body.hw ?? '');
					let b = this.badges.get(hw);
					if (!b) {
						b = { id: rid('e'), key: randomBytes(16).toString('hex'), secret: randomBytes(16).toString('hex'), hw };
						this.badges.set(hw, b);
					}
					return Response.json({
						id: b.id,
						secret: b.secret,
						key: b.key,
						claimed: true,
						owner: { name: `tester ${b.id}` },
						user: 'u1',
						poll: 300,
						mesh: 1,
						time: Date.now()
					});
				}
				return Response.json({ error: 'not here' }, { status: 404 });
			}
		});
		this.url = `http://127.0.0.1:${this.srv.port}`;
	}

	/** The badge whose emulator MAC is mac (its hw is "emu-<mac>"). */
	byMac(mac: string): Badge | undefined {
		for (const b of this.badges.values()) if (b.hw.toLowerCase().includes(mac.slice(2))) return b;
		return undefined;
	}

	/** A role session (GOSSIP.md §8 RoleState): a fresh role id and role key. */
	session(sid: number, ro: string, more: Record<string, unknown> = {}) {
		const id = rid('R');
		const key = randomBytes(16).toString('hex');
		const now = Date.now();
		const state = { sid, ro, id, key, since: now, until: now + 15 * 60_000, st: [], boost: 0, ...more };
		return { id, key, state };
	}

	stop() {
		this.srv.stop(true);
	}
}
