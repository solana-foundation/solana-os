// lab-mock.ts: the Lab (gossip-lab, a C app in the parent repo) on the mock host,
// every screen walked by script against the scripted states of states.ts.
//
//   bun firmware/sdk/c/tests/gossip/lab-mock.ts [--app <gossip-lab dir>] [--only <scenario>]
//
// Builds the app with tools/cc/badge-cc, then runs mock-host.ts once per
// scenario (each a fresh app). Exit 1 when any scenario has a problem.
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const args = Bun.argv.slice(2);
const opt = (k: string) => {
    const i = args.indexOf(k);
    return i >= 0 ? args[i + 1] : undefined;
};
const here = import.meta.dir;
const sdk = resolve(here, '../../../..');
const app = resolve(opt('--app') ?? join(sdk, '../server/content/apps/gossip-lab'));
if (!existsSync(join(app, 'main.c'))) {
    console.error(`lab-mock: no main.c in ${app} (pass --app)`);
    process.exit(2);
}
const wasm = join(mkdtempSync(join(tmpdir(), 'lab-mock-')), 'lab.wasm');
const cc = Bun.spawnSync([join(sdk, 'tools/cc/badge-cc'), app, '-o', wasm], { stdout: 'inherit', stderr: 'inherit' });
if (cc.exitCode !== 0) process.exit(1);

// [name, starting state, script]
const SCENARIOS: [string, string, string][] = [
    ['not linked, then linked', 'unlinked', 'screen:unlinked t:5 state:busy t:200 screen:home none'],
    ['no gossip module', 'nomodule', 'screen:unlinked t:20 none'],
    ['first launch: what it is, a class, the first deploy', 'fresh',
        'screen:intro k:select t:1 screen:intro sel:1 want:set_class t:1 k:select want:deploy screen:home'],
    ['first launch put off', 'fresh', 'screen:intro k:back screen:home none'],
    ['an empty node: every screen says so', 'empty',
        'screen:intro k:back screen:home sel:0 screen:node t:3 k:back sel:1 screen:slots sel:0 screen:slots k:back sel:2 screen:radar t:40 k:back ' +
        'sel:3 screen:hearing t:40 k:back sel:4 screen:items k:back sel:5 screen:timers t:40 k:back sel:6 screen:objectives t:40 k:back ' +
        'sel:7 screen:trophies k:back sel:8 screen:settings k:back screen:home none'],
    ['a busy node: slots, programs, place, drop, activate, name, parent', 'busy',
        'screen:home sel:0 screen:node t:5 sel:0 want:deploy sel:1 screen:settings k:back k:back screen:home ' +
        'sel:1 screen:slots t:5 sel:0 screen:program t:5 k:select screen:program sel:0 screen:pick sel:2 want:place ' +
        'sel:1 want:drop k:back k:back screen:slots sel:1 screen:program k:select sel:0 want:activate screen:program ' +
        'k:select sel:3 want:name sel:4 screen:program t:5 k:back k:back k:back screen:slots ' +
        'sel:5 screen:program k:select sel:0 screen:pick sel:0 want:place screen:program'],
    ['items: each kind and its picker', 'busy',
        'sel:4 screen:items sel:0 screen:item k:select want:use screen:items sel:2 screen:item k:select screen:pick sel:1 want:use ' +
        'screen:items sel:5 screen:item k:select screen:pick sel:0 screen:pick sel:0 want:use screen:items ' +
        'sel:3 screen:item k:select screen:pick sel:2 want:use screen:items sel:8 screen:item k:select want:deploy screen:items ' +
        'sel:10 screen:item k:select none k:back screen:items sel:6 screen:item k:select screen:pick sel:3 want:use screen:items'],
    ['timers, objectives, trophies, settings', 'busy',
        'sel:5 screen:timers t:60 k:back sel:6 screen:objectives t:60 k:back sel:7 screen:trophies k:back ' +
        'sel:8 screen:settings sel:1 want:public sel:2 want:lead sel:0 none sel:3 screen:settings sel:2 none'],
    ['the radar with 20 nodes and roles, one node, its program', 'nearby',
        'sel:2 screen:radar t:80 k:down k:down t:20 sel:0 screen:peer t:20 sel:0 screen:program k:back k:back ' +
        'sel:5 screen:peer t:20 k:back sel:19 screen:peer t:10 k:back k:back sel:3 screen:hearing t:80 none'],
    ['exposure and results arriving', 'exposure',
        'sel:3 screen:hearing t:40 ev:exposed ev:adopted t:20 ev:forked ev:seen ev:node ev:rank ev:milestone ev:drop ev:role t:40 ' +
        'k:back sel:7 screen:trophies k:back none'],
    ['gossip off and long unsynced', 'offline', 'screen:home t:40 sel:8 screen:settings sel:0 none'],
    // The role side (docs/GOSSIP.md section 8), with the module's real shapes (LAB-NOTES).
    ['a role offer: the banner, the countdown, accept', 'offer', 'screen:home t:3 sel:0 screen:offer t:40 sel:0 want:role_answer~true'],
    ['a role offer declined', 'offer-mev', 'screen:home sel:0 screen:offer sel:1 want:role_end'],
    ['the offer starts a Leader slot: bids, the block, the end', 'offer',
        'sel:0 screen:offer state:leader t:40 screen:leader t:60 sel:2 sel:3 sel:0 want:block~B01X9Q ev:bid t:60 sel:4 none k:right want:role_end'],
    ['the MEV Bot as its owner', 'mev', 'screen:home sel:0 screen:hide t:60 k:right want:role_end'],
    ['the Anonymous Deployer as its owner', 'anon', 'screen:home sel:0 screen:hide t:60 ev:session t:5 k:back screen:home none'],
    ['an ended role and what it paid', 'ended', 'screen:home sel:0 screen:role-ended k:back screen:home ev:ended none'],
    ['roles on the radar: catch close up, bid on a Leader, too far to find', 'roles',
        'sel:2 screen:radar t:5 sel:0 screen:peer sel:0 screen:target t:3 sel:0 want:claim~R0LE02 ev:ack t:3 sel:0 none k:back k:back ' +
        'sel:1 screen:peer sel:0 screen:target sel:0 screen:pick sel:0 screen:bid sel:0 want:bid~true screen:target ev:bid-refused k:back k:back ' +
        'sel:5 screen:peer sel:0 screen:target t:3 sel:0 none ev:noack k:back k:back sel:2 screen:peer sel:0 screen:target sel:0 none'],
    ['a Validator cures spam nearby; the cured side', 'validator',
        'sel:3 screen:cure t:3 sel:0 want:cure~S0PAM1 ev:cured t:2 k:back sel:2 screen:radar t:3 sel:3 screen:peer sel:0 want:cure ev:cured-me'],
    ['a fight for a player: the beat, votes, a miss, a fork, won', 'fight',
        'screen:home sel:0 log:lab:_fight_R0LE04 t:5 tobeat:0:10 t:1 k:select log:vote_in_time want:vote ' +
        'beat:-40 t:1 k:select beat:-40 t:1 k:select beat:-40 t:1 k:select log:vote_in_time want:vote~4 ' +
        'tobeat:5:180 t:1 k:select log:vote_miss want:vote~0 tobeat:12:10 t:1 k:select log:vote_fork t:40 log:lab:_led ' +
        'fight:2 t:10 screen:fight-result ev:ack-fight ev:boss-paid'],
    ['a fight for a player, lost', 'fight',
        'sel:0 log:lab:_fight tobeat:0:5 t:1 k:select want:vote k:back screen:home sel:0 log:lab:_fight fight:3 t:10 screen:fight-result'],
    ["the boss's owner: the metronome, forks, the end", 'boss-own',
        'screen:home sel:0 log:lab:_boss_fight t:10 tobeat:3:100 t:4 k:up want:fork t:40 k:left want:fork fight:2 t:5 screen:fight-result'],
    ['trophies, how to play, the Upgrade Authority+ trait, the lead setting', 'ach',
        'sel:7 screen:trophies ev:achievement k:back sel:9 screen:howto k:back sel:4 screen:items sel:0 screen:item k:select screen:pick sel:0 ' +
        'screen:trait sel:2 want:use~trait screen:items sel:1 screen:item k:select screen:pick sel:0 want:use!trait k:back ' +
        'sel:8 screen:settings sel:2 want:lead~false'],
];

