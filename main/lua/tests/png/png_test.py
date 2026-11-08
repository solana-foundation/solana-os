#!/usr/bin/env python3
"""Host test for the Lua apps' image decoders (lua/lua_png.c, lua/lua_gif.c):
writes PNGs in every colour type and bit depth and animated GIFs with Pillow,
decodes them with the C code and compares each pixel (each frame, composed)
with Pillow's own reading. Also feeds them broken files, which must fail
cleanly (built with -fsanitize=address,undefined).

    python3 main/lua/tests/png/png_test.py      (from firmware/; system python3, Pillow)
"""
import io, os, random, struct, subprocess, sys, tempfile, zlib
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CLEAR = 0x0001


def rgb565(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return 0 if v == CLEAR else v


def expected(im):
    """What the badge should show, from Pillow's RGBA reading."""
    rgba = im.convert("RGBA")
    return [CLEAR if a < 128 else rgb565(r, g, b) for (r, g, b, a) in rgba.getdata()]


def build(tmp):
    exe = os.path.join(tmp, "png")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=undefined",  # a UBSan report fails the run, not just prints
                    "-o", exe, os.path.join(HERE, "main.c"), os.path.join(HERE, "..", "..", "lua_png.c"),
                    os.path.join(HERE, "..", "..", "lua_gif.c")], check=True)
    return exe


def decode(exe, data, tmp):
    p = os.path.join(tmp, "in.png")
    with open(p, "wb") as f:
        f.write(data)
    out = subprocess.run([exe, p], capture_output=True, text=True)
    if out.returncode:
        raise SystemExit(f"decoder crashed:\n{out.stderr[-2000:]}")
    lines = out.stdout.split("\n")
    if lines[0].startswith("ERR"):
        return lines[0][4:], None
    w, h = map(int, lines[0].split())
    return (w, h), [int(x, 16) for x in lines[1:1 + w * h]]


def sample_image(w, h, seed):
    rnd = random.Random(seed)
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if rnd.random() < 0.5:   # runs, so the encoder finds matches
                px[x, y] = ((x * 7) & 255, (y * 13) & 255, ((x + y) * 5) & 255, 255 if (x + y) % 5 else 0)
            else:
                px[x, y] = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.choice((0, 90, 200, 255)))
    return im


def png_bytes(im, **kw):
    b = io.BytesIO()
    im.save(b, "PNG", **kw)
    return b.getvalue()


def cases():
    base = sample_image(37, 23, 1)
    yield "RGBA 8", base, {}
    yield "RGB 8", base.convert("RGB"), {}
    yield "LA 8", base.convert("LA"), {}
    yield "L 8", base.convert("L"), {}
    yield "1-bit", base.convert("1"), {}
    for bits in (1, 2, 4, 8):
        yield f"P {bits}-bit", base.convert("RGB").quantize(colors=2 ** bits), {"bits": bits}
    p = base.quantize(colors=16)
    yield "P + tRNS", p, {"transparency": 3}
    yield "RGB + tRNS", base.convert("RGB"), {"transparency": base.convert("RGB").getpixel((0, 0))}
    yield "L + tRNS", base.convert("L"), {"transparency": base.convert("L").getpixel((1, 0))}
    yield "no compression", base, {"compress_level": 0}
    yield "best compression", sample_image(120, 90, 2), {"compress_level": 9}
    yield "1x1", Image.new("RGBA", (1, 1), (20, 241, 149, 255)), {}
    yield "flat", Image.new("RGB", (200, 150), (153, 69, 255)), {}


