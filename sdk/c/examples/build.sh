#!/bin/sh
# Builds every example (and the probes in ../tests/api, ../tests/libc and
# ../tests/host) with badge-cc into build/<name>.wasm, plus build/<name>.aot when
# wamrc is there (checking only: a badge takes native code from the store alone).
# Then runs each module on the mock host when bun is installed
# (tests/mock-host.ts: a fake badge, not one), except tests/host, whose checks
# are of a real host's answers (run it in the emulator or on a badge).
#
#   sh examples/build.sh [--no-aot] [--no-mock]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cc="$here/../../../tools/cc/badge-cc"
tc=${SOLANA_CC_TOOLCHAIN:-$here/../../../tools/cc/.toolchain}
out="$here/build"
aot=1 mock=1
for a in "$@"; do
    case "$a" in
    --no-aot) aot= ;;
    --no-mock) mock= ;;
    *) echo "usage: build.sh [--no-aot] [--no-mock]" >&2; exit 1 ;;
    esac
done
[ -x "$tc/wamrc" ] || aot=
command -v bun >/dev/null 2>&1 || mock=
mkdir -p "$out"

failed=0
for dir in "$here"/*/ "$here/../tests/api/" "$here/../tests/libc/" "$here/../tests/host/"; do
    [ -f "$dir/main.c" ] || continue
    name=$(basename "$dir")
    set -- "$dir" -o "$out/$name.wasm"
    [ -z "$aot" ] || set -- "$@" --aot "$out/$name.aot"
    if ! "$cc" "$@"; then
        failed=1
        continue
    fi
    [ "$name" != host ] || continue
    if [ -n "$mock" ] && ! bun "$here/../tests/mock-host.ts" "$out/$name.wasm" --quiet; then
        failed=1
    fi
done
exit $failed
