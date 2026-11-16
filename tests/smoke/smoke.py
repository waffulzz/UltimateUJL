"""End-to-end smoke test of the recompiler and runtime, with no game data.

It assembles a small PS-X EXE that exercises the parts of the pipeline every port depends on (calls and
recursion, delay slots, loops, mult/div, shifts, sign extension, a function-pointer call, GPU MMIO), puts it
on a synthetic ISO9660 Mode2/2352 disc image, recompiles it with tools/recomp/gen.py, then runs the headless
probe built against that code and checks guest RAM and the displayed frame.

  python -I tests/smoke/smoke.py gen <workdir>              disc + generated C in <workdir>/gen
  python -I tests/smoke/smoke.py run <ujl_probe> <workdir>  boot the disc in the probe and check results
  python -I tests/smoke/smoke.py frontend <ujl> <workdir>   boot it in the real frontend (needs OpenGL 3.3+),
                                                            check the hardware renderer's screenshot

CI configures CMake with -DRECOMP_DIR=<workdir>/gen between the two steps. The binaries built that way run
only this test program, not the game.
"""
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

BASE = 0x80010000      # load address and entry point
RESULT = 0x80080000    # results block (phys 0x80000)
DONE_MAGIC = 0x600DF00D
FILL_RGB = (0x20, 0x80, 0xE0)

# ---------------------------------------------------------------- tiny MIPS assembler

