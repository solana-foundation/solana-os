// The server for Cheer (wifi-room/main.lua and mesh-and-wifi/main.lua).
//
//   GET  /rooms/<name>   a WebSocket room: every cheer goes to everyone in it
//   POST /score          {batch: id, cheers: n} from Cheer (mesh), each batch counted once
//
// Run it with Bun behind HTTPS on port 443 at the host the apps list
// (`hosts: ["cheer.example.com"]`): badges connect only to wss:// and https://
// on 443. PUBLIC_URL is that address as badges call it, because the signature
// covers the URL the badge asked for, not the one a proxy hands to Bun.
//
//   PUBLIC_URL=https://cheer.example.com PORT=8080 bun server.ts
//
// The ticket and header checks follow LUA_API.md (`http` "What your server
// receives", `ws` "What the host receives"); here every request waiting for the
// signing key shares one fetch, and a request id is checked and claimed in one
// step after the signature, so no copy gets in twice. Counts and the batch ids
// already counted live in memory; keep them in a database (the count and the
// batch id in one transaction) for anything that has to survive a restart.

// The badge server's API, not the store's address. Set it to a stand-in to test without one.
const BADGE_SERVER = process.env.BADGE_SERVER ?? 'https://badge-api.solana.com';
const APP_WS = 'cheer-room';                         // wifi-room/app.json "id"
const APP_HTTP = 'cheer-mesh';                       // mesh-and-wifi/app.json "id"
const PUBLIC_URL = process.env.PUBLIC_URL ?? 'https://cheer.example.com';
const HOST = new URL(PUBLIC_URL).host;
const MINUTE = 60_000;                               // how old a signature may be, either way

// ---------------------------------------------------------------- checks (from LUA_API.md)
const keys = new Map<string, CryptoKey>();
const seen = new Map<string, number>(); // request id -> its time, for replays
let fetching: Promise<void> | null = null; // the key fetch in flight: everyone waits for the same one
let fetchedAt = 0;

async function badgeKey(id: string): Promise<CryptoKey | null> {
  if (!keys.has(id) && !fetching && Date.now() - fetchedAt > MINUTE) { // at most once a minute
    fetchedAt = Date.now();
    fetching = (async () => {
      const k = await (await fetch(`${BADGE_SERVER}/api/http/key`)).json();
      const raw = Uint8Array.from(atob(k.key), (c) => c.charCodeAt(0));
      keys.set(k.id, await crypto.subtle.importKey('raw', raw, 'Ed25519', false, ['verify']));
    })()
      .catch(() => { fetchedAt = 0; }) // failed: the next request tries again
      .finally(() => { fetching = null; });
  }
  if (!keys.has(id) && fetching) await fetching;
  return keys.get(id) ?? null;
}

/** True the first time `key` comes, false again for a minute after `time`. The check and
 *  the claim are one step, with no `await` between them: two copies arriving together
 *  can't both pass. Older entries go as it runs, so the map holds a minute's worth. */
function once(memory: Map<string, number>, key: string, time: number): boolean {
  const now = Date.now();
  for (const [k, t] of memory) if (now - t > MINUTE) memory.delete(k);
  if (memory.has(key)) return false;
  memory.set(key, time);
  return true;
}

/** The badge and player behind an `http` request, or null. */
async function fromBadge(req: Request, url: string, body: string, app: string) {
  const h = (n: string) => req.headers.get(n) ?? '';
  const time = Number(h('x-badge-time'));
  const id = h('x-badge-request');
  if (h('x-badge-app') !== app || !(Math.abs(Date.now() - time) < MINUTE) || !id) return null;
  const key = await badgeKey(h('x-badge-key'));
  if (!key) return null;
  const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(body)));
  const signed = ['badge-http-v1', req.method, url, [...hash].map((b) => b.toString(16).padStart(2, '0')).join(''),
    app, h('x-badge-id'), h('x-badge-user'), h('x-badge-time'), id].join('\n');
  const sig = Uint8Array.from(atob(h('x-badge-signature')), (c) => c.charCodeAt(0));
  if (!(await crypto.subtle.verify('Ed25519', key, sig, new TextEncoder().encode(signed)))) return null;
  if (!once(seen, id, time)) return null; // a replay: after the last await, so a copy can't slip past
  return { badge: h('x-badge-id'), user: h('x-badge-user') };
}

