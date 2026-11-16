#!/usr/bin/env bash
# Builds the C app regression apps into tests/c/out/<id>/{app.wasm,app.json}.
# Most are built bare: the SDK's clang (tools/cc/fetch.sh's wasi-sdk, else
# emscripten's) with the SDK's WebAssembly features (sdk/c/toolchain/cflags)
# straight against badge_abi.h, without a libc (tlib.h has the little these
# tests need), so each sees exactly the imports it calls and a case can be
# picked with -D. t-c-libc goes through tools/cc/badge-cc like a store app
# (wasi-libc, printf, malloc) when the toolchain is there, as do t-c-bench and
# the SDK's sdk/c/tests/host (id host-probe). gen.ts writes the hand-made
# modules, and wamrc (when the toolchain has it) an AOT file named app.wasm.
# run.ts runs them.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fw="$(cd "$here/../../../.." && pwd)"
out="$here/out"
rm -rf "$out"
mkdir -p "$out"
tc="${SOLANA_CC_TOOLCHAIN:-$fw/tools/cc/.toolchain}"
if [ -x "$tc/wasi-sdk/bin/clang" ]; then
    clang="${CLANG:-$tc/wasi-sdk/bin/clang}"
else
    clang="${CLANG:-$(brew --prefix emscripten)/libexec/llvm/bin/clang}"
fi
features="-mcpu=mvp -mmutable-globals -msign-ext -mnontrapping-fptoint -mbulk-memory -mno-reference-types -mno-multivalue"
cc() {   # cc <id> <src> [defines...]
    local id=$1 src=$2
    shift 2
    mkdir -p "$out/$id"
    "$clang" --target=wasm32 $features -O2 -nostdlib -Wall -Werror -Wno-unused-function -Wno-unused-variable -I"$fw/sdk/c/include" "$@" \
        -Wl,--no-entry -Wl,--export-table -Wl,--allow-undefined -o "$out/$id/app.wasm" "$src"
    if [ -f "$(dirname "$src")/app.json" ]; then
        cp "$(dirname "$src")/app.json" "$out/$id/app.json"
    else
        printf '{"id":"%s","name":"Test: %s","version":"1.0.0","author":"Solana OS","lang":"c","permissions":[]}\n' \
            "$id" "$id" > "$out/$id/app.json"
    fi
}
cc t-c-api "$here/t-c-api/main.c"
cc t-c-import "$here/t-c-import/main.c"
cc t-c-abi "$here/t-c-abi/main.c"
cc t-c-svc "$here/t-c-svc/main.c"
cc t-c-fbench "$here/t-c-fbench/main.c"   # ui_frame timings on a badge (C_API.md "Heavy apps")
cc t-c-fstress "$here/t-c-fstress/main.c"  # frames while the flash is written (on a badge)
# ui_frame and files_read with hostile arguments (2.3.0): the checks, then misuse.
cc t-c-frame "$here/t-c-frame/main.c" -DCASE=0
frames=(pointer side flags big buffer name noperm huge name0 nameread namestraddle)
for i in "${!frames[@]}"; do
    cc "t-c-fr-${frames[$i]}" "$here/t-c-frame/main.c" -DCASE=$((i + 1))
    sed "s/\"t-c-frame\"/\"t-c-fr-${frames[$i]}\"/" "$here/t-c-frame/app.json" > "$out/t-c-fr-${frames[$i]}/app.json"
    if [ "${frames[$i]}" = noperm ]; then
        sed -i.bak 's/"files"//' "$out/t-c-fr-noperm/app.json" && rm -f "$out/t-c-fr-noperm/app.json.bak"
    fi
done
cases=(loop loopcb trapcb grow luamem index pointer oob unreachable div0 stack indirect fail exit pushfn frame unref nullfn shared unshared nest fspin importloop nres)
for i in "${!cases[@]}"; do
    cc "t-c-f-${cases[$i]}" "$here/t-c-fail/main.c" -DCASE=$((i + 1)) -Wno-infinite-recursion
done
# Work that must stay inside a callback's limits (t-c-limits), and modules the
# badge must refuse: hand-made ones (gen.ts) and one exporting __wasm_call_ctors.
limits=(fill copy pushstr getkey fdwrite random log)
for i in "${!limits[@]}"; do
    cc "t-c-l-${limits[$i]}" "$here/t-c-limits/main.c" -DCASE=$((i + 1))
done
cc t-c-l-ctors "$here/t-c-limits/main.c" -DCASE=8 -Wl,--export=__wasm_call_ctors
cc t-c-l-print "$here/t-c-limits/main.c" -DCASE=9
bun "$here/gen.ts" "$out"
# Native code under app.wasm's name: refused before WAMR's loader sees it.
if [ -x "$tc/wamrc" ]; then
    mkdir -p "$out/t-c-l-aotwasm"
    flags=()
    while IFS= read -r l; do flags+=("$l"); done < <(sed 's/#.*//' "$fw/sdk/c/toolchain/wamrcflags" | awk 'NF{$1=$1;print}')
    "$tc/wamrc" "${flags[@]}" -o "$out/t-c-l-aotwasm/app.wasm" "$out/t-c-api/app.wasm" > /dev/null
    printf '{"id":"t-c-l-aotwasm","name":"Test: t-c-l-aotwasm","version":"1.0.0","author":"Solana OS","lang":"c","permissions":[]}\n' \
        > "$out/t-c-l-aotwasm/app.json"
else
    echo "no wamrc (tools/cc/fetch.sh): t-c-l-aotwasm skipped"
fi
if [ -x "$fw/tools/cc/badge-cc" ] && [ -x "$tc/wasi-sdk/bin/clang" ]; then
    mkdir -p "$out/t-c-libc"
    "$fw/tools/cc/badge-cc" "$here/t-c-libc" -o "$out/t-c-libc/app.wasm" -q
    cp "$here/t-c-libc/app.json" "$out/t-c-libc/app.json"
    mkdir -p "$out/t-c-bench"
    "$fw/tools/cc/badge-cc" "$here/t-c-bench" -o "$out/t-c-bench/app.wasm" -q
    cp "$here/t-c-bench/app.json" "$out/t-c-bench/app.json"
    # The SDK's own probe of badge.h's wrappers (sdk/c/tests/host), a store-built app.
    mkdir -p "$out/host-probe"
    "$fw/tools/cc/badge-cc" "$fw/sdk/c/tests/host" -o "$out/host-probe/app.wasm" -q
    cp "$fw/sdk/c/tests/host/app.json" "$out/host-probe/app.json"
    # badge.h's mail wrappers (Mailbox, 2.3.0) on a badge: lua recv + lua run, like tests/t-mail.
    mkdir -p "$out/t-c-mail"
    "$fw/tools/cc/badge-cc" "$here/t-c-mail" -o "$out/t-c-mail/app.wasm" -q
    cp "$here/t-c-mail/app.json" "$out/t-c-mail/app.json"
    # badge.h's profile_items / profile_pages (2.3.0); the server test invemu.test.ts runs it against a server.
    mkdir -p "$out/t-c-inv"
    "$fw/tools/cc/badge-cc" "$here/t-c-inv" -o "$out/t-c-inv/app.wasm" -q
    cp "$here/t-c-inv/app.json" "$out/t-c-inv/app.json"
else
    echo "no SDK toolchain (tools/cc/fetch.sh): t-c-libc, t-c-bench, host-probe, t-c-mail and t-c-inv skipped"
fi
ls "$out" | wc -l | xargs echo "apps built:"