REG = {n: i for i, n in enumerate(
    "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}

I_OPS = {"beq": 4, "bne": 5, "addiu": 9, "slti": 10, "sltiu": 11, "andi": 12, "ori": 13, "xori": 14, "lui": 15,
         "lb": 32, "lh": 33, "lw": 35, "lbu": 36, "lhu": 37, "sb": 40, "sh": 41, "sw": 43}
R_FUNCS = {"sll": 0, "srl": 2, "sra": 3, "jr": 8, "jalr": 9, "mfhi": 16, "mflo": 18, "mult": 24, "multu": 25,
           "div": 26, "divu": 27, "addu": 33, "subu": 35, "and": 36, "or": 37, "xor": 38, "nor": 39,
           "slt": 42, "sltu": 43}


class Asm:
    def __init__(self, base):
        self.base = base
        self.items = []     # (kind, args)
        self.labels = {}

    def pc(self):
        return self.base + 4 * len(self.items)

    def label(self, name):
        self.labels[name] = self.pc()

    def __getattr__(self, op):
        def emit(*args):
            self.items.append((op, args))
        return emit

    def li(self, rt, value):
        value &= 0xFFFFFFFF
        if value < 0x10000:
            self.items.append(("ori", (rt, "zero", value)))
        else:
            self.items.append(("lui", (rt, value >> 16)))
            if value & 0xFFFF:
                self.items.append(("ori", (rt, rt, value & 0xFFFF)))

    def la(self, rt, label):  # fixed two-word form, resolved at link time
        self.items.append(("lui", (rt, ("hi", label))))
        self.items.append(("ori", (rt, rt, ("lo", label))))

    def op(self, name, *args):  # mnemonics that are Python keywords (or, and)
        self.items.append((name, args))

    def nop(self):
        self.items.append(("word", (0,)))

    def word(self, value):
        self.items.append(("word", (value,)))

    def link(self):
        out = bytearray()
        for i, (op, args) in enumerate(self.items):
            out += struct.pack("<I", self._encode(self.base + 4 * i, op, args))
        return bytes(out)

    def _val(self, v):
        if isinstance(v, tuple):
            kind, name = v
            a = self.labels[name]
            return a >> 16 if kind == "hi" else a & 0xFFFF
        if isinstance(v, str):
            return self.labels[v]
        return v

    def _encode(self, pc, op, a):
        r = lambda x: REG[x]
        if op == "word":
            return self._val(a[0]) & 0xFFFFFFFF
        if op in ("j", "jal"):
            return (2 if op == "j" else 3) << 26 | (self.labels[a[0]] >> 2) & 0x3FFFFFF
        if op in ("beq", "bne"):
            off = (self.labels[a[2]] - (pc + 4)) >> 2
            return I_OPS[op] << 26 | r(a[0]) << 21 | r(a[1]) << 16 | off & 0xFFFF
        if op == "b":
            off = (self.labels[a[0]] - (pc + 4)) >> 2
            return 4 << 26 | off & 0xFFFF
        if op == "lui":
            return 15 << 26 | r(a[0]) << 16 | self._val(a[1]) & 0xFFFF
        if op in ("lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw"):  # op rt, off(base)
            return I_OPS[op] << 26 | r(a[2]) << 21 | r(a[0]) << 16 | self._val(a[1]) & 0xFFFF
        if op in I_OPS:  # op rt, rs, imm
            return I_OPS[op] << 26 | r(a[1]) << 21 | r(a[0]) << 16 | self._val(a[2]) & 0xFFFF
        f = R_FUNCS[op]
        if op in ("sll", "srl", "sra"):  # op rd, rt, sa
            return r(a[1]) << 16 | r(a[0]) << 11 | (a[2] & 31) << 6 | f
        if op == "jr":
            return r(a[0]) << 21 | f
        if op == "jalr":  # jalr rs (rd = ra)
            return r(a[0]) << 21 | 31 << 11 | f
        if op in ("mfhi", "mflo"):
            return r(a[0]) << 11 | f
        if op in ("mult", "multu", "div", "divu"):
            return r(a[0]) << 21 | r(a[1]) << 16 | f
        return r(a[1]) << 21 | r(a[2]) << 16 | r(a[0]) << 11 | f  # op rd, rs, rt


def u32(x):
    return x & 0xFFFFFFFF


def test_program():
    """Returns (exe image bytes, extra function seeds, {result offset: expected u32})."""
    a = Asm(BASE)
    expect = {}

    a.lui("s0", RESULT >> 16)

    # recursion + stack + jal + a delay-slot stack pop
    a.li("a0", 20); a.jal("fib"); a.nop()
    a.sw("v0", 0x00, "s0"); expect[0x00] = 6765

    # counted loop (backward branch = safepoint)
    a.addu("t0", "zero", "zero"); a.li("t1", 1); a.li("t2", 1001)
    a.label("sum")
    a.addu("t0", "t0", "t1"); a.addiu("t1", "t1", 1); a.bne("t1", "t2", "sum"); a.nop()
    a.sw("t0", 0x04, "s0"); expect[0x04] = 500500

    # mult / div / signed div
    a.li("t0", 123456); a.li("t1", 789); a.mult("t0", "t1"); a.mflo("t2"); a.mfhi("t3")
    a.sw("t2", 0x08, "s0"); a.sw("t3", 0x0C, "s0"); expect[0x08] = 123456 * 789; expect[0x0C] = 0
    a.li("t4", 7); a.div("t2", "t4"); a.mflo("t5"); a.mfhi("t6")
    a.sw("t5", 0x10, "s0"); a.sw("t6", 0x14, "s0"); expect[0x10] = 123456 * 789 // 7; expect[0x14] = 123456 * 789 % 7
    a.addiu("t0", "zero", -100); a.div("t0", "t4"); a.mflo("t5"); a.mfhi("t6")
    a.sw("t5", 0x18, "s0"); a.sw("t6", 0x1C, "s0"); expect[0x18] = u32(-14); expect[0x1C] = u32(-2)
    a.li("t0", 0xFFFFFFFE); a.li("t1", 3); a.multu("t0", "t1"); a.mfhi("t2"); a.mflo("t3")
    a.sw("t2", 0x20, "s0"); a.sw("t3", 0x24, "s0"); expect[0x20] = (0xFFFFFFFE * 3) >> 32; expect[0x24] = u32(0xFFFFFFFE * 3)

    # shifts, compares
    a.lui("t0", 0x8000); a.sra("t1", "t0", 4); a.srl("t2", "t0", 4)
    a.sw("t1", 0x28, "s0"); a.sw("t2", 0x2C, "s0"); expect[0x28] = 0xF8000000; expect[0x2C] = 0x08000000
    a.addiu("t0", "zero", -1); a.li("t1", 1); a.slt("t2", "t0", "t1"); a.sltu("t3", "t0", "t1")
    a.sll("t3", "t3", 1); a.op("or", "t2", "t2", "t3")
    a.sw("t2", 0x30, "s0"); expect[0x30] = 1

    # sub-word stores and sign/zero-extending loads
    a.li("t0", 0x80FF); a.sh("t0", 0x34, "s0")
    a.lb("t1", 0x34, "s0"); a.lbu("t2", 0x35, "s0"); a.lh("t3", 0x34, "s0"); a.lhu("t4", 0x34, "s0")
    a.sw("t1", 0x38, "s0"); a.sw("t2", 0x3C, "s0"); a.sw("t3", 0x40, "s0"); a.sw("t4", 0x44, "s0")
    expect[0x34] = 0x80FF; expect[0x38] = 0xFFFFFFFF; expect[0x3C] = 0x80; expect[0x40] = 0xFFFF80FF; expect[0x44] = 0x80FF

    # indirect call through a function-pointer table (call_indirect)
    a.la("t0", "table"); a.lw("t9", 4, "t0"); a.li("a0", 14); a.jalr("t9"); a.nop()
    a.sw("v0", 0x48, "s0"); expect[0x48] = 42
    a.lw("t9", 0, "t0"); a.jalr("t9"); a.li("a0", 50)          # argument set in the delay slot
    a.sw("v0", 0x4C, "s0"); expect[0x4C] = 100

    # GPU: 320x240 display at (0,0), fill it with one colour
    a.lui("t0", 0x1F80)
    for v in (0x00000000, 0x08000001, 0x05000000, 0x03000000):
        a.li("t1", v); a.sw("t1", 0x1814, "t0")
    r, g, b = FILL_RGB
    for v in (0x02000000 | b << 16 | g << 8 | r, 0x00000000, 240 << 16 | 320):
        a.li("t1", v); a.sw("t1", 0x1810, "t0")

    a.li("t1", DONE_MAGIC); a.sw("t1", 0x7C, "s0"); expect[0x7C] = DONE_MAGIC
    a.label("forever")
    a.b("forever"); a.nop()

    # fib(n): classic recursive, restores sp in the jr delay slot
    a.label("fib")
    a.addiu("sp", "sp", -16); a.sw("ra", 12, "sp"); a.sw("s0", 8, "sp"); a.sw("s1", 4, "sp")
    a.slti("t0", "a0", 2); a.beq("t0", "zero", "fib_rec"); a.nop()
    a.addu("v0", "a0", "zero"); a.b("fib_out"); a.nop()
    a.label("fib_rec")
    a.addu("s0", "a0", "zero")
    a.addiu("a0", "s0", -1); a.jal("fib"); a.nop(); a.addu("s1", "v0", "zero")
    a.addiu("a0", "s0", -2); a.jal("fib"); a.nop(); a.addu("v0", "v0", "s1")
    a.label("fib_out")
    a.lw("ra", 12, "sp"); a.lw("s0", 8, "sp"); a.lw("s1", 4, "sp")
    a.jr("ra"); a.addiu("sp", "sp", 16)

    a.label("times2")
    a.jr("ra"); a.sll("v0", "a0", 1)
    a.label("times3")
    a.sll("v0", "a0", 1); a.jr("ra"); a.addu("v0", "v0", "a0")

    a.label("table")
    a.word("times2"); a.word("times3")

    code = a.link()
    return code, [a.labels["times2"], a.labels["times3"]], expect


def make_exe(code):
    body = code + b"\0" * (-len(code) % 2048)
    hdr = bytearray(0x800)
    hdr[0:8] = b"PS-X EXE"
    struct.pack_into("<4I", hdr, 0x10, BASE, 0, BASE, len(body))
    struct.pack_into("<2I", hdr, 0x30, 0x801FFF00, 0)
    hdr[0x4C:0x4C + 21] = b"UltimateUJL CI smoke\0"
    return bytes(hdr) + body

# ---------------------------------------------------------------- ISO9660 Mode2/2352 image


def bcd(n):
    return (n // 10) << 4 | n % 10


def raw_sector(lba, data, eof=False):
    assert len(data) <= 2048
    m, s, f = (lba + 150) // 4500, (lba + 150) // 75 % 60, (lba + 150) % 75
    sub = bytes([0, 0, 0x89 if eof else 0x08, 0]) * 2
    return (b"\x00" + b"\xff" * 10 + b"\x00" + bytes([bcd(m), bcd(s), bcd(f), 2]) + sub
            + data.ljust(2048, b"\0") + b"\0" * 280)


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(name, lba, size, is_dir):
    n = len(name)
    rec = bytearray(33 + n + (1 - n % 2))
    rec[0] = len(rec)
    rec[2:10] = both32(lba)
    rec[10:18] = both32(size)
    rec[18:25] = bytes([99, 1, 1, 0, 0, 0, 0])
    rec[25] = 2 if is_dir else 0
    rec[28:32] = both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def make_disc(files):
    """files: [(NAME, bytes)] in the root directory. Returns the raw image."""
    root_lba, first_file = 18, 20
    layout, lba = [], first_file
    for name, data in files:
        layout.append((name, lba, data))
        lba += max(1, (len(data) + 2047) // 2048)
    total = lba
    root = dir_record(b"\x00", root_lba, 2048, True) + dir_record(b"\x01", root_lba, 2048, True)
    for name, flba, data in layout:
        root += dir_record(name.encode() + b";1", flba, len(data), False)
    pvd = bytearray(2048)
    pvd[0], pvd[1:6], pvd[6] = 1, b"CD001", 1
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[40:72] = b"UJL_SMOKE".ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(2048)
    pvd[156:190] = dir_record(b"\x00", root_lba, 2048, True)
    pvd[881] = 1
    term = bytearray(2048)
    term[0], term[1:6], term[6] = 255, b"CD001", 1
    sectors = {16: bytes(pvd), 17: bytes(term), root_lba: root}
    out = bytearray()
    for s in range(first_file):
        out += raw_sector(s, sectors.get(s, b""), eof=s in (17, root_lba))
    for name, flba, data in layout:
        n = max(1, (len(data) + 2047) // 2048)
        for i in range(n):
            out += raw_sector(flba + i, data[i * 2048:(i + 1) * 2048], eof=i == n - 1)
    return bytes(out)

# ---------------------------------------------------------------- commands


def cmd_gen(work):
    os.makedirs(work, exist_ok=True)
    code, seeds, _ = test_program()
    exe = make_exe(code)
    cnf = b"BOOT = cdrom:\\TEST.EXE;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFF00\r\n"
    with open(os.path.join(work, "TEST.EXE"), "wb") as f:
        f.write(exe)
    with open(os.path.join(work, "smoke.bin"), "wb") as f:
        f.write(make_disc([("SYSTEM.CNF", cnf), ("TEST.EXE", exe)]))
    seeds_path = os.path.join(work, "seeds.txt")
    with open(seeds_path, "w") as f:
        f.write("# indirect-call targets of the smoke test program\n")
        for s in seeds:
            f.write(f"{s:08x}\n")
    overrides = os.path.join(work, "overrides.txt")
    open(overrides, "w").close()
    subprocess.check_call([sys.executable, "-I", os.path.join(ROOT, "tools", "recomp", "gen.py"),
                           os.path.join(work, "TEST.EXE"), os.path.join(work, "gen"),
                           "--seeds", seeds_path, "--overrides", overrides])
    print(f"smoke: disc and generated code in {work}")


def read_bmp_pixel(path, x, y):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    bypp = bpp // 8
    stride = (w * bypp + 3) & ~3
    if x is None:  # centre
        x, y = w // 2, abs(h) // 2
    row = (h - 1 - y) if h > 0 else y
    p = off + row * stride + x * bypp
    b, g, r = d[p], d[p + 1], d[p + 2]
    return (w, abs(h)), (r, g, b)


def cmd_run(probe, work):
    run = os.path.join(work, "run")
    os.makedirs(os.path.join(run, "build", "shots"), exist_ok=True)
    env = dict(os.environ, PROBE_RAMDUMP_AT="1", SDL_AUDIO_DRIVER="dummy")
    proc = subprocess.run([os.path.abspath(probe), os.path.abspath(os.path.join(work, "smoke.bin")), "-t", "2", "-shot"],
                          cwd=run, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    log = proc.stdout.decode("utf-8", "replace")
    with open(os.path.join(work, "probe.log"), "w", encoding="utf-8") as f:
        f.write(log)
    fails = []
    if proc.returncode != 0:
        fails.append(f"probe exited with {proc.returncode}")
    for needle in ("booted TEST.EXE", "[probe] done"):
        if needle not in log:
            fails.append(f"log lacks {needle!r}")

    _, _, expect = test_program()
    ram_path = os.path.join(run, "build", "ram_1.bin")
    if os.path.exists(ram_path):
        ram = open(ram_path, "rb").read()
        for off, want in sorted(expect.items()):
            got = struct.unpack_from("<I", ram, (RESULT & 0x1FFFFF) + off)[0]
            if got != want:
                fails.append(f"result +0x{off:02x}: got {got:08x}, want {want:08x}")
    else:
        fails.append("no RAM dump (build/ram_1.bin)")

    shot = os.path.join(run, "build", "shots", "t001.bmp")
    if os.path.exists(shot):
        size, rgb = read_bmp_pixel(shot, 160, 120)
        want = tuple(c & 0xF8 for c in FILL_RGB)
        if size != (320, 240) or rgb != want:
            fails.append(f"frame: {size[0]}x{size[1]} centre {rgb}, want 320x240 {want}")
    else:
        fails.append("no frame dump (build/shots/t001.bmp)")

    if fails:
        print(log[-4000:])
        print("smoke: FAILED")
        for f in fails:
            print("  -", f)
        return 1
    print(f"smoke: OK ({len(expect)} results, frame colour checked)")
    return 0


def cmd_frontend(ujl, work):
    """Boot the disc in the real frontend (OpenGL hardware renderer) with a UJL_SCRIPT and check its screenshot."""
    ujl = os.path.abspath(ujl)
    shots = os.path.join(os.path.dirname(ujl), "shots")
    shot = os.path.join(shots, "hw_v00060.bmp")
    if os.path.exists(shot):
        os.remove(shot)
    script = os.path.join(work, "frontend_script.txt")
    with open(script, "w") as f:
        f.write("1 shot\n2 quit\n")
    env = dict(os.environ, UJL_SCRIPT=os.path.abspath(script), UJL_LOG_STDERR="1",
               UJL_SCRIPT_CARD=os.path.abspath(os.path.join(work, "frontend_card.mcd")))
    env.setdefault("SDL_AUDIO_DRIVER", "dummy")
    proc = subprocess.run([ujl, os.path.abspath(os.path.join(work, "smoke.bin"))], cwd=work, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    log = proc.stdout.decode("utf-8", "replace")
    with open(os.path.join(work, "frontend.log"), "w", encoding="utf-8") as f:
        f.write(log)
    fails = []
    if proc.returncode != 0:
        fails.append(f"ujl exited with {proc.returncode}")
        if proc.returncode < 0 and shutil.which("gdb"):  # crashed: rerun under gdb for a backtrace
            g = subprocess.run(["gdb", "-batch", "-ex", "run", "-ex", "thread apply all bt 40", "--args", ujl,
                                os.path.abspath(os.path.join(work, "smoke.bin"))], cwd=work, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
            print(g.stdout.decode("utf-8", "replace")[-20000:])
    for needle in ("booted TEST.EXE", "hw: renderer ready"):
        if needle not in log:
            fails.append(f"log lacks {needle!r}")
    if os.path.exists(shot):
        (w, h), rgb = read_bmp_pixel(shot, None, None)
        if any(abs(c - want) > 8 for c, want in zip(rgb, FILL_RGB)):
            fails.append(f"window centre {rgb} ({w}x{h}), want about {FILL_RGB}")
    else:
        fails.append(f"no screenshot ({shot})")
    if fails:
        print(log[-4000:])
        print("frontend smoke: FAILED")
        for f in fails:
            print("  -", f)
        return 1
    print(f"frontend smoke: OK (hardware renderer, {w}x{h} window, centre {rgb})")
    return 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "gen":
        cmd_gen(sys.argv[2])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "run":
        return cmd_run(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == "frontend":
        return cmd_frontend(sys.argv[2], sys.argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