/** The badge and player opening a socket, or null. */
async function fromBadgeSocket(url: string, app: string) {
  const ticket = new URL(url).searchParams.get('badge_ticket') ?? '';
  const [a, badge, user, time, request, keyId, sig, extra] = ticket.split('.');
  if (extra !== undefined || a !== app || !sig || !request ||
      !(Math.abs(Date.now() - Number(time)) < MINUTE)) return null;
  const key = await badgeKey(keyId);
  if (!key) return null;
  const signed = ['badge-ws-v1', HOST, app, badge, user, time, request].join('\n');
  const raw = Uint8Array.from(atob(sig.replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
  if (!(await crypto.subtle.verify('Ed25519', key, raw, new TextEncoder().encode(signed)))) return null;
  if (!once(seen, request, Number(time))) return null; // one ticket, one socket
  return { badge, user };
}

// ---------------------------------------------------------------- the game
type Who = { badge: string; user: string; room: string; last: number };
const totals = new Map<string, number>();   // room -> cheers
const present = new Map<string, number>();  // room -> sockets open
const scores = new Map<string, number>();   // user -> cheers posted by Cheer (mesh)
// Batches already counted, so a batch sent again is answered but not counted again. A badge
// sends one batch until it hears back, maybe days later: keep each badge's latest batch for
// good. A copy of an older one can still arrive for a minute (the signature's window): keep
// every batch for that minute too.
const latest = new Map<string, string>();   // user + badge -> its last batch counted
const counted = new Map<string, number>();  // user + batch -> when it was counted
const MAX_BATCH = 1000;                     // cheers per batch; the app sends no more

const state = (room: string, last: string | null) =>
  JSON.stringify({ total: totals.get(room) ?? 0, here: present.get(room) ?? 0, last });

const server = Bun.serve<Who>({
  port: Number(process.env.PORT ?? 8080),
  async fetch(req, server) {
    const path = new URL(req.url).pathname;
    const url = PUBLIC_URL + path + new URL(req.url).search; // what the badge signed

    const room = path.match(/^\/rooms\/([a-z0-9-]{1,24})$/)?.[1];
    if (room && req.method === 'GET') {
      const who = await fromBadgeSocket(url, APP_WS);
      if (who && server.upgrade(req, { data: { ...who, room, last: 0 } })) return undefined;
      return new Response('no', { status: 403 });
    }

    if (path === '/score' && req.method === 'POST') {
      const body = await req.text();
      const who = await fromBadge(req, url, body, APP_HTTP);
      if (!who) return new Response('no', { status: 403 });
      let msg: { batch?: unknown; cheers?: unknown } = {};
      try { msg = JSON.parse(body) ?? {}; } catch {}
      const batch = String(msg.batch ?? '');      // the app's id for these cheers, the same on every retry
      const n = Number(msg.cheers);
      // The player's badge says how many; your server decides what it believes.
      if (!/^[0-9a-f]{1,32}$/.test(batch) || !Number.isInteger(n) || n < 1 || n > MAX_BATCH) {
        return new Response('bad batch', { status: 400 });
      }
      // Count it the first time, answer every time: the app forgets a batch only once it has
      // this answer, so a lost answer means the same batch again. No await from here on.
      const from = `${who.user} ${who.badge}`;
      if (latest.get(from) !== batch && once(counted, `${who.user} ${batch}`, Date.now())) {
        latest.set(from, batch);
        scores.set(who.user, (scores.get(who.user) ?? 0) + n);
      }
      return Response.json({ batch, total: scores.get(who.user) ?? 0 }); // short: badges read ≤ about 3.5 KB
    }
    return new Response('not found', { status: 404 });
  },
  websocket: {
    open(ws) {
      const { room } = ws.data;
      present.set(room, (present.get(room) ?? 0) + 1);
      ws.subscribe(room);
      server.publish(room, state(room, null));
    },
    message(ws, text) {
      const { room } = ws.data;
      // One cheer every quarter second per badge, whatever the badge sends.
      if (Date.now() - ws.data.last < 250) return;
      ws.data.last = Date.now();
      let msg: { c?: unknown } = {};
      try { msg = JSON.parse(String(text)); } catch { return; }
      if (msg.c !== 1) return;
      totals.set(room, (totals.get(room) ?? 0) + 1);
      server.publish(room, state(room, ws.data.badge)); // ≤ 4 KB: the badge closes on bigger messages
    },
    close(ws) {
      const { room } = ws.data;
      present.set(room, Math.max(0, (present.get(room) ?? 1) - 1));
      server.publish(room, state(room, null));
    },
  },
});

console.log(`cheer server on :${server.port}, signed for ${PUBLIC_URL}`);
