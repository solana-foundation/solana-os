// gossip-mock.ts: the gossip module (docs/GOSSIP.md section 10) for mock-host.ts,
// with scripted states (states.ts) and a script of steps run after the keys.
//
//   bun tests/mock-host.ts app.wasm --mock tests/gossip/gossip-mock.ts --keys '' --ticks 2 \
//       --state busy --script 'screen:home sel:1 screen:slots ...'
//
// Steps (space separated):
//   k:<key>        on_key (up down left right select back)
//   t:<n>          n ticks
//   sel:<i>        the newest list's on_select, row i (from 0)
//   ev:<name>      on_gossip with states.ts EVENTS[name]
//   state:<name>   the module answers from another state from now on
//   screen:<name>  the app's last "lab: <name>" line names that screen
//   want:<fn>      the app called gossip.<fn> since the last want (acting calls);
//                  want:<fn>~<text> also needs <text> in its arguments, want:<fn>!<text> not
//   wait:<ms>      the clock moves on without a tick
//   beat:<off>     the clock moves to the fight's next beat + off ms (no tick)
//   tobeat:<k>:<off> the clock moves to beat k + off ms
//   fight:<phase>  the fight ends: 2 rooted, 3 the boss won
//   none           no acting call since the last want
// The clock: sys.millis() is the mock's own (it starts at 1000 and moves by the tick
// period the app asked for, sys.tick, at each t step, and by wait/beat steps), so the
// fight's beats (states.ts `fight`) land where the app expects them.
//   log:<text>     a console line containing text since the last log check
// Lists don't select by themselves here (mock-host's ui.list does): sel does it.
import { EVENTS, NOW, STATES, bossAt, votersAt, type State } from './states';

type Ctx = Record<string, any>;
let state: State;
let stateName = 'empty';
const acted: string[] = [];
const lines: string[] = [];
let logMark = 0;
let lastList: { f: any; n: number } | null = null;
let clock = 1000;
let period = 30;
export const millis = () => clock++; // a real clock moves within a callback too

const ACTING = ['set_class', 'activate', 'place', 'drop', 'deploy', 'use', 'name', 'public', 'lead', 'bid', 'claim', 'cure', 'vote',
    'role_answer', 'role_end', 'block', 'fork'];

export function install(c: Ctx) {
    const { api, fn, want, js2v, v2js, NIL, s, b, tbl, args } = c;
    const opt = (k: string, d: string) => {
        const i = args.indexOf(k);
        return i >= 0 ? args[i + 1] : d;
    };
    stateName = opt('--state', 'empty');
    if (!STATES[stateName]) throw new Error(`gossip-mock: no state ${stateName}`);
    state = STATES[stateName]();
    // Capture what the app prints (the screen names).
    const log = console.log;
    console.log = (...a: unknown[]) => {
        lines.push(a.join(' '));
        log(...a);
    };
    const read = (name: keyof State) => fn((a: any[]) => {
        if (name === 'me' && state.me === undefined) throw 'attempt to call a nil value';
        want(a, name === 'program' || name === 'genome' ? 's' : '', `gossip.${name}`);
        const v = name === 'program' ? (state.program ?? {})[c.str(a[0])] : state[name];
        return [v === undefined || v === null ? NIL : js2v(v)];
    });
    for (const n of ['me', 'programs', 'program', 'nearby', 'hearing', 'role'] as const) api[`gossip.${n}`] = read(n);
    // The fight, from the clock (beat and ms moved forward to the read, as the badge does).
    api['gossip.boss'] = fn((a: any[]) => {
        want(a, '', 'gossip.boss');
        const f = state.fight;
        return [f && !f.own && f.heard !== false ? js2v(bossAt(f, clock)) : NIL];
    });
    api['gossip.voters'] = fn((a: any[]) => {
        want(a, '', 'gossip.voters');
        const f = state.fight;
        return [f && f.own ? js2v(votersAt(f, clock)) : NIL];
    });
    api['gossip.bids'] = fn((a: any[]) => (want(a, '', 'gossip.bids'), [state.bids ? js2v(state.bids) : NIL]));
    api['sys.tick'] = fn((a: any[]) => {
        want(a, 'i', 'sys.tick');
        const old = period;
        period = a[0].v;
        return [c.i(old)];
    });
    const specs: Record<string, string> = {
        set_class: 's', activate: 's', place: 's i', drop: 'i', deploy: 's?', use: 's t?', name: 's s', public: 'b', lead: 'b',
        bid: 's s b?', claim: 's i', cure: 's', vote: 'i i', role_answer: 'b', role_end: '', block: 't', fork: '',
    };
    for (const n of ACTING) {
        api[`gossip.${n}`] = fn((a: any[]) => {
            want(a, specs[n], `gossip.${n}`);
            const shown = a.map((x: any) => JSON.stringify(v2js(x))).join(', ');
            acted.push(`${n}(${shown})`);
            if (n === 'name' && c.str(a[1]).length > 16) return [NIL, s('too long')];
            if (n === 'place' && (a[1].v < 1 || a[1].v > 5)) return [NIL, s('no such slot')];
            return [b(true)];
        });
    }
    // ui.list without the stand-in's automatic selection; `sel` selects.
    api['ui.list'] = fn((a: any[]) => {
        want(a, 'i i i i t t?', 'ui.list');
        const o = a[5];
        const cb = o?.t === 5 ? o.v.get('on_select') : undefined;
        if (cb && cb.t !== 6) c.bug('ui.list: on_select is not a function');
        const rows = a[4].t === 5 ? a[4].v.size : 0;
        lastList = { f: cb ?? null, n: rows };
        return [c.obj('list', {
            items: (x: any[]) => (want(x, 'o t', 'list:items'), (lastList = { f: lastList?.f ?? null, n: x[1].v.size }), []),
            selected: (x: any[]) => (want(x, 'o', 'list:selected'), [c.i(1)]),
            select: (x: any[]) => (want(x, 'o i', 'list:select'), []),
        })];
    });
    // The theme's background reads black.
    api['ui.canvas'] = fn((a: any[]) => {
        want(a, 'i i i i t?', 'ui.canvas');
        return [c.obj('canvas', { pixel: () => [], get: () => [c.i(0)], clear: () => [] })];
    });
    // sys.time as the states expect.
    api['sys.time'] = fn(() => [c.i(NOW)]);
}

