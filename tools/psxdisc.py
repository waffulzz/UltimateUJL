"""Minimal PS1 (Mode2/2352) ISO9660 reader.

Usage:
  python -I tools/psxdisc.py ls  <image.bin>
  python -I tools/psxdisc.py get <image.bin> <PATH> <out>      (Form1 data, 2048/sector)
  python -I tools/psxdisc.py raw <image.bin> <PATH> <out>      (full 2352-byte sectors, for STR/XA)
  python -I tools/psxdisc.py dump <image.bin> <outdir>         (extract all files, Form1 + .raw for XA/STR)
"""
import os
import struct
import sys

SECTOR = 2352


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)

    def raw(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def data(self, lba):
        s = self.raw(lba)
        return s[24:24 + 2048]

    def is_form2(self, lba):
        s = self.raw(lba)
        return bool(s[18] & 0x20)

    def read(self, lba, size):
        out = bytearray()
        n = (size + 2047) // 2048
        for i in range(n):
            out += self.data(lba + i)
        return bytes(out[:size])

    def walk(self):
        pvd = self.data(16)
        assert pvd[1:6] == b"CD001", "not ISO9660"
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        yield from self._walk_dir(lba, size, "")

    def _walk_dir(self, lba, size, prefix):
        buf = self.read(lba, size)
        i = 0
        while i < len(buf):
            ln = buf[i]
            if ln == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = buf[i:i + ln]
            elba = struct.unpack_from("<I", rec, 2)[0]
            esize = struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            nlen = rec[32]
            name = rec[33:33 + nlen]
            i += ln
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            path = prefix + "/" + name
            if flags & 2:
                yield (path, elba, esize, True)
                yield from self._walk_dir(elba, esize, path)
            else:
                yield (path, elba, esize, False)


def main():
    cmd, img = sys.argv[1], sys.argv[2]
    d = Disc(img)
    entries = list(d.walk())
    if cmd == "ls":
        for p, lba, size, isdir in entries:
            kind = "DIR " if isdir else ("XA  " if (not isdir and d.is_form2(lba)) else "    ")
            print(f"{kind}{lba:7d} {size:10d}  {p}")
    elif cmd in ("get", "raw"):
        want = sys.argv[3].upper()
        for p, lba, size, isdir in entries:
            if p.upper() == want:
                with open(sys.argv[4], "wb") as o:
                    if cmd == "get":
                        o.write(d.read(lba, size))
                    else:
                        for k in range((size + 2047) // 2048):
                            o.write(d.raw(lba + k))
                return
        sys.exit("not found")
    elif cmd == "dump":
        outdir = sys.argv[3]
        for p, lba, size, isdir in entries:
            dst = os.path.join(outdir, p.lstrip("/"))
            if isdir:
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if d.is_form2(lba):
                # XA/STR: ISO size counts 2048/sector; keep raw sectors
                with open(dst + ".raw", "wb") as o:
                    for k in range((size + 2047) // 2048):
                        o.write(d.raw(lba + k))
            else:
                with open(dst, "wb") as o:
                    o.write(d.read(lba, size))


if __name__ == "__main__":
    main()
