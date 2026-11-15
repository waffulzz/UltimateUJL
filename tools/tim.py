"""TIM image + INT archive tools.

python -I tools/tim.py list <file.INT>
python -I tools/tim.py dump <file.INT> <outdir> [NAME ...]   -> PNGs (first CLUT) + info
"""
import os
import struct
import sys
import zlib


def png_write(path, w, h, rgba):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def c15(c, transparent_black=True):
    r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
    a = 0 if (c == 0 and transparent_black) else 255
    return bytes((r | r >> 5, g | g >> 5, b | b >> 5, a))


def parse_tim(d):
    """Returns dict with bpp, clut (list of colors), clut_xy, img_xy, w, h (pixels), pixels (indices or colors)."""
    magic, flags = struct.unpack_from("<II", d, 0)
    assert magic == 0x10, "not a TIM"
    bpp = {0: 4, 1: 8, 2: 16, 3: 24}[flags & 3]
    off = 8
    t = {"bpp": bpp, "clut": None}
    if flags & 8:
        blen, cx, cy, cw, ch = struct.unpack_from("<IHHHH", d, off)
        t["clut_xy"] = (cx, cy, cw, ch)
        t["clut"] = list(struct.unpack_from(f"<{cw * ch}H", d, off + 12))
        off += blen
    blen, ix, iy, iw, ih = struct.unpack_from("<IHHHH", d, off)
    t["img_xy"] = (ix, iy, iw, ih)
    data = d[off + 12:off + blen]
    if bpp == 4:
        px = [(b >> s) & 15 for b in data for s in (0, 4)]
        w = iw * 4
    elif bpp == 8:
        px = list(data)
        w = iw * 2
    elif bpp == 16:
        px = list(struct.unpack_from(f"<{len(data) // 2}H", data))
        w = iw
    else:
        px = data
        w = iw * 2 // 3
    t["w"], t["h"], t["pixels"] = w, ih, px
    t["size"] = off + blen
    return t


def tim_rgba(t, clut_row=0):
    w, h = t["w"], t["h"]
    out = bytearray()
    if t["bpp"] in (4, 8):
        n = 16 if t["bpp"] == 4 else 256
        pal = t["clut"][clut_row * n:(clut_row + 1) * n] if t["clut"] else [i * 0x421 for i in range(n)]
        for i in range(w * h):
            out += c15(pal[t["pixels"][i]] if t["pixels"][i] < len(pal) else 0)
    elif t["bpp"] == 16:
        for i in range(w * h):
            out += c15(t["pixels"][i])
    else:
        for i in range(w * h):
            out += bytes(t["pixels"][i * 3:i * 3 + 3]) + b"\xff"
    return bytes(out)


def parse_int(d):
    ver, n, w2, w3 = struct.unpack_from("<4I", d, 0)
    ents = []
    off = 16
    for _ in range(n):
        size = struct.unpack_from("<I", d, off)[0]
        name = d[off + 4:off + 20].split(b"\0")[0].decode("ascii", "replace")
        ents.append([name, size, None])
        off += 20
    # TIM data starts after the name table, aligned to 8 KB (0x2000)
    pos = (off + 0x1FFF) & ~0x1FFF
    for e in ents:
        e[2] = pos
        pos += (e[1] + 3) & ~3
    return ents


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    ents = parse_int(d)
    if cmd == "list":
        for name, size, off in ents:
            ok = d[off:off + 4] == b"\x10\0\0\0"
            info = ""
            if ok:
                t = parse_tim(d[off:off + size])
                info = f"{t['bpp']}bpp {t['w']}x{t['h']} img@{t['img_xy'][:2]} clut@{t.get('clut_xy', ('-',))[:2]}"
            print(f"{off:8x} {size:7d} {name:16s} {'TIM' if ok else '???'} {info}")
    elif cmd == "dump":
        outdir = sys.argv[3]
        want = set(sys.argv[4:])
        os.makedirs(outdir, exist_ok=True)
        for name, size, off in ents:
            if want and name not in want:
                continue
            if d[off:off + 4] != b"\x10\0\0\0":
                continue
            t = parse_tim(d[off:off + size])
            png_write(os.path.join(outdir, name.replace(".TIM", ".png")), t["w"], t["h"], tim_rgba(t))


if __name__ == "__main__":
    main()
