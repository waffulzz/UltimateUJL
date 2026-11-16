"""Function discovery + control-flow analysis for a PS-X EXE.

python -I tools/recomp/analyze.py <exe> [--listing out.txt]
"""
import json
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Exe, Insn, REG  # noqa: E402


class Func:
    def __init__(self, start):
        self.start = start
        self.end = start          # exclusive
        self.labels = set()       # branch targets inside function
        self.calls = set()        # jal targets
        self.tailcalls = set()    # j targets that are other functions
        self.jumptables = {}      # jr addr -> list of targets
        self.indirect = set()     # jr addrs with unknown targets
        self.addrs = set()        # instruction addresses that belong here
        self.bios = {}            # jr addr -> (vector, fn) for BIOS stubs


class Analyzer:
    def __init__(self, exe):
        self.exe = exe
        self.funcs = {}
        self.code_end = exe.end
        self.entry_points = set()
        self.extra_seeds = set()

    def refs_out(self, lo, hi):
        """Addresses in [lo,hi) that this image references: jal targets, data words, lui/addiu
        constants. Used to seed the main exe with functions only overlays call."""
        exe = self.exe
        out = set()
        for a in range(exe.base, exe.end, 4):
            w = exe.word(a)
            if w >> 26 == 3:
                t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if lo <= t < hi:
                    out.add(("jal", t))
            if lo <= w < hi and w & 3 == 0:
                out.add(("ptr", w))
            i1 = exe.insn(a)
            if i1.name == "lui" and i1.rt:
                for k in range(1, 64):
                    if not exe.contains(a + 4 * k):
                        break
                    i2 = exe.insn(a + 4 * k)
                    if i2.name in ("addiu", "ori") and i2.rs == i1.rt:
                        v = ((i1.imm << 16) + (i2.simm if i2.name == "addiu" else i2.imm)) & 0xFFFFFFFF
                        if lo <= v < hi and v & 3 == 0:
                            out.add(("ptr", v))
                    if i2.rt == i1.rt and i2.name not in ("sw", "sh", "sb", "beq", "bne"):
                        break
        return out

    def load_seeds(self, path):
        """Seeds file: one hex address per line ('#' comments). Used for continuation
        entry points (e.g. setjmp return sites the BIOS custom exit resumes at)."""
        if not path or not os.path.exists(path):
            return
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                self.extra_seeds.add(int(line, 16))

    # ---------- seeds ----------
    def jal_targets(self):
        out = set()
        for a in range(self.exe.base, self.exe.end, 4):
            w = self.exe.word(a)
            if w >> 26 == 3:
                t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if self.exe.contains(t):
                    out.add(t)
        return out

    def valid_code(self, addr, n=6):
        """Rough check that addr starts plausible code."""
        for i in range(n):
            a = addr + i * 4
            if not self.exe.contains(a):
                return False
            ins = self.exe.insn(a)
            if ins.name in ("invalid", "cop0?", "cop2?"):
                return False
        return True

    # ---------- CFG ----------
    @staticmethod
    def writes_reg(ins):
        """GPR written by ins (None if none)."""
        n = ins.name
        if ins.op == 0 and n not in ("jr", "mult", "multu", "div", "divu", "mthi", "mtlo", "syscall", "break", "nop"):
            return ins.rd
        if n in ("addiu", "addi", "slti", "sltiu", "andi", "ori", "xori", "lui", "lb", "lbu", "lh", "lhu",
                 "lw", "lwl", "lwr", "mfc0", "mfc2", "cfc2"):
            return ins.rt
        if n in ("jal", "bltzal", "bgezal"):
            return 31
        return None

    def find_const(self, f, at, reg, depth=12):
        """Backtrack linearly for 'lui reg' (+ optional addiu/ori reg,reg) or 'addiu/ori reg,zero'.
        Gives up at any other write to reg."""
        lo = None
        a = at - 4
        while depth > 0 and a >= f.start:
            ins = self.exe.insn(a)
            w = self.writes_reg(ins)
            if w == reg:
                if ins.name in ("addiu", "ori") and ins.rs == 0:
                    if lo is not None:
                        return None
                    return ins.simm & 0xFFFFFFFF if ins.name == "addiu" else ins.imm
                if ins.name in ("addiu", "ori") and ins.rs == reg and lo is None:
                    lo = ins
                elif ins.name == "lui":
                    v = ins.imm << 16
                    if lo is not None:
                        v = (v + lo.simm) & 0xFFFFFFFF if lo.name == "addiu" else v | lo.imm
                    return v
                else:
                    return None
            a -= 4
            depth -= 1
        return None

    def find_jumptable(self, f, jr_addr, reg):
        """Pattern: lui rX,hi ; addu rX,rX,rIdx ; lw rJ,lo(rX) ; jr rJ  (and variants)."""
        a = jr_addr - 4
        lw = None
        for _ in range(8):
            if a < f.start:
                break
            ins = self.exe.insn(a)
            if ins.name == "lw" and ins.rt == reg:
                lw = ins
                break
            a -= 4
        if lw is None:
            return None
        base_reg = lw.rs
        hi = None
        b = lw.addr - 4
        for _ in range(64):
            if b < f.start:
                break
            ins = self.exe.insn(b)
            if ins.name == "lui" and ins.rt == base_reg:
                hi = ins.imm << 16
                break
            if self.writes_reg(ins) == base_reg and ins.name != "addu":
                break
            if ins.name == "addu" and ins.rd == base_reg:
                # base = hi(rs) + idx(rt) ; look for lui of either
                for cand in (ins.rs, ins.rt):
                    v = self.find_const(f, ins.addr, cand, 64)
                    if v is not None:
                        hi = v
                        break
                if hi is not None:
                    break
            b -= 4
        if hi is None:
            return None
        table = (hi + lw.simm) & 0xFFFFFFFF
        targets = []
        t = table
        while self.exe.contains(t):
            v = self.exe.word(t)
            if not (self.exe.base <= v < self.exe.end) or v & 3:
                break
            targets.append(v)
            t += 4
            if len(targets) > 512:
                break
        return table, targets

    def analyze_func(self, start, known_starts):
        f = Func(start)
        work = [start]
        seen = set()
        while work:
            a = work.pop()
            while True:
                if a in seen or not self.exe.contains(a):
                    break
                seen.add(a)
                ins = self.exe.insn(a)
                f.addrs.add(a)
                if ins.has_delay:
                    # delay slot is covered but NOT marked seen: it may also be
                    # a branch target whose sequential successors must be walked
                    f.addrs.add(a + 4)
                n = ins.name
                if ins.is_branch:
                    f.labels.add(ins.target)
                    work.append(ins.target)
                    if n in ("bltzal", "bgezal"):
                        f.calls.add(ins.target)
                    # beq zero,zero is unconditional
                    if n == "beq" and ins.rs == 0 and ins.rt == 0:
                        break
                    a += 8
                    f.labels.add(a)
                    continue
                if n == "jal":
                    f.calls.add(ins.target)
                    a += 8
                    continue
                if n == "j":
                    if not self.exe.contains(ins.target):
                        f.tailcalls.add(ins.target)
                    elif ins.target in known_starts and ins.target != start:
                        f.tailcalls.add(ins.target)
                    else:
                        f.labels.add(ins.target)
                        work.append(ins.target)
                    break
                if n == "jr":
                    if ins.rs == 31:
                        break
                    v = self.find_const(f, a, ins.rs)
                    if v in (0xA0, 0xB0, 0xC0):
                        fn = self.find_const(f, a + 8, 9, 3)  # t1 set in delay slot or before
                        f.bios[a] = (v, fn)
                        break
                    jt = self.find_jumptable(f, a, ins.rs)
                    if jt:
                        table, targets = jt
                        f.jumptables[a] = (table, targets)
                        for t in targets:
                            f.labels.add(t)
                            work.append(t)
                    else:
                        f.indirect.add(a)
                    break
                if n == "jalr":
                    a += 8
                    continue
                if n == "break" or n == "invalid":
                    break
                a += 4
        f.end = max(f.addrs) + 4 if f.addrs else start + 4
        return f

    def prologue_sweep(self):
        """Linear sweep: 'addiu sp,sp,-N' followed shortly by a store relative to sp."""
        out = set()
        exe = self.exe
        for a in range(exe.base, exe.end - 24, 4):
            i = exe.insn(a)
            if not (i.name == "addiu" and i.rs == 29 and i.rt == 29 and -0x1000 < i.simm < 0 and i.simm % 8 == 0):
                continue
            for k in range(1, 6):
                j = exe.insn(a + 4 * k)
                if j.name == "sw" and j.rs == 29 and j.rt in (31, 16, 17, 18, 19, 20, 21, 22, 23, 30):
                    if self.valid_code(a, 8):
                        out.add(a)
                    break
        return out

    def run(self, sweep=False):
        exe = self.exe
        starts = self.jal_targets()
        if exe.pc is not None:
            starts.add(exe.pc)
        if sweep:
            starts |= self.prologue_sweep()
        # function pointers in data: words pointing at plausible code that begins after a jr ra
        ptrs = set()
        for a in range(exe.base, exe.end, 4):
            v = exe.word(a)
            if exe.contains(v) and v & 3 == 0 and v not in starts:
                prev2 = v - 8
                if exe.contains(prev2) and exe.word(prev2) == 0x03E00008 and self.valid_code(v):
                    ptrs.add(v)
        # lui/addiu pairs producing code pointers
        for a in range(exe.base, exe.end - 4, 4):
            i1 = exe.insn(a)
            if i1.name == "lui":
                for k in range(1, 6):
                    if not exe.contains(a + 4 * k):
                        break
                    i2 = exe.insn(a + 4 * k)
                    if i2.name == "addiu" and i2.rs == i1.rt:
                        v = ((i1.imm << 16) + i2.simm) & 0xFFFFFFFF
                        if exe.contains(v) and exe.word(v - 8) == 0x03E00008 and self.valid_code(v):
                            ptrs.add(v)
                        break
        self.ptr_starts = ptrs
        starts |= ptrs
        starts |= self.extra_seeds
        self.entry_points = set(starts)
        # iterate: analyze; add newly discovered jal targets
        pending = sorted(starts)
        while pending:
            s = pending.pop()
            if s in self.funcs:
                continue
            f = self.analyze_func(s, starts)
            self.funcs[s] = f
            for c in f.calls | f.tailcalls:
                if c not in starts and exe.contains(c):
                    starts.add(c)
                    pending.append(c)
        # second pass: any word in the image pointing into code that no function covers
        # (pointer tables whose targets follow alignment nops, etc.)
        # constants built with lui + addiu/ori anywhere in code; follow the lui register
        # forward until it is overwritten (catches 'lui fp,hi ... addiu a0,fp,lo' far apart)
        consts = set()
        for a in range(exe.base, exe.end - 4, 4):
            i1 = exe.insn(a)
            if i1.name != "lui" or i1.rt == 0:
                continue
            reg, hi = i1.rt, i1.imm << 16
            for k in range(1, 400):
                b = a + 4 * k
                if not exe.contains(b):
                    break
                i2 = exe.insn(b)
                if i2.name in ("addiu", "ori") and i2.rs == reg:
                    consts.add((hi + (i2.simm if i2.name == "addiu" else i2.imm)) & 0xFFFFFFFF)
                if i2.name == "jr" and i2.rs == 31:
                    break
                # register overwritten?
                writes = None
                if i2.op == 0 and i2.name not in ("jr", "mult", "multu", "div", "divu", "mthi", "mtlo", "syscall", "break", "nop"):
                    writes = i2.rd
                elif i2.name in ("addiu", "addi", "slti", "sltiu", "andi", "ori", "xori", "lui", "lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr", "mfc0", "mfc2", "cfc2"):
                    writes = i2.rt
                elif i2.name in ("jal", "jalr"):
                    if reg not in (16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30):
                        break  # caller-saved reg clobbered by call
                if writes == reg:
                    break

        def looks_like_start(v):
            """v can't be reached by falling through (preceded by an unconditional jump + delay
            slot, possibly followed by nop padding), or it opens a stack frame early."""
            k = v - 4
            while k >= exe.base and exe.word(k) == 0 and v - k < 32:
                k -= 4
            for cand in (k - 4, k):
                if cand < exe.base:
                    continue
                w = exe.word(cand)
                ins = exe.insn(cand)
                if w == 0x03E00008 or ins.name in ("j", "jr") or (ins.name == "beq" and ins.rs == 0 and ins.rt == 0):
                    return True
            for q in range(4):
                if not exe.contains(v + 4 * q):
                    break
                i = exe.insn(v + 4 * q)
                if i.name == "addiu" and i.rs == 29 and i.rt == 29 and i.simm < 0:
                    return True
                if i.has_delay:
                    break
            return False

        for _ in range(8):
            covered = set()
            for f in self.funcs.values():
                covered |= f.addrs
            code_end = max(f.end for f in self.funcs.values())
            new = []
            words = set(exe.word(a) for a in range(exe.base, exe.end, 4))
            cands = words | consts
            for v in cands:
                if not (exe.base <= v < code_end and v & 3 == 0) or v in self.funcs:
                    continue
                first = exe.insn(v)
                prologue = first.name == "addiu" and first.rs == 29 and first.rt == 29 and first.simm < 0
                # referenced + (follows a function return, or opens a stack frame) => function start,
                # even if another function's analysis flowed into it
                # a pointer-table target no analyzed function reaches is a start too (COMOD12.COP's
                # 801c6d94 follows embedded sequence data, so nothing marks it as code otherwise)
                orphan = v in words and v not in covered
                if self.valid_code(v) and (looks_like_start(v) or prologue or orphan):
                    new.append(v)
            if not new:
                break
            for v in set(new):
                starts.add(v)
                self.ptr_starts.add(v)
                pending.append(v)
            while pending:
                s = pending.pop()
                if s in self.funcs:
                    continue
                f = self.analyze_func(s, starts)
                self.funcs[s] = f
                for c in f.calls | f.tailcalls:
                    if c not in starts and exe.contains(c):
                        starts.add(c)
                        pending.append(c)
        return self.funcs


