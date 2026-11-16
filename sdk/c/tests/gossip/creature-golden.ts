// creature-golden.ts: the Lab's creature renderer against docs/GOSSIP.md section 3.
// Compiles creature.c with the host's C compiler, decodes the contract's three
// vector genomes and checks every field, then hashes frame 0 of each (48 x 48
// RGB565 values as little-endian bytes, on black) against the golden hashes the
// web's port must reproduce. Each frame of each vector is also written as a PNG
// (scaled 4x) when --png <dir> is given.
//
//   bun firmware/sdk/c/tests/gossip/creature-golden.ts [--app <gossip-lab dir>] [--png <dir>]
//
// The Lab lives in the parent repo (server/content/apps/gossip-lab); --app
// points elsewhere. Exit 1 on any mismatch.
import { existsSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { png } from './png';

const args = Bun.argv.slice(2);
const opt = (k: string) => {
    const i = args.indexOf(k);
    return i >= 0 ? args[i + 1] : undefined;
};
const app = resolve(opt('--app') ?? join(import.meta.dir, '../../../../../server/content/apps/gossip-lab'));
if (!existsSync(join(app, 'creature.c'))) {
    console.error(`creature-golden: no creature.c in ${app} (pass --app)`);
    process.exit(2);
}

// docs/GOSSIP.md section 3, "Test vectors".
const VECTORS = [
    { name: 'vector 1', g: '03ec29c2b46ec7796a2c660097494fe9158ea94290cf78279c5cbfe6d2239d62', hue: 3, rgb: [255, 77, 64], body: 4, eyes: 5, marks: 1, seed: 5, traits: [2, 3], vhs: [10, 2, 7], rarity: 1 },
    { name: 'vector 2', g: '49e674d6bae74e9e521eaed32e56b2b232a24972a6c55ffcf4d9e45d27bbf8e0', hue: 73, rgb: [120, 255, 64], body: 6, eyes: 4, marks: 4, seed: 14, traits: [4, 0], vhs: [9, 9, 3], rarity: 1 },
    { name: 'vector 3', g: 'd263b55cf6342cdfcb1a034523cdc72c26c672f64ed11e2eb76aa9f4cb1e9bcb', hue: 210, rgb: [240, 64, 255], body: 3, eyes: 4, marks: 5, seed: 22, traits: [2, 4, 0], vhs: [5, 4, 4], rarity: 1 },
];
// SHA-256 of frame 0 (bg 0x000000). The web's renderer must produce the same.
const GOLDEN: Record<string, string> = {
    'vector 1': '0922d9b7f45f0fe246bb85a09167ec13cd588f480746a93f7f09a3bce3f37358',
    'vector 2': 'dc409de07c51201699b191a23f58619d301e2caee85e56caaea4b24215db4c84',
    'vector 3': '74ab953af50586b6f2386b8cef1f4e8fb5803f05daf70421073c69d8056e0269',
};

const dir = mkdtempSync(join(tmpdir(), 'creature-'));
const harness = `#include <stdio.h>
#include "creature.h"
static int hex(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
int creature_selftest(void);
int main(int argc, char **argv) {
    printf("S %d\\n", creature_selftest());
    for (int a = 1; a < argc; a++) {
        uint8_t g[32]; uint16_t px[48 * 48]; uint8_t rgb[3]; creature_genome c;
        for (int i = 0; i < 32; i++) g[i] = (uint8_t)(hex(argv[a][2 * i]) * 16 + hex(argv[a][2 * i + 1]));
        creature_decode(g, &c);
        creature_rgb(c.hue, rgb);
        printf("D %d %d %d %d %d %d %d %d %d %d %d %d %d", c.hue, rgb[0], rgb[1], rgb[2], c.body, c.eyes, c.marks, c.seed, c.rarity, c.vir, c.hard, c.stab, c.ntraits);
        for (int i = 0; i < c.ntraits; i++) printf(" %d", c.traits[i]);
        printf("\\n");
        for (int f = 0; f < CREATURE_FRAMES; f++) {
            creature_render(&c, f, 0x000000, px);
            printf("F%d ", f);
            for (int i = 0; i < 48 * 48; i++) printf("%02x%02x", px[i] & 255, px[i] >> 8);
            printf("\\n");
        }
    }
    return 0;
}
`;
writeFileSync(join(dir, 'main.c'), harness);
const cc = Bun.spawnSync(['cc', '-std=c11', '-O1', '-DCREATURE_SELFTEST', '-Wall', '-Wextra', '-Werror', '-I', app, '-o', join(dir, 'golden'), join(dir, 'main.c'), join(app, 'creature.c')]);
if (cc.exitCode !== 0) {
    console.error(cc.stderr.toString());
    process.exit(1);
}
const run = Bun.spawnSync([join(dir, 'golden'), ...VECTORS.map((v) => v.g)]);
const all = run.stdout.toString().trim().split('\n');
const selftest = Number(all[0].slice(2));
const lines = all.slice(1);
const pngDir = opt('--png');
let bad = 0;
const fail = (m: string) => {
    bad++;
    console.log(`creature-golden: FAIL ${m}`);
};
if (selftest !== 0) fail(`the fast body masks differ from shape_in() at ${selftest} pixels`);

function rgb565png(px: Uint8Array, scale: number): Uint8Array {
    return png(48 * scale, 48 * scale, (x, y) => {
        const k = ((y / scale) | 0) * 48 + ((x / scale) | 0);
        const v = px[k * 2] | (px[k * 2 + 1] << 8);
        return [(((v >> 11) & 31) * 255) / 31, (((v >> 5) & 63) * 255) / 63, ((v & 31) * 255) / 31];
    });
}

VECTORS.forEach((v, n) => {
    const d = lines[n * 4].split(' ').slice(1).map(Number);
    const want = [v.hue, ...v.rgb, v.body, v.eyes, v.marks, v.seed, v.rarity, ...v.vhs, v.traits.length, ...v.traits];
    if (JSON.stringify(d) !== JSON.stringify(want)) fail(`${v.name}: decoded ${d.join(' ')}, the contract says ${want.join(' ')}`);
    for (let f = 0; f < 3; f++) {
        const hex = lines[n * 4 + 1 + f].slice(3);
        const px = Uint8Array.from(Buffer.from(hex, 'hex'));
        if (px.length !== 48 * 48 * 2) fail(`${v.name} frame ${f}: ${px.length} bytes`);
        const hash = new Bun.CryptoHasher('sha256').update(px).digest('hex');
        if (f === 0) {
            console.log(`creature-golden: ${v.name} frame 0 sha256 ${hash}`);
            if (GOLDEN[v.name] !== hash) fail(`${v.name}: frame 0 hashes ${hash}, golden ${GOLDEN[v.name]}`);
        }
        if (pngDir) writeFileSync(join(pngDir, `creature-v${n + 1}-f${f}.png`), rgb565png(px, 4));
    }
});
console.log(bad ? `creature-golden: ${bad} failures` : 'creature-golden: all vectors decode as the contract says and match the golden hashes');
process.exit(bad ? 1 : 0);
