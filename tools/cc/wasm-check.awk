# wasm-check.awk: checks a C app's WebAssembly module against what Solana OS
# runs (docs/C_API.md "How it is compiled"). POSIX awk, fed the module's bytes
# as decimal numbers:
#
#   od -An -v -tu1 app.wasm | awk -f wasm-check.awk -v name=app.wasm \
#       -v imports=sdk/c/toolchain/imports [-v maxpages=64] [-v report=1]
#
# It decodes every section and every instruction, and refuses:
#   - an import that isn't in the `imports` file (one module.name a line),
#     or anything imported but functions;
#   - WebAssembly features beyond MVP + bulk memory, sign extension,
#     non-trapping float-to-int and mutable globals (WAMR 2.4.5 in a small
#     embedded build): SIMD, threads/atomics, reference types (also an overlong
#     call_indirect table index, which newer wasi-libc builds emit), multi-value,
#     exceptions, tail calls, extended constants, memory64, multi-memory, GC;
#   - a module without one memory whose maximum is <= maxpages (64 = 4 MB),
#     a start section, a `_start` export (command model), or missing exports
#     (on_start, _initialize, memory, __indirect_function_table).
#
# Errors go to stdout as "<name>: error: ..." and the exit status is 1. With
# report=1 it also prints "imports: ...", "exports: ...", "features: ...",
# "memory: ..." lines. The server doesn't run this file: it has a TypeScript
# port of these checks (wasmProblems() in server/src/cc.ts). Change both
# together, so a module badge-cc passes is one the store accepts.

function err(msg) { print name ": error: " msg; nerr++ }

function u8() {
    if (P >= N) { if (!trunc) err("the module ends early (truncated or not WebAssembly)"); trunc = 1; P = N + 1; return 0 }
    return B[P++]
}

function leb(   r, s, x) {      # unsigned LEB128 (u32/u64: values stay exact up to 2^53)
    r = 0; s = 1
    do { x = u8(); r += (x % 128) * s; s *= 128 } while (x >= 128 && P <= N)
    return r
}

function sleb(   x) {            # signed LEB128: only skipped
    do { x = u8() } while (x >= 128 && P <= N)
}

function str(   n, s, i) {       # a name: bytes as characters (ASCII is all we compare)
    n = leb(); s = ""
    for (i = 0; i < n && P < N; i++) s = s sprintf("%c", B[P++])
    return s
}

function use(f) { USED[f] = 1 }
function bad(f, what) { if (!(f in BAD)) { BAD[f] = 1; err("uses " what " (" f "), which the badge doesn't run") } }

function valtype(t) {
    if (t >= 124 && t <= 127) return 1                  # i32 i64 f32 f64
    if (t == 123) { bad("simd128", "SIMD (v128)"); return 0 }
    if (t == 112 || t == 111) { bad("reference-types", "reference types"); return 0 }
    err("unknown value type " t); return 0
}

function blocktype(   t) {
    t = B[P]
    if (t == 64) { P++; return }                         # empty
    if (t >= 111 && t <= 127) { P++; valtype(t); return }
    sleb(); bad("multivalue", "multi-value blocks")      # a type index
}

function limits(what,   f, min, max) {
    f = leb(); min = leb(); max = -1
    if (f == 1 || f == 3 || f == 5 || f == 7) max = leb()
    if (f >= 2 && f <= 3) bad("threads", "shared memory")
    if (f >= 4) bad("memory64", "64-bit memory")
    LMIN = min; LMAX = max
}

function constexpr(   op, n) {   # init expressions: one constant, then end
    n = 0
    while (P < N) {
        op = u8()
        if (op == 11) { if (n == 0) err("empty constant expression"); return }
        n++
        if (op == 65 || op == 66) sleb()
        else if (op == 67) P += 4
        else if (op == 68) P += 8
        else if (op == 35) leb()
        else if (op == 208) { u8(); bad("reference-types", "reference types") }
        else if (op == 210) { leb(); bad("reference-types", "reference types") }
        else if (op == 106 || op == 107 || op == 108 || op == 124 || op == 125 || op == 126) bad("extended-const", "extended constant expressions")
        else { err("unsupported instruction " op " in a constant expression"); return }
    }
}

function memarg(   a) {
    a = leb()
    if (a >= 64 && a < 128) { leb(); bad("multi-memory", "several memories") }
    leb()
}