def raw_png(w, h, depth, color, rows, extra=b"", interlace=0):
    """A PNG built by hand (for 16-bit RGB/RGBA, which Pillow won't write)."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, interlace)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + extra + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def raw_gif(w, h, palette, frames, bg=0):
    """A GIF built by hand: frames are (x, y, fw, fh, indices, disposal, transparent or None).
    LZW without compression: literal codes, a clear before the table would grow."""
    def lzw(idx, min_size=8):
        clear, size, out, bits, n = 1 << min_size, min_size + 1, bytearray(), 0, 0
        codes = [clear]
        for k, v in enumerate(idx):
            if k and k % (clear - 3) == 0:
                codes.append(clear)
            codes.append(v)
        codes.append(clear + 1)
        for c in codes:
            bits |= c << n
            n += size
            while n >= 8:
                out.append(bits & 255)
                bits >>= 8
                n -= 8
        if n:
            out.append(bits & 255)
        blocks = b"".join(bytes([len(out[i:i + 255])]) + out[i:i + 255] for i in range(0, len(out), 255))
        return bytes([min_size]) + blocks + b"\x00"
    pal = bytes(palette) + bytes(768 - len(palette))
    data = b"GIF89a" + struct.pack("<HHBBB", w, h, 0xF7, bg, 0) + pal
    for x, y, fw, fh, idx, disposal, transparent in frames:
        flags = (disposal << 2) | (1 if transparent is not None else 0)
        data += b"\x21\xf9\x04" + struct.pack("<BHB", flags, 5, transparent or 0) + b"\x00"
        data += b"\x2c" + struct.pack("<HHHHB", x, y, fw, fh, 0) + lzw(idx)
    return data + b"\x3b"


def decode_gif(exe, data, tmp):
    """(w, h), [(delay, pixels)...], loops; or the error and None."""
    p = os.path.join(tmp, "in.gif")
    with open(p, "wb") as f:
        f.write(data)
    out = subprocess.run([exe, p], capture_output=True, text=True)
    if out.returncode:
        raise SystemExit(f"decoder crashed:\n{out.stderr[-2000:]}")
    lines = out.stdout.split("\n")
    if lines[0].startswith("ERR"):
        return lines[0][4:], None, None
    _, w, h = lines[0].split()
    w, h = int(w), int(h)
    frames, i, loops = [], 1, None
    while i < len(lines) and lines[i]:
        if lines[i].startswith("ERR"):
            return lines[i][4:], None, None
        if lines[i].startswith("LOOPS"):
            loops = int(lines[i].split()[1])
            break
        delay = int(lines[i].split()[1])
        frames.append((delay, [int(x, 16) for x in lines[i + 1:i + 1 + w * h]]))
        i += 1 + w * h
    return (w, h), frames, loops


def gif_frames_expected(data):
    im = Image.open(io.BytesIO(data))
    out = []
    for n in range(getattr(im, "n_frames", 1)):
        im.seek(n)
        out.append(expected(im))
    return out


def gif_cases():
    rnd = random.Random(7)
    # A ball bouncing over a striped background, 6 frames, full canvas each.
    frames = []
    for n in range(6):
        f = Image.new("RGB", (40, 30), (20, 20, 30))
        d = ImageDraw.Draw(f)
        for x in range(0, 40, 8):
            d.rectangle([x, 0, x + 3, 29], fill=(153, 69, 255))
        d.ellipse([n * 5, 5 + (n % 3) * 4, n * 5 + 10, 15 + (n % 3) * 4], fill=(20, 241, 149))
        frames.append(f)
    yield "full frames", frames, {"duration": 80, "loop": 0}
    # Noise: long LZW strings and resets (more than 4096 codes).
    noise = [Image.frombytes("RGB", (90, 70), bytes(rnd.randrange(256) for _ in range(90 * 70 * 3))) for _ in range(3)]
    yield "noise", noise, {"duration": [30, 10, 200], "loop": 2}
    # Transparency and disposal 2 (restore to background): a sprite moving on nothing.
    sprites = []
    for n in range(5):
        f = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
        ImageDraw.Draw(f).rectangle([n * 5, n * 4, n * 5 + 8, n * 4 + 8], fill=(235, 84, 188, 255))
        sprites.append(f)
    yield "transparent, disposal 2", sprites, {"duration": 50, "loop": 0, "disposal": 2}
    yield "transparent, disposal 1", sprites, {"duration": 50, "disposal": 1}
    yield "one frame", [frames[0]], {}
    yield "interlaced", frames[:2], {"interlace": True, "duration": 100}
    yield "not interlaced", frames, {"interlace": False, "duration": 100}
    yield "noise, not interlaced", noise, {"interlace": False}
    yield "loop count over 32767", frames[:2], {"duration": 50, "loop": 40000}


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        for name, im, kw in cases():
            data = png_bytes(im, **kw)
            size, px = decode(exe, data, tmp)
            want = expected(Image.open(io.BytesIO(data)))
            ok = px is not None and size == im.size and px == want
            if not ok:
                fails += 1
                bad = next((i for i, (a, b) in enumerate(zip(px or [], want)) if a != b), None)
                print(f"FAIL {name}: {size} first difference at {bad}")
            else:
                print(f"ok   {name} ({im.size[0]}x{im.size[1]}, {len(data)} bytes)")

        # 16-bit RGBA by hand: the high byte of each sample counts.
        rnd = random.Random(3)
        w, h = 9, 5
        pix = [[tuple(rnd.randrange(65536) for _ in range(4)) for _ in range(w)] for _ in range(h)]
        rows = b"".join(b"\x00" + b"".join(struct.pack(">HHHH", *p) for p in r) for r in pix)
        size, px = decode(exe, raw_png(w, h, 16, 6, rows), tmp)
        want = [CLEAR if (a >> 8) < 128 else rgb565(r >> 8, g >> 8, b >> 8) for row in pix for (r, g, b, a) in row]
        print("ok   RGBA 16" if px == want else "FAIL RGBA 16")
        fails += px != want

        # 16-bit grey (Pillow reads it clamped, not scaled): the high byte.
        grey = [[rnd.randrange(65536) for _ in range(w)] for _ in range(h)]
        rows = b"".join(b"\x00" + b"".join(struct.pack(">H", v) for v in r) for r in grey)
        size, px = decode(exe, raw_png(w, h, 16, 0, rows), tmp)
        want = [rgb565(v >> 8, v >> 8, v >> 8) for r in grey for v in r]
        print("ok   grey 16" if px == want else "FAIL grey 16")
        fails += px != want

        for name, ims, kw in gif_cases():
            b = io.BytesIO()
            ims[0].save(b, "GIF", save_all=len(ims) > 1, append_images=ims[1:], **kw)
            data = b.getvalue()
            size, frames, loops = decode_gif(exe, data, tmp)
            want = gif_frames_expected(data)
            ok = frames is not None and len(frames) == len(want) and all(f[1] == w for f, w in zip(frames, want))
            if not ok:
                fails += 1
                print(f"FAIL GIF {name}: {size}, {len(frames or [])} frames, want {len(want)}")
                for k, (f, w) in enumerate(zip(frames or [], want)):
                    bad = [i for i, (a, b2) in enumerate(zip(f[1], w)) if a != b2]
                    if bad:
                        print(f"     frame {k}: {len(bad)} pixels differ, first {bad[0]}: {f[1][bad[0]]:04x} want {w[bad[0]]:04x}")
            elif "loop" in kw and kw["loop"] and loops != kw["loop"]:
                fails += 1
                print(f"FAIL GIF {name}: loops {loops}, want {kw['loop']}")
            else:
                print(f"ok   GIF {name} ({size[0]}x{size[1]}, {len(frames)} frames, delays {[f[0] for f in frames]}, loops {loops}, {len(data)} bytes)")

        # Hand-built: an opaque GIF whose small frames go back to the background colour (disposal 2),
        # with the last frame's disposal 2 never applied (it stays); and a transparent one, which clears.
        pal = [20, 20, 30, 235, 84, 188, 100, 200, 50]
        for name, transparent in (("opaque", None), ("transparent", 2)):
            fr = [(0, 0, 24, 16, [0] * 384, 1, None)] + [(n * 5, 4, 6, 6, [1] * 36, 2, transparent) for n in range(4)]
            data = raw_gif(24, 16, pal, fr)
            size, frames, _ = decode_gif(exe, data, tmp)
            want = gif_frames_expected(data)
            back = rgb565(20, 20, 30) if transparent is None else CLEAR
            # Pillow shows a cleared area of a transparent frame as its transparent index, opaque; browsers
            # (and the badge) clear it, so only the opaque one is compared with Pillow pixel by pixel.
            ok = frames is not None and len(frames) == 5 and (transparent is not None or all(f[1] == w for f, w in zip(frames, want)))
            ok = ok and frames[2][1][4 * 24] == back and frames[-1][1][4 * 24 + 15] == rgb565(235, 84, 188)
            print(f"ok   GIF {name} sprite, disposal 2" if ok else f"FAIL GIF {name} sprite, disposal 2")
            fails += not ok

        # Broken files fail with a message, never a crash (ASan/UBSan watch).
        good = png_bytes(sample_image(30, 20, 4))
        bad_cases = {
            "empty": b"",
            "not png": b"GIF89a" + b"\x00" * 40,
            "truncated": good[:len(good) // 2],
        }
        bad_cases["interlaced"] = raw_png(2, 2, 8, 0, b"\x00" * 6, interlace=1)
        bad_cases["huge"] = raw_png(5000, 1, 8, 0, b"\x00" * 5001)
        bad_cases["short data"] = raw_png(10, 10, 8, 2, b"\x00" * 50)
        bad_cases["long data"] = raw_png(2, 2, 8, 0, b"\x00" * 40)
        bad_cases["bad filter"] = raw_png(2, 2, 8, 0, b"\x07\x00\x00\x07\x00\x00")
        bad_cases["no palette"] = raw_png(2, 2, 8, 3, b"\x00\x00\x00\x00\x00\x00")
        b = io.BytesIO()
        ims = list(gif_cases())[1][1]
        ims[0].save(b, "GIF", save_all=True, append_images=ims[1:])
        good_gif = b.getvalue()
        bad_cases["gif truncated"] = good_gif[:len(good_gif) // 2]
        bad_cases["gif header only"] = good_gif[:13]
        # A frame wider than the canvas (65535 x 65535 on a tiny screen) is refused, not decoded.
        big = raw_gif(4, 4, [255, 0, 0], [(0, 0, 65535, 65535, [0] * 16, 0, None)])
        msg, px, _ = decode_gif(exe, big, tmp)
        if px is not None or msg != "a frame is bigger than the image":
            fails += 1
            print(f"FAIL GIF oversized frame: {msg}")
        else:
            print("ok   GIF oversized frame refused")
        rnd = random.Random(6)
        for i in range(300):
            b = bytearray(good_gif)
            for _ in range(rnd.randrange(1, 8)):
                b[rnd.randrange(13, len(b))] = rnd.randrange(256)
            bad_cases[f"fuzz gif {i}"] = bytes(b)
        for i in range(300):
            b = bytearray(good)
            for _ in range(rnd.randrange(1, 8)):
                b[rnd.randrange(33, len(b))] = rnd.randrange(256)
            bad_cases[f"fuzz {i}"] = bytes(b)
        errs = 0
        for name, data in bad_cases.items():
            if "gif" in name:
                size, px, _ = decode_gif(exe, data, tmp)
                errs += px is None
                continue
            size, px = decode(exe, data, tmp)
            if not name.startswith("fuzz") and px is not None:
                print(f"FAIL {name}: decoded")
                fails += 1
            errs += px is None
        print(f"ok   {len(bad_cases)} broken files ({errs} refused, the rest decoded wrong pixels but stayed in bounds)")
        msg = decode(exe, bad_cases["interlaced"], tmp)[0]
        print(f"     interlaced says: {msg}")
    print("FAILED" if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
