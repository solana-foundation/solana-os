#!/bin/sh
# fetch.sh: downloads the pinned C app toolchain (sdk/c/toolchain/versions):
# wasi-sdk (clang, wasm-ld, wasi-libc, compiler-rt) and WAMR's wamrc, each
# checked against the SHA-256 below, into tools/cc/.toolchain/ (or
# $SOLANA_CC_TOOLCHAIN):
#
#   .toolchain/wasi-sdk/bin/clang      .toolchain/wasi-sdk/share/wasi-sysroot
#   .toolchain/wamrc                   (on macOS a wrapper; see below)
#
# Platforms: macOS on Apple Silicon and Linux x86_64 (the server's Docker
# image runs it at build time). WAMR publishes wamrc for x86_64 only: on a Mac
# it runs under Rosetta 2 (softwareupdate --install-rosetta) and needs an
# x86_64 libzstd, taken from Homebrew's zstd 1.5.7 bottle (pinned, checked).
#
# Idempotent: what is already there at the pinned version is kept. POSIX sh,
# curl, tar (gzip) and sha256sum or shasum.
#
# Licences: wasi-sdk is Apache-2.0 WITH LLVM-exception (wasi-libc: MIT/
# Apache-2.0/BSD; see its share/ folders), WAMR Apache-2.0 WITH
# LLVM-exception, zstd BSD-3-Clause. Nothing downloaded is committed.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
versions="$here/../../sdk/c/toolchain/versions"
tc=${SOLANA_CC_TOOLCHAIN:-$here/.toolchain}

val() { sed -n "s/^$1=//p" "$versions"; }
WASI_SDK=$(val WASI_SDK)
WAMR=$(val WAMR)
[ "$WASI_SDK" = 24.0 ] && [ "$WAMR" = 2.4.5 ] || {
    echo "fetch.sh: versions says wasi-sdk $WASI_SDK and WAMR $WAMR; this script pins 24.0 and 2.4.5: update both together" >&2
    exit 1
}

# --- what to fetch, pinned ------------------------------------------------
WASI_URL=https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-24
WAMR_URL=https://github.com/bytecodealliance/wasm-micro-runtime/releases/download/WAMR-2.4.5
os=$(uname -s)
arch=$(uname -m)
case "$os/$arch" in
Darwin/arm64)
    wasi_name=wasi-sdk-24.0-arm64-macos
    wasi_sha=aeae999396d5f5caa5ce419f52e83c35869d5fd21d40af80acba2c80f51b0b3a
    wamrc_name=wamrc-2.4.5-x86_64-macos-15-intel.tar.gz
    wamrc_sha=419d41b8b9b29bb8bb0e86e0409725abab81aa693daa479e502fadd0c206db69
    zstd_url=https://ghcr.io/v2/homebrew/core/zstd/blobs/sha256:8b8656acd6f30bcbbb9a033ae840afea299c9f0852f71b7540492b0fe7a36742
    zstd_sha=8b8656acd6f30bcbbb9a033ae840afea299c9f0852f71b7540492b0fe7a36742
    ;;
Linux/x86_64 | Linux/amd64)
    wasi_name=wasi-sdk-24.0-x86_64-linux
    wasi_sha=c6c38aab56e5de88adf6c1ebc9c3ae8da72f88ec2b656fb024eda8d4167a0bc5
    wamrc_name=wamrc-2.4.5-x86_64-ubuntu-22.04.tar.gz
    wamrc_sha=fedda99950a70f7dc45470ce88e12bf9e435b9482058ae97ce2cadd760e2329f
    ;;
*)
    echo "fetch.sh: no pinned toolchain for $os $arch (macOS arm64 and Linux x86_64 only)" >&2
    exit 1
    ;;
esac

# --- helpers --------------------------------------------------------------
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# get <url> <file> <sha256> [curl args...]: downloads and checks, or exits.
get() {
    url=$1 out=$2 want=$3
    shift 3
    echo "fetch.sh: downloading $url" >&2
    curl -fL --retry 3 --proto '=https' -sS "$@" -o "$out" "$url"
    got=$(sha256 "$out")
    if [ "$got" != "$want" ]; then
        rm -f "$out"
        echo "fetch.sh: $url has SHA-256 $got, expected $want: refusing it" >&2
        exit 1
    fi
}