# One function body, [P, end). Returns early (P = end) after an instruction it
# can't decode, having reported it.
function body(end,   n, i, op, so, k) {
    n = leb()
    for (i = 0; i < n; i++) { leb(); valtype(u8()) }
    while (P < end) {
        op = u8()
        if (op <= 1 || op == 5 || op == 11 || op == 15 || op == 26 || op == 27) continue
        if (op >= 2 && op <= 4) { blocktype(); continue }
        if (op == 12 || op == 13 || op == 16) { leb(); continue }
        if (op == 14) { k = leb(); for (; k >= 0; k--) leb(); continue }
        if (op == 17) {                                  # call_indirect typeidx table
            leb()
            if (B[P] != 0) { if (B[P] >= 128) bad("call-indirect-overlong", "an overlong call_indirect table index (reference types)"); else bad("reference-types", "a second table") }
            leb(); continue
        }
        if (op >= 32 && op <= 36) { leb(); continue }
        if (op >= 40 && op <= 62) { memarg(); continue }
        if (op == 63 || op == 64) { if (B[P] != 0) bad("multi-memory", "several memories"); leb(); continue }
        if (op == 65 || op == 66) { sleb(); continue }
        if (op == 67) { P += 4; continue }
        if (op == 68) { P += 8; continue }
        if (op >= 69 && op <= 191) continue              # MVP numeric
        if (op >= 192 && op <= 196) { use("sign-ext"); continue }
        if (op == 252) {
            so = leb()
            if (so <= 7) { use("nontrapping-fptoint"); continue }
            if (so == 8) { leb(); u8(); use("bulk-memory"); continue }
            if (so == 9) { leb(); use("bulk-memory"); continue }
            if (so == 10) { u8(); u8(); use("bulk-memory"); continue }
            if (so == 11) { u8(); use("bulk-memory"); continue }
            if (so >= 12 && so <= 17) bad("reference-types", "table instructions")
            else err("unknown instruction 0xfc " so)
            P = end; return
        }
        if (op == 253) { bad("simd128", "SIMD"); P = end; return }
        if (op == 254) { bad("threads", "atomics"); P = end; return }
        if (op >= 6 && op <= 10 || op == 24 || op == 25 || op == 31) { bad("exception-handling", "exceptions"); P = end; return }
        if (op == 18 || op == 19) { bad("tail-call", "tail calls"); P = end; return }
        if (op == 20 || op == 21) { bad("gc", "typed function references"); P = end; return }
        if (op == 28 || op == 37 || op == 38 || op >= 208 && op <= 214) { bad("reference-types", "reference types"); P = end; return }
        err(sprintf("unknown opcode 0x%02x", op)); P = end; return
    }
    if (P != end) err("a function body overruns its size")
}

