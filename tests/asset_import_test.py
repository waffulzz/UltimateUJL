"""Exercise user-media import with synthetic bytes only. No game data is needed."""
import importlib.util
import io
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import warnings
import zipfile
import zlib


def run_tests(executable):
    exe = str(pathlib.Path(executable).resolve())
    checks = 0
    with tempfile.TemporaryDirectory(prefix="ujl-media-") as temporary:
        root = pathlib.Path(temporary)

        def run(*args, success=True):
            nonlocal checks
            result = subprocess.run([exe, *map(str, args)], capture_output=True, text=True)
            assert (result.returncode == 0) == success, (args, result.returncode, result.stderr)
            checks += 1

        def archive(name, entries, compression=zipfile.ZIP_DEFLATED):
            path = root / name
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(path, "w", compression=compression) as z:
                    for key, data in entries:
                        z.writestr(key, data)
            return path

        data = random.Random(73).randbytes(200_003) + bytes(200_000)
        for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            path = archive(f"method{method}.zip", [("nested/file.bin", data), ("empty", b"")], method)
            target = root / "output.bin"
            run("extract", path, "file.bin", target)
            assert target.read_bytes() == data
            run("verify", path, "empty")
            run("cancel", path, "file.bin", success=False)
            run("verify", path, "absent", success=False)
        # Archive names never become output paths.
        traversal = archive("traversal.zip", [("../../outside.bin", data)])
        run("extract", traversal, "outside.bin", root / "fixed.bin")
        assert (root / "fixed.bin").read_bytes() == data
        duplicate = archive("duplicate.zip", [("a/file", data), ("b/FILE", data)])
        run("verify", duplicate, "file", success=False)
        stored = archive("corrupt.zip", [("file", data)], zipfile.ZIP_STORED)
        raw = bytearray(stored.read_bytes())
        raw[40] ^= 1
        stored.write_bytes(raw)
        run("extract", stored, "file", root / "bad.bin", success=False)
        assert not (root / "bad.bin").exists()
        stored.write_bytes(raw[:-12])
        run("verify", stored, "file", success=False)
        encrypted = archive("encrypted.zip", [("file", data)])
        raw = bytearray(encrypted.read_bytes())
        central_at = raw.index(b"PK\x01\x02")
        struct.pack_into("<H", raw, 6, 1)
        struct.pack_into("<H", raw, central_at + 8, 1)
        encrypted.write_bytes(raw)
        run("verify", encrypted, "file", success=False)
        damaged = archive("deflate-bad.zip", [("file", data)])
        raw = bytearray(damaged.read_bytes())
        raw[40] ^= 0xff
        damaged.write_bytes(raw)
        run("verify", damaged, "file", success=False)
        oversized = archive("oversized.zip", [("file", bytes(200000))])
        raw = bytearray(oversized.read_bytes())
        struct.pack_into("<I", raw, raw.index(b"PK\x01\x02") + 24, 1)
        oversized.write_bytes(raw)
        run("extract", oversized, "file", root / "oversized.bin", success=False)
        assert not (root / "oversized.bin").exists()
        raw = bytearray(encrypted.read_bytes())
        central = raw.index(b"PK\x01\x02")
        struct.pack_into("<H", raw, 6, 1)
        struct.pack_into("<H", raw, central + 8, 1)
        encrypted.write_bytes(raw)
        run("verify", encrypted, "file", success=False)
        # ZIP64 EOCD, locator, and per-entry size/offset extra fields.
        z64 = archive("zip64.zip", [("file", data)])
        raw = z64.read_bytes()
        at = raw.index(b"PK\x01\x02")
        old = raw[at:-22]
        name_len = struct.unpack_from("<H", old, 28)[0]
        packed, unpacked = struct.unpack_from("<II", old, 20)
        central = bytearray(old[:46 + name_len])
        struct.pack_into("<II", central, 20, 0xffffffff, 0xffffffff)
        struct.pack_into("<H", central, 30, 28)
        struct.pack_into("<I", central, 42, 0xffffffff)
        central += struct.pack("<HHQQQ", 1, 24, unpacked, packed, 0)
        record_at = at + len(central)
        record = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, 1, 1, len(central), at)
        locator = struct.pack("<IIQI", 0x07064b50, 0, record_at, 1)
        end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 65535, 65535, 0xffffffff, 0xffffffff, 0)
        z64.write_bytes(raw[:at] + central + record + locator + end)
        run("extract", z64, "file", root / "zip64.bin")
        assert (root / "zip64.bin").read_bytes() == data

        # Generate zero-filled board-sized fixtures with the expected checksum. Only numeric metadata is used.
        roms = [
            ("ul1rom0l.ic12", 8388608, 0x7a77cffd), ("ul1rom0u.ic11", 8388608, 0x1fdb34b3),
            ("ul1rom1l.ic10", 8388608, 0x2d300d8c), ("ul1rom1u.ic9", 8388608, 0xa4762002),
            ("ul1spro.10c", 524288, 0x1bd0e763), ("ul1vera.11s", 524288, 0x2f466df3),
            ("ul1vera.2l", 2097152, 0x40251bc3), ("ul1vera.2p", 2097152, 0x98181955),
            ("ul1wave0.ic2", 8388608, 0x4efbff5d), ("ul1wave1.ic1", 8388608, 0xad512f4c),
        ]
        basis = {}
        zero_crc = zlib.crc32(bytes(4))
        for i in range(32):
            value = zlib.crc32((1 << i).to_bytes(4, "little")) ^ zero_crc
            mask = 1 << i
            while value:
                bit = value.bit_length() - 1
                if bit not in basis:
                    basis[bit] = value, mask
                    break
                other, previous = basis[bit]
                value ^= other
                mask ^= previous

        def rom_bytes(size, crc):
            value = crc ^ zlib.crc32(bytes(size))
            mask = 0
            while value:
                other, previous = basis[value.bit_length() - 1]
                value ^= other
                mask ^= previous
            data = bytes(size - 4) + mask.to_bytes(4, "little")
            assert zlib.crc32(data) == crc
            return data

        board = archive("board.zip", [(name, rom_bytes(size, crc)) for name, size, crc in roms])
        destination = root / "board"
        run("import", board, destination, 0)
        assert sorted(p.name for p in destination.iterdir()) == sorted(name for name, _, _ in roms)
        for name, size, crc in roms:
            content = (destination / name).read_bytes()
            assert len(content) == size and zlib.crc32(content) == crc
        run("import", board, destination, 0, success=False)
        assert (destination / roms[0][0]).exists()  # existing import survives
        cancelled = root / "cancelled"
        run("cancel-import", board, cancelled, success=False)
        assert not cancelled.exists()

        # Small raw ISO9660 arcade-shaped CD, reusing the smoke test's structural helpers.
        spec = importlib.util.spec_from_file_location("smoke", pathlib.Path(__file__).parent / "smoke/smoke.py")
        smoke = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smoke)
        image = bytearray(smoke.make_disc([]))
        pvd = bytearray(image[16 * 2352 + 24:16 * 2352 + 24 + 2048])
        pvd[80:88] = smoke.both32(24)
        record = smoke.dir_record
        directory = lambda lba, parent: record(b"\0", lba, 2048, True) + record(b"\1", parent, 2048, True)
        sectors = {
            16: bytes(pvd),
            18: directory(18, 18) + record(b"MOVIE", 19, 2048, True) + record(b"XA", 20, 2048, True),
            19: directory(19, 18) + record(b"AM01.IKI;1", 21, 4, False) + record(b"NAMCO.IKI;1", 22, 4, False),
            20: directory(20, 18) + record(b"JOE1.XA;1", 23, 4, False),
        }
        image += bytes(4 * 2352)
        for lba, contents in sectors.items():
            image[lba * 2352:(lba + 1) * 2352] = smoke.raw_sector(lba, contents)
        cd = archive("cd.zip", [("disc/image.img", image)])
        bundle = archive("bundle.zip", [
            ("ujlnow.zip", board.read_bytes()),
            ("UJLNOW - CD image (HL GDR-8164B, CloneCD).zip", cd.read_bytes()),
            ("unused.zip", b"not extracted"),
        ])
        output = root / "bundle"
        run("import", bundle, output, 1)
        assert (output / "arcade.img").read_bytes() == image
        assert len(list(output.iterdir())) == 11
        bad_bundle = archive("bad-bundle.zip", [
            ("ujlnow.zip", board.read_bytes()),
            ("CD image (CloneCD).zip", archive("wrong-cd.zip", [("image.img", bytes(64))]).read_bytes()),
        ])
        output = root / "bad-bundle"
        run("import", bad_bundle, output, 1, success=False)
        assert not output.exists()
    print(f"PASS: {checks} archive/import checks (synthetic media only)")


if __name__ == "__main__":
    run_tests(sys.argv[1])