const only = opt('--only');
let failed = 0;
for (const [name, state, script] of SCENARIOS) {
    if (only && !name.includes(only)) continue;
    const r = Bun.spawnSync(['bun', join(sdk, 'sdk/c/tests/mock-host.ts'), wasm, '--mock', join(here, 'gossip-mock.ts'), '--keys', '',
        '--ticks', '2', '--state', state, '--script', script]);
    const out = r.stdout.toString() + r.stderr.toString();
    const problems = out.split('\n').filter((l) => l.includes('PROBLEM'));
    const screens = [...new Set(out.split('\n').map((l) => l.match(/\[app\] lab: (\S+)/)?.[1]).filter(Boolean))];
    const acts = out.split('\n').filter((l) => l.startsWith('gossip-mock: ') && l.includes('(')).map((l) => l.slice(13));
    if (r.exitCode !== 0 || problems.length) {
        failed++;
        console.log(`FAIL ${name}\n  ${problems.join('\n  ') || out.split('\n').slice(-15).join('\n  ')}`);
    } else {
        console.log(`ok   ${name}: ${screens.join(', ')}${acts.length ? `; ${acts.join(' ')}` : ''}`);
    }
}
console.log(failed ? `lab-mock: ${failed} scenarios failed` : 'lab-mock: every scenario passed');
process.exit(failed ? 1 : 0);