def main():
    exe = Exe(sys.argv[1])
    an = Analyzer(exe)
    if "--seeds" in sys.argv:
        an.load_seeds(sys.argv[sys.argv.index("--seeds") + 1])
    funcs = an.run()
    total = sum(len(f.addrs) for f in funcs.values())
    print(f"base={exe.base:08x} end={exe.end:08x} entry={exe.pc:08x}")
    print(f"functions={len(funcs)} insns={total} ptr-only={len(an.ptr_starts)}")
    print(f"jumptables={sum(len(f.jumptables) for f in funcs.values())} "
          f"unresolved-jr={sum(len(f.indirect) for f in funcs.values())} "
          f"bios-stubs={sum(len(f.bios) for f in funcs.values())}")
    hi = max(f.end for f in funcs.values())
    print(f"highest code addr={hi:08x}")
    out = {
        "base": exe.base, "end": exe.end, "entry": exe.pc,
        "funcs": {f"{s:08x}": {
            "end": f.end, "labels": sorted(f.labels), "calls": sorted(f.calls),
            "tail": sorted(f.tailcalls),
            "jt": {f"{k:08x}": v for k, v in f.jumptables.items()},
            "ind": sorted(f.indirect),
            "bios": {f"{k:08x}": v for k, v in f.bios.items()},
            "addrs": sorted(f.addrs),
        } for s, f in sorted(funcs.items())},
    }
    if "--json" in sys.argv:
        json.dump(out, open(sys.argv[sys.argv.index("--json") + 1], "w"))
    if "--listing" in sys.argv:
        path = sys.argv[sys.argv.index("--listing") + 1]
        owner = {}
        for s, f in funcs.items():
            for a in f.addrs:
                owner.setdefault(a, s)
        with open(path, "w") as o:
            for s, f in sorted(funcs.items()):
                tags = []
                if f.bios:
                    tags.append("BIOS " + ",".join(f"{v:02x}:{(fn if fn is not None else -1):02x}" for v, fn in f.bios.values()))
                if s in an.ptr_starts:
                    tags.append("ptr")
                o.write(f"\n;=== func_{s:08x}  ({len(f.addrs)} insns) {' '.join(tags)}\n")
                for a in sorted(f.addrs):
                    lab = "L" if a in f.labels else " "
                    o.write(f"{lab} {a:08x}: {exe.word(a):08x}  {exe.insn(a)}\n")


if __name__ == "__main__":
    main()