BEGIN {
    if (name == "") name = "app.wasm"
    if (maxpages == "") maxpages = 64
    if (imports != "") {
        while ((getline line < imports) > 0) {
            sub(/#.*/, "", line); gsub(/[ \t\r]/, "", line)
            if (line != "") ALLOW[line] = 1
        }
        close(imports)
    }
    N = 0
}

{ for (i = 1; i <= NF; i++) B[N++] = $i + 0 }

END {
    if (N < 8 || B[0] != 0 || B[1] != 97 || B[2] != 115 || B[3] != 109) { err("not a WebAssembly module"); exit 1 }
    if (B[4] != 1 || B[5] != 0 || B[6] != 0 || B[7] != 0) { err("not a WebAssembly 1.0 module (a component?)"); exit 1 }
    P = 8; nimpf = 0; nmem = 0; ntab = 0; imps = ""; exps = ""
    while (P < N) {
        id = u8(); size = leb(); end = P + size
        if (end > N) { err("section " id " runs past the end of the module"); break }
        if (id == 0) { cname = str(); if (cname == "target_features" || cname == "name" || cname == "producers") CUSTOM = CUSTOM " " cname }
        else if (id == 1) {
            n = leb()
            for (i = 0; i < n && P < end; i++) {
                if (u8() != 96) { err("unsupported type form"); break }
                k = leb(); for (j = 0; j < k; j++) valtype(u8())
                k = leb(); for (j = 0; j < k; j++) valtype(u8())
                if (k > 1) bad("multivalue", "functions with several results")
            }
        } else if (id == 2) {
            n = leb()
            for (i = 0; i < n && P < end; i++) {
                mod = str(); fld = str(); kind = u8(); full = mod "." fld
                imps = imps " " full
                if (kind == 0) { leb(); nimpf++; if (imports != "" && !(full in ALLOW)) err("imports " full ", which the badge doesn't provide" (mod == "wasi_snapshot_preview1" ? " (a libc function the badge has no use for: files, sockets, sleep, ...)" : "")) }
                else if (kind == 1) { u8(); limits(); err("imports a table (" full ")") }
                else if (kind == 2) { limits(); err("imports its memory (" full "): the module must define it") }
                else if (kind == 3) { u8(); u8(); err("imports a global (" full ")") }
                else { err("imports " full " of kind " kind); P = end }
            }
        } else if (id == 3) { n = leb(); for (i = 0; i < n; i++) leb() }
        else if (id == 4) {
            n = leb(); ntab += n
            for (i = 0; i < n && P < end; i++) { t = u8(); if (t != 112) bad("reference-types", "a table of references"); limits() }
            if (ntab > 1) bad("reference-types", "several tables")
        } else if (id == 5) {
            n = leb(); nmem += n
            for (i = 0; i < n && P < end; i++) {
                limits(); MEMMIN = LMIN; MEMMAX = LMAX
            }
            if (nmem > 1) bad("multi-memory", "several memories")
        } else if (id == 6) {
            n = leb()
            for (i = 0; i < n && P < end; i++) { valtype(u8()); mut = u8(); constexpr() }
        } else if (id == 7) {
            n = leb()
            for (i = 0; i < n && P < end; i++) {
                ename = str(); kind = u8(); idx = leb()
                EXP[ename] = kind; exps = exps " " ename
            }
        } else if (id == 8) { leb(); err("has a start function: build it as a reactor (-mexec-model=reactor)") }
        else if (id == 9) {
            n = leb()
            for (i = 0; i < n && P < end; i++) {
                f = leb()
                if (f != 0) { bad("reference-types", "element segments of kind " f); P = end; break }
                constexpr(); k = leb(); for (j = 0; j < k; j++) leb()
            }
        } else if (id == 10) {
            n = leb()
            for (i = 0; i < n && P < end; i++) { s = leb(); body(P + s) }
        } else if (id == 11) {
            n = leb()
            for (i = 0; i < n && P < end; i++) {
                f = leb()
                if (f == 1) { use("bulk-memory"); k = leb(); P += k; continue }
                if (f == 2) { if (leb() != 0) bad("multi-memory", "several memories") }
                else if (f != 0) { err("unknown data segment kind " f); P = end; break }
                constexpr(); k = leb(); P += k
            }
        } else if (id == 12) { leb(); use("bulk-memory") }
        else if (id == 13) bad("exception-handling", "exception tags")
        else err("unknown section " id)
        if (P != end && !trunc) { err("section " id " has " (end - P) " bytes it doesn't account for"); }
        P = end
    }

    if (nmem != 1) err("must define one memory")
    else if (MEMMAX < 0) err("its memory has no maximum (link with --max-memory=" maxpages * 65536 ")")
    else if (MEMMAX > maxpages) err("its memory may grow to " MEMMAX * 64 " KB, over the badge's " maxpages * 64 " KB")
    if (!("on_start" in EXP) || EXP["on_start"] != 0) err("doesn't export on_start (define void on_start(void) and include badge.h)")
    if (!("_initialize" in EXP) || EXP["_initialize"] != 0) err("doesn't export _initialize (link with -mexec-model=reactor)")
    if ("_start" in EXP) err("exports _start: build it as a reactor (-mexec-model=reactor), with no main()")
    if (!("memory" in EXP) || EXP["memory"] != 2) err("doesn't export its memory")
    if (!("__indirect_function_table" in EXP) || EXP["__indirect_function_table"] != 1) err("doesn't export its function table (link with --export-table)")

    if (report) {
        print "imports:" imps
        print "exports:" exps
        fl = "mvp"
        for (f in USED) fl = fl " " f
        print "features:" " " fl
        print "memory: " MEMMIN * 64 " KB initial, " (MEMMAX < 0 ? "no maximum" : MEMMAX * 64 " KB maximum")
    }
    exit (nerr > 0 ? 1 : 0)
}
