// Hand-made modules for the C app regression apps (build.sh runs it): what
// clang won't emit from C. Each is a whole app.wasm, no imports.
//   bun gen.ts <out dir>
//   t-c-l-start   a start function that loops for ever: the badge must
//                 refuse the module before instantiating it (WAMR would run
//                 it with no instruction limit)
//   t-c-l-init    on_start loops on memory.init of a 64 KB passive segment:
//                 the interpreter must charge it by length, native code must
//                 stop in the helper's pieces
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const leb = (v: number) => {
    const o: number[] = [];
    do {
        let b = v & 0x7f;
        v >>>= 7;
        if (v) b |= 0x80;
        o.push(b);
    } while (v);
    return o;
};
const sleb = (v: number) => {
    const o: number[] = [];
    for (;;) {
        const b = v & 0x7f;
        v >>= 7;
        if ((v === 0 && !(b & 0x40)) || (v === -1 && b & 0x40)) {
            o.push(b);
            return o;
        }
        o.push(b | 0x80);
    }
};
const str = (s: string) => [...leb(s.length), ...Buffer.from(s)];
const sec = (id: number, body: number[]) => [id, ...leb(body.length), ...body];
const fn = (locals: number[], body: number[]) => {
    const f = [...locals, ...body];
    return [...leb(f.length), ...f];
};
const module = (...sections: number[][]) => new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sections.flat()]);
const VOID = [1, 0x60, 0, 0];                         // one type: () -> ()
const LOOP = 0x03, BLOCK_VOID = 0x40, BR = 0x0c, END = 0x0b, I32_CONST = 0x41;

const out = process.argv[2];
const write = (id: string, bytes: Uint8Array) => {
    mkdirSync(join(out, id), { recursive: true });
    writeFileSync(join(out, id, 'app.wasm'), bytes);
    writeFileSync(join(out, id, 'app.json'),
        JSON.stringify({ id, name: `Test: ${id}`, version: '1.0.0', author: 'Solana OS', lang: 'c', permissions: [] }) + '\n');
};

// start: func 0 loops for ever and is the start function; func 1 is on_start.
write('t-c-l-start', module(
    sec(1, VOID),
    sec(3, [2, 0, 0]),
    sec(5, [1, 0, 1]),
    sec(7, [2, ...str('on_start'), 0, 1, ...str('memory'), 2, 0]),
    sec(8, [0]),
    sec(10, [2, ...fn([0], [LOOP, BLOCK_VOID, BR, 0, END, END]), ...fn([0], [END])]),
));

// init: on_start copies a 64 KB passive segment over the 64 KB memory for ever.
const SEG = 65536;
write('t-c-l-init', module(
    sec(1, VOID),
    sec(3, [1, 0]),
    sec(5, [1, 0, 1]),
    sec(7, [2, ...str('on_start'), 0, 0, ...str('memory'), 2, 0]),
    sec(12, [1]),                                     // data count: memory.init needs it
    sec(10, [1, ...fn([0], [
        LOOP, BLOCK_VOID,
        I32_CONST, 0, I32_CONST, 0, I32_CONST, ...sleb(SEG), 0xfc, 0x08, 0, 0,   // memory.init 0 (dst 0, offset 0, 64 KB)
        BR, 0, END, END,
    ])]),
    sec(11, [1, 1, ...leb(SEG), ...new Array(SEG).fill(0).map((_, i) => i & 0xff)]),   // one passive segment
));
console.log('hand-made modules: t-c-l-start, t-c-l-init');