mkdir -p "$tc"
dl="$tc/.download"
rm -rf "$dl"
mkdir -p "$dl"
trap 'rm -rf "$dl"' EXIT INT TERM

# --- wasi-sdk ---------------------------------------------------------------
# Only what building a C app needs: clang (with its libraries), wasm-ld,
# llvm-nm/llvm-objdump, the wasm32-wasip1 sysroot and compiler-rt. About a
# third of the release's size.
stamp="$wasi_name $wasi_sha"
if [ "$(cat "$tc/wasi-sdk/.pinned" 2>/dev/null)" != "$stamp" ]; then
    get "$WASI_URL/$wasi_name.tar.gz" "$dl/wasi-sdk.tar.gz" "$wasi_sha"
    p=$wasi_name
    mkdir -p "$dl/x"
    tar -xzf "$dl/wasi-sdk.tar.gz" -C "$dl/x" \
        "$p/bin/clang" "$p/bin/clang-18" "$p/bin/lld" "$p/bin/wasm-ld" \
        "$p/bin/llvm-nm" "$p/bin/llvm-objdump" "$p/lib" \
        "$p/share/wasi-sysroot/include/wasm32-wasip1" "$p/share/wasi-sysroot/lib/wasm32-wasip1"
    rm -rf "$tc/wasi-sdk"
    mv "$dl/x/$p" "$tc/wasi-sdk"
    "$tc/wasi-sdk/bin/clang" --version >/dev/null
    echo "$stamp" >"$tc/wasi-sdk/.pinned"
fi

# --- wamrc ------------------------------------------------------------------
stamp="$wamrc_name $wamrc_sha"
if [ "$(cat "$tc/wamr/.pinned" 2>/dev/null)" != "$stamp" ]; then
    get "$WAMR_URL/$wamrc_name" "$dl/wamrc.tar.gz" "$wamrc_sha"
    rm -rf "$tc/wamr" "$tc/wamrc"
    mkdir -p "$tc/wamr"
    tar -xzf "$dl/wamrc.tar.gz" -C "$tc/wamr" wamrc
    if [ "$os" = Darwin ]; then
        # The release links /usr/local/opt/zstd/lib/libzstd.1.dylib (an Intel
        # Homebrew path): a copy beside it, found through the fallback path.
        get "$zstd_url" "$dl/zstd.tar.gz" "$zstd_sha" -H "Authorization: Bearer QQ=="
        mkdir -p "$tc/wamr/lib"
        tar -xzf "$dl/zstd.tar.gz" -C "$dl" zstd/1.5.7_1/lib/libzstd.1.5.7.dylib
        mv "$dl/zstd/1.5.7_1/lib/libzstd.1.5.7.dylib" "$tc/wamr/lib/libzstd.1.dylib"
        # /bin/sh drops DYLD_* from its own environment, so the wrapper sets
        # it, then runs the x86_64 binary (Rosetta), never through arch(1).
        cat >"$tc/wamrc" <<'EOF'
#!/bin/sh
d=$(cd "$(dirname "$0")" && pwd)
DYLD_FALLBACK_LIBRARY_PATH="$d/wamr/lib:/usr/lib" exec "$d/wamr/wamrc" "$@"
EOF
        chmod +x "$tc/wamrc"
    else
        ln -s wamr/wamrc "$tc/wamrc"
    fi
    if ! "$tc/wamrc" --version >/dev/null 2>&1; then
        if [ "$os" = Darwin ]; then
            echo "fetch.sh: wamrc doesn't start; it is an x86_64 program: install Rosetta 2 (softwareupdate --install-rosetta)" >&2
        else
            echo "fetch.sh: wamrc doesn't start (it needs glibc 2.34+, libstdc++6, zlib and libzstd1)" >&2
        fi
        exit 1
    fi
    echo "$stamp" >"$tc/wamr/.pinned"
fi

echo "fetch.sh: toolchain ready in $tc ($("$tc/wasi-sdk/bin/clang" --version | head -n 1); $("$tc/wamrc" --version))"