export function script(c: Ctx) {
    const { run, deliver, ex, js2v, problems, args } = c;
    const i = args.indexOf('--script');
    const steps = (i >= 0 ? args[i + 1] : '').split(/\s+/).filter(Boolean);
    const keys: Record<string, number> = { up: 0, down: 1, left: 2, right: 3, select: 4, back: 5 };
    const screen = () => {
        for (let k = lines.length - 1; k >= 0; k--) {
            const m = lines[k].match(/\[app\] lab: (\S+)/);
            if (m) return m[1];
        }
        return '';
    };
    let mark = 0;
    for (const step of steps) {
        const [op, arg = ''] = [step.slice(0, step.indexOf(':') < 0 ? step.length : step.indexOf(':')), step.slice(step.indexOf(':') + 1)];
        const fail = (m: string) => problems.push(`script ${step}: ${m}`);
        if (op === 'k') {
            if (!(arg in keys)) fail('no such key');
            run(`on_key(${arg})`, () => ex.on_key(keys[arg], 0));
            deliver();
        } else if (op === 't') {
            for (let n = 0; n < Number(arg); n++) {
                clock += period;
                run('on_tick', () => ex.on_tick(period));
                deliver();
            }
        } else if (op === 'wait') {
            clock += Number(arg);
        } else if (op === 'beat' || op === 'tobeat') {
            const f = state.fight;
            if (!f) fail('no fight in this state');
            else {
                const bm = 60000 / f.bpm;
                const [kArg, offArg] = op === 'tobeat' ? arg.split(':') : ['', arg];
                const k = op === 'tobeat' ? Number(kArg) : Math.max(0, Math.floor((clock - Number(offArg) - f.t0) / bm) + 1);
                const to = f.t0 + k * bm + Number(offArg);
                if (to < clock) fail(`beat ${k} + ${offArg} is in the past`);
                else clock = to;
            }
        } else if (op === 'fight') {
            if (!state.fight) fail('no fight in this state');
            else state.fight.end = Number(arg);
        } else if (op === 'sel') {
            if (!lastList?.f) fail('no list with on_select');
            else if (Number(arg) >= lastList.n) fail(`the list has ${lastList.n} rows`);
            else {
                const f = lastList.f;
                c.later(f, [Number(arg) + 1, NIL_SAFE(c)], 'list on_select');
                deliver();
            }
        } else if (op === 'ev') {
            if (!EVENTS[arg]) fail('no such event');
            else if (!ex.on_gossip) fail('the app exports no on_gossip');
            else {
                run(`on_gossip(${arg})`, () => ex.on_gossip(), [js2v(EVENTS[arg])]);
                deliver();
            }
        } else if (op === 'state') {
            if (!STATES[arg]) fail('no such state');
            else {
                stateName = arg;
                state = STATES[arg]();
            }
        } else if (op === 'screen') {
            if (screen() !== arg) fail(`on screen "${screen()}"`);
        } else if (op === 'want') {
            const got = acted.slice(mark);
            const [name, has, not] = [arg.split(/[~!]/)[0], arg.includes('~') ? arg.split('~')[1] : '', arg.includes('!') ? arg.split('!')[1] : ''];
            const ok = (x: string) => x.startsWith(`${name}(`) && (!has || x.includes(has)) && (!not || !x.includes(not));
            if (!got.some(ok)) fail(`acting calls since the last check: ${got.join(' ') || 'none'}`);
            else console.log(`gossip-mock: ${got.join(' ')}`);
            mark = acted.length;
        } else if (op === 'none') {
            if (acted.length > mark) fail(`acting calls: ${acted.slice(mark).join(' ')}`);
        } else if (op === 'log') {
            const text = arg.replace(/_/g, ' ');
            if (!lines.slice(logMark).some((l) => l.includes(text))) fail('not printed');
            logMark = lines.length;
        } else fail('unknown step');
    }
    console.log(`gossip-mock: state ${stateName}, ${steps.length} steps, ${acted.length} acting calls`);
}

const NIL_SAFE = (c: Ctx) => c.NIL;
