#!/usr/bin/env python3
"""Host test for core/ed25519.c (crypto.verify, crypto.on_curve).

Builds ed25519.c with a small driver (ed25519_test.c), then checks it against
a pure-Python reference of RFC 8032 with the checks Solana makes of a
signature (ed25519-dalek's verify_strict): s below the group order, neither
the key nor R of small order. Vectors: RFC 8032's, random keys and messages
signed here, every small-order point (and their non-canonical encodings) as a
key and as R, and a "signature" with R the neutral point that verifies under a
plain check for a real key. Also the on-curve test, which must still say yes
to the small-order points.

    python3 main/lua/tests/crypto/ed25519_test.py      (from firmware/)
"""
import hashlib, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.normpath(os.path.join(HERE, "..", "..", ".."))

p = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
d = -121665 * pow(121666, p - 2, p) % p
I = pow(2, (p - 1) // 4, p)

def H(m): return hashlib.sha512(m).digest()

def add(P, Q):
    x1, y1, z1, t1 = P; x2, y2, z2, t2 = Q
    a = (y1 - x1) * (y2 - x2) % p; b = (y1 + x1) * (y2 + x2) % p
    c = t1 * 2 * d * t2 % p; dd = z1 * 2 * z2 % p
    e, f, g, h = b - a, dd - c, dd + c, b + a
    return (e * f % p, g * h % p, f * g % p, e * h % p)

def mul(s, P):
    Q = (0, 1, 1, 0)
    while s:
        if s & 1: Q = add(Q, P)
        P = add(P, P); s >>= 1
    return Q

def eq(P, Q): return (P[0] * Q[2] - Q[0] * P[2]) % p == 0 and (P[1] * Q[2] - Q[1] * P[2]) % p == 0

def recover_x(y, sign):
    if y >= p: return None
    x2 = (y * y - 1) * pow(d * y * y + 1, p - 2, p) % p
    if x2 == 0: return None if sign else 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p: x = x * I % p
    if (x * x - x2) % p: return None
    if (x & 1) != sign: x = p - x
    return x

def decompress(s):
    """dalek-style: a y of p or more counts as y - p; x = 0 with the sign bit is x = 0."""
    y = int.from_bytes(s, "little"); sign = y >> 255; y &= (1 << 255) - 1
    y %= p
    x2 = (y * y - 1) * pow(d * y * y + 1, p - 2, p) % p
    if x2 == 0: x = 0
    else:
        x = pow(x2, (p + 3) // 8, p)
        if (x * x - x2) % p: x = x * I % p
        if (x * x - x2) % p: return None
        if (x & 1) != sign: x = p - x
    return (x, y, 1, x * y % p)

def compress(P):
    zi = pow(P[2], p - 2, p); x = P[0] * zi % p; y = P[1] * zi % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")

G = decompress(bytes.fromhex("5866666666666666666666666666666666666666666666666666666666666666"))

def small_order(P): return eq(mul(8, P), (0, 1, 1, 0))

def secret(seed):
    h = H(seed); a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8; a |= 1 << 254
    return a, h[32:]

def public(seed): return compress(mul(secret(seed)[0], G))

def sign(seed, m):
    a, prefix = secret(seed); A = compress(mul(a, G))
    r = int.from_bytes(H(prefix + m), "little") % L
    R = compress(mul(r, G))
    k = int.from_bytes(H(R + A + m), "little") % L
    return R + int.to_bytes((r + k * a) % L, 32, "little")

def verify_strict(A_b, m, sig):
    if len(A_b) != 32 or len(sig) != 64: return False
    s = int.from_bytes(sig[32:], "little")
    if s >= L: return False
    A = decompress(A_b); R = decompress(sig[:32])
    if A is None or R is None or small_order(A) or small_order(R): return False
    k = int.from_bytes(H(sig[:32] + A_b + m), "little") % L
    neg_A = ((-A[0]) % p, A[1], A[2], (-A[3]) % p)
    return compress(add(mul(s, G), mul(k, neg_A))) == sig[:32]

def verify_plain(A_b, m, sig):
    """The check before 2.1.0's fix: no small-order tests."""
    s = int.from_bytes(sig[32:], "little")
    A = decompress(A_b)
    if s >= L or A is None: return False
    k = int.from_bytes(H(sig[:32] + A_b + m), "little") % L
    neg_A = ((-A[0]) % p, A[1], A[2], (-A[3]) % p)
    return compress(add(mul(s, G), mul(k, neg_A))) == sig[:32]

# The eight small-order points, canonical, then non-canonical encodings of some.
SMALL = [bytes.fromhex(h) for h in [
    "0100000000000000000000000000000000000000000000000000000000000000",  # neutral, order 1
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  # order 2
    "0000000000000000000000000000000000000000000000000000000000000080",  # order 4
    "0000000000000000000000000000000000000000000000000000000000000000",  # order 4 (y = 0, x even)
    "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05",  # order 8
    "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85",
    "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a",
    "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac03fa",
]]
NONCANON = [bytes.fromhex(h) for h in [
    "0100000000000000000000000000000000000000000000000000000000000080",  # neutral, x = -0
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",  # order 2, x = -0
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  # y = p: order 4
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  # y = p + 1: neutral
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
]]

def main():
    work = tempfile.mkdtemp()
    exe = os.path.join(work, "ed25519_test")
    cc = os.environ.get("CC", "cc")
    subprocess.check_call([cc, "-O2", "-Wall", "-Wextra", "-Werror", "-I", MAIN,
                           os.path.join(HERE, "ed25519_test.c"), os.path.join(MAIN, "core", "ed25519.c"), "-o", exe])
    cases = []          # (kind, name, args, want)
    def v(name, A, m, sig, want): cases.append(("v", name, (A, m, sig), want))
    def c(name, P, want): cases.append(("c", name, (P,), want))

    # RFC 8032 tests 1 and 2
    v("rfc 1", bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"), b"",
      bytes.fromhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"), True)
    v("rfc 2", bytes.fromhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"), b"\x72",
      bytes.fromhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"), True)

    rng = random.Random(2026)
    for i in range(200):
        seed = rng.randbytes(32); m = rng.randbytes(rng.choice([0, 1, 32, 100, 1000]))
        A = public(seed); sig = sign(seed, m)
        v(f"random {i}", A, m, sig, True)
        if i % 4 == 0:
            bad = bytearray(sig); bad[rng.randrange(64)] ^= 1 << rng.randrange(8)
            v(f"random {i} flipped", A, m, bytes(bad), verify_strict(A, m, bytes(bad)))
            v(f"random {i} other message", A, m + b"x", sig, False)
            s = int.from_bytes(sig[32:], "little")
            v(f"random {i} s + L", A, m, sig[:32] + int.to_bytes(s + L, 32, "little"), False)

    # Small-order keys: the forgery of the review (key = the neutral point,
    # R = the neutral point, s = 0) and every small-order R with s = 0.
    forged_plain = 0
    for ai, A in enumerate(SMALL + NONCANON):
        for ri, R in enumerate(SMALL + NONCANON):
            for m in (b"", b"hello", b"pay 1000 to me"):
                sig = R + bytes(32)
                forged_plain += verify_plain(A, m, sig)
                v(f"small key {ai} R {ri} '{m.decode()}'", A, m, sig, False)
    assert forged_plain > 0, "the reference's plain check should take some forgeries"
    # A real key, R the neutral point, s = h*a: verifies under a plain check.
    seed = bytes(range(32)); a, _ = secret(seed); A = public(seed)
    for ri, R in enumerate(SMALL[:1] + NONCANON[:1] + NONCANON[4:5]):
        m = b"strict R"
        k = int.from_bytes(H(R + A + m), "little") % L
        sig = R + int.to_bytes(k * a % L, 32, "little")
        if ri == 0:
            assert verify_plain(A, m, sig), "R = neutral should pass a plain check"
        v(f"small R {ri}, real key", A, m, sig, False)
    # A real signature with its key's encoding made non-canonical can't exist
    # (a key's y below p has one encoding); s = L - 1 is the largest allowed.
    # The on-curve test: unchanged, small-order points are on the curve.
    for i, P in enumerate(SMALL):
        c(f"on_curve small {i}", P, True)
    c("on_curve base point", bytes.fromhex("58" + "66" * 31), True)
    c("on_curve y = 2", bytes.fromhex("02" + "00" * 31), False)
    for i in range(100):
        P = rng.randbytes(32)
        c(f"on_curve random {i}", P, decompress(P) is not None)

    lines = []
    for kind, name, args, want in cases:
        if kind == "v":
            A, m, sig = args
            lines.append(f"v {A.hex()} {sig.hex()} {m.hex() or '-'}")
        else:
            lines.append(f"c {args[0].hex()}")
    out = subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True).stdout.split()
    failed = 0
    for (kind, name, args, want), got in zip(cases, out):
        if (got == "1") != want:
            failed += 1
            print(f"FAIL {name}: got {got}, want {int(want)}")
    if len(out) != len(cases):
        failed += 1
        print(f"FAIL: {len(out)} answers for {len(cases)} cases")
    print(f"ed25519: {len(cases)} cases, {failed} failed ({forged_plain} small-order forgeries a plain check takes)")
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
