"""MIPS -> C static recompiler.

python -I tools/recomp/gen.py <exe> <outdir> [--per-file N]

Emits:
  outdir/recomp_funcs.h     prototypes
  outdir/recomp_NNN.c       function bodies
  outdir/recomp_dispatch.c  address -> function table
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import Analyzer  # noqa: E402
from mips import Exe, REG  # noqa: E402


def fn(a):
    return f"F_{a:08x}"


def R(n):
    return "0" if n == 0 else f"R{n}"


class Gen:
    def __init__(self, exe, funcs, prefix="F_", external=None):
        self.exe = exe
        self.funcs = funcs
        self.starts = set(funcs)
        self.prefix = prefix
        self.external = external or set()   # main-exe function starts callable from an overlay
        self.warn = []

    def name(self, a):
        return f"{self.prefix}{a:08x}"

    # ---- single non-control instruction ----
    def op(self, ins):
        n, rs, rt, rd, sa = ins.name, ins.rs, ins.rt, ins.rd, ins.sa
        S, T = R(rs), R(rt)
        si, ui = ins.simm, ins.imm

        def wd(reg, expr):
            if reg == 0:
                return f"(void)({expr});" if "mem_" in expr else ""
            return f"R{reg} = {expr};"

        ea = f"({S} + {si})" if si else S
        if n == "nop":
            return ""
        if n in ("addu", "add"):
            return wd(rd, f"{S} + {T}")
        if n in ("subu", "sub"):
            return wd(rd, f"{S} - {T}")
        if n == "and":
            return wd(rd, f"{S} & {T}")
        if n == "or":
            return wd(rd, f"{S} | {T}")
        if n == "xor":
            return wd(rd, f"{S} ^ {T}")
        if n == "nor":
            return wd(rd, f"~({S} | {T})")
        if n == "slt":
            return wd(rd, f"(int32_t){S} < (int32_t){T}")
        if n == "sltu":
            return wd(rd, f"{S} < {T}")
        if n == "sll":
            return wd(rd, f"{T} << {sa}")
        if n == "srl":
            return wd(rd, f"{T} >> {sa}")
        if n == "sra":
            return wd(rd, f"(uint32_t)((int32_t){T} >> {sa})")
        if n == "sllv":
            return wd(rd, f"{T} << ({S} & 31)")
        if n == "srlv":
            return wd(rd, f"{T} >> ({S} & 31)")
        if n == "srav":
            return wd(rd, f"(uint32_t)((int32_t){T} >> ({S} & 31))")
        if n == "mult":
            return f"{{ int64_t p = (int64_t)(int32_t){S} * (int32_t){T}; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); }}"
        if n == "multu":
            return f"{{ uint64_t p = (uint64_t){S} * {T}; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); }}"
        if n == "div":
            return f"cpu_div(c, {S}, {T});"
        if n == "divu":
            return f"cpu_divu(c, {S}, {T});"
        if n == "mfhi":
            return wd(rd, "HI")
        if n == "mflo":
            return wd(rd, "LO")
        if n == "mthi":
            return f"HI = {S};"
        if n == "mtlo":
            return f"LO = {S};"
        if n in ("addiu", "addi"):
            return wd(rt, f"{S} + {si}" if si else S)
        if n == "slti":
            return wd(rt, f"(int32_t){S} < {si}")
        if n == "sltiu":
            return wd(rt, f"{S} < 0x{si & 0xFFFFFFFF:x}u")
        if n == "andi":
            return wd(rt, f"{S} & 0x{ui:x}")
        if n == "ori":
            return wd(rt, f"{S} | 0x{ui:x}")
        if n == "xori":
            return wd(rt, f"{S} ^ 0x{ui:x}")
        if n == "lui":
            return wd(rt, f"0x{ui << 16:08x}u")
        if n == "lb":
            return wd(rt, f"(uint32_t)(int8_t)mem_r8({ea})")
        if n == "lbu":
            return wd(rt, f"mem_r8({ea})")
        if n == "lh":
            return wd(rt, f"(uint32_t)(int16_t)mem_r16({ea})")
        if n == "lhu":
            return wd(rt, f"mem_r16({ea})")
        if n == "lw":
            return wd(rt, f"mem_r32({ea})")
        if n == "lwl":
            return wd(rt, f"cpu_lwl({T}, {ea})")
        if n == "lwr":
            return wd(rt, f"cpu_lwr({T}, {ea})")
        if n == "sb":
            return f"mem_w8({ea}, {T});"
        if n == "sh":
            return f"mem_w16({ea}, {T});"
        if n == "sw":
            return f"mem_w32({ea}, {T});"
        if n == "swl":
            return f"cpu_swl({T}, {ea});"
        if n == "swr":
            return f"cpu_swr({T}, {ea});"
        if n == "lwc2":
            return f"gte_write(c, {rt}, mem_r32({ea}));"
        if n == "swc2":
            if 12 <= rt <= 15:  # projected points: remember where they went (precise positions, gte.c)
                return f"gte_store_sxy(c, {ea}, {rt});"
            return f"mem_w32({ea}, gte_read(c, {rt}));"
        if n == "mfc2":
            return wd(rt, f"gte_read(c, {rd})")
        if n == "mtc2":
            return f"gte_write(c, {rd}, {T});"
        if n == "cfc2":
            return wd(rt, f"gte_read(c, {rd + 32})")
        if n == "ctc2":
            return f"gte_write(c, {rd + 32}, {T});"
        if n == "gte":
            return f"gte_cmd(c, 0x{ins.word & 0x1FFFFFF:x});"
        if n == "mfc0":
            return wd(rt, f"cpu_mfc0(c, {rd})")
        if n == "mtc0":
            return f"cpu_mtc0(c, {rd}, {T});"
        if n == "rfe":
            return "cpu_rfe(c);"
        if n == "syscall":
            return f"cpu_syscall(c, 0x{ins.addr:08x});"
        if n == "break":
            return f"cpu_break(c, 0x{ins.addr:08x});"
        self.warn.append(f"{ins.addr:08x}: unhandled {n} {ins.word:08x}")
        return f"cpu_invalid(c, 0x{ins.addr:08x}, 0x{ins.word:08x});"

    def cond(self, ins):
        S, T = R(ins.rs), R(ins.rt)
        return {
            "beq": f"{S} == {T}", "bne": f"{S} != {T}",
            "blez": f"(int32_t){S} <= 0", "bgtz": f"(int32_t){S} > 0",
            "bltz": f"(int32_t){S} < 0", "bgez": f"(int32_t){S} >= 0",
            "bltzal": f"(int32_t){S} < 0", "bgezal": f"(int32_t){S} >= 0",
        }[ins.name]

    def call(self, target):
        if target in self.starts:
            return f"{self.name(target)}(c);"
        if target in self.external:
            return f"{fn(target)}(c);"
        return f"call_indirect(c, 0x{target:08x});"

    # ---- one function ----
    def func(self, f):
        exe = self.exe
        ov = getattr(self, "overrides", {}).get(f.start)
        hook = None
        if ov and ov[0].startswith("hook_"):  # call a runtime hook first, then the original body
            hook = ov[0] + "(c, 0x%08xu%s)" % (f.start, "".join(f", 0x{int(a, 16):08x}u" for a in ov[1:]))
            ov = None
        if ov:  # replaced by a native (HLE) implementation in the runtime
            fn_name, args = ov[0], "".join(f", 0x{int(a, 16):08x}u" for a in ov[1:])
            return (f"void {self.name(f.start)}(CPU *c) {{\n  TRACE_FN(0x{f.start:08x}u);\n"
                    f"  {fn_name}(c{args});\n}}")
        out = [f"void {self.name(f.start)}(CPU *c) {{", f"  TRACE_FN(0x{f.start:08x}u);"]
        if hook:
            out.append(f"  {hook};")
        addrs = sorted(f.addrs)
        addrset = f.addrs
        i = 0
        skip = set()
        for a in addrs:
            if a in skip:
                continue
            ins = exe.insn(a)
            if a in f.labels:
                out.append(f"L_{a:08x}:;")
            if not ins.has_delay:
                s = self.op(ins)
                if s:
                    out.append("  " + s)
                # fallthrough into an address we don't own -> treat as tailcall
                if (a + 4) not in addrset and ins.name not in ("break",):
                    out.append(f"  {self.call(a + 4)} return;")
                continue
            d = exe.insn(a + 4)
            skip.add(a + 4)
            if d.has_delay:
                self.warn.append(f"{a:08x}: branch in delay slot")
            ds = self.op(d) if not d.has_delay else f"/* nested branch {d} */"
            n = ins.name
            back = ins.target is not None and ins.target <= a
            if ins.is_branch:
                link = n in ("bltzal", "bgezal")
                if n == "beq" and ins.rs == 0 and ins.rt == 0:
                    out.append(f"  {ds}")
                    if ins.target not in addrset:  # leaves the function (data decoded as code): tail call
                        out.append(f"  {self.call(ins.target)} return;")
                        continue
                    if back:
                        out.append(f"  SAFEPOINT(c, 0x{a:08x}u);")
                    out.append(f"  goto L_{ins.target:08x};")
                else:
                    out.append(f"  {{ int t = {self.cond(ins)}; {ds}")
                    if link:
                        out.append(f"    if (t) {{ R31 = 0x{a + 8:08x}u; {self.call(ins.target)} }} }}")
                    else:
                        sp = f" SAFEPOINT(c, 0x{a:08x}u);" if back else ""
                        if ins.target not in addrset:
                            out.append(f"    if (t) {{ {self.call(ins.target)} return; }} }}")
                        else:
                            out.append(f"    if (t) {{{sp} goto L_{ins.target:08x}; }} }}")
            elif n == "jal":
                out.append(f"  {ds}")
                out.append(f"  R31 = 0x{a + 8:08x}u; {self.call(ins.target)}")
            elif n == "j":
                out.append(f"  {ds}")
                if ins.target in f.tailcalls or ins.target not in addrset:
                    out.append(f"  {self.call(ins.target)} return;")
                else:
                    if back:
                        out.append(f"  SAFEPOINT(c, 0x{a:08x}u);")
                    out.append(f"  goto L_{ins.target:08x};")
            elif n == "jr":
                if ins.rs == 31:
                    out.append(f"  {ds}")
                    out.append("  return;")
                elif a in f.bios:
                    vec, num = f.bios[a]
                    out.append(f"  {ds}")
                    out.append(f"  bios_call(c, 0x{vec:02x}); return;")
                elif a in f.jumptables:
                    table, targets = f.jumptables[a]
                    out.append(f"  {{ uint32_t t = {R(ins.rs)}; {ds}")
                    out.append("    switch (t) {")
                    for tg in sorted(set(targets)):
                        out.append(f"    case 0x{tg:08x}u: goto L_{tg:08x};")
                    out.append(f"    default: call_indirect(c, t); return; }} }}")
                else:
                    out.append(f"  {{ uint32_t t = {R(ins.rs)}; {ds}")
                    out.append("    call_indirect(c, t); return; }")
            elif n == "jalr":
                out.append(f"  {{ uint32_t t = {R(ins.rs)}; {ds}")
                if ins.rd:
                    out.append(f"    R{ins.rd} = 0x{a + 8:08x}u;")
                out.append("    call_indirect(c, t); }")
            # Someone branches into our delay slot: emit it standalone
            if (a + 4) in f.labels:
                out.append(f"  goto L_{a + 8:08x};")
                out.append(f"L_{a + 4:08x}:;")
                s = self.op(d)
                if s:
                    out.append("  " + s)
                if (a + 8) not in f.labels:
                    f.labels.add(a + 8)
            # code after a conditional branch/jal continues; make sure it exists
            if (ins.is_branch or n in ("jal", "jalr")) and (a + 8) not in addrset:
                out.append(f"  {self.call(a + 8)} return;")
        out.append("}")
        return "\n".join(out)


HEADER = """/* generated by tools/recomp/gen.py -- do not edit */
#include "recomp.h"
#include "recomp_regs.h"
#include "recomp_funcs.h"
"""


def write_bodies(g, funcs, outdir, stem, per, header):
    starts = sorted(funcs)
    bodies = {s: g.func(funcs[s]) for s in starts}
    files = []
    for k in range(0, len(starts), per):
        name = f"{stem}_{len(files):03d}.c"
        with open(os.path.join(outdir, name), "w") as o:
            o.write(header)
            for s in starts[k:k + per]:
                o.write("\n" + bodies[s] + "\n")
        files.append(name)
    return files


def main():
    exe = Exe(sys.argv[1])
    outdir = sys.argv[2]
    per = 40
    if "--per-file" in sys.argv:
        per = int(sys.argv[sys.argv.index("--per-file") + 1])
    os.makedirs(outdir, exist_ok=True)
    for x in os.listdir(outdir):
        if x.endswith(".c") and (x.startswith("recomp_") or x.startswith("ov_")):
            os.remove(os.path.join(outdir, x))
    an = Analyzer(exe)
    if "--seeds" in sys.argv:
        an.load_seeds(sys.argv[sys.argv.index("--seeds") + 1])

    # overlays: --overlays <dir> <hexbase> ; every file in dir is an overlay image
    overlays = []
    if "--overlays" in sys.argv:
        i = sys.argv.index("--overlays")
        ovdir, ovbase = sys.argv[i + 1], int(sys.argv[i + 2], 16)
        for k, name in enumerate(sorted(os.listdir(ovdir))):
            raw = open(os.path.join(ovdir, name), "rb").read()
            img = Exe(raw=raw, base=ovbase)
            oan = Analyzer(img)
            ofuncs = oan.run(sweep=True)
            overlays.append((k, name, img, ofuncs))
            # main-exe functions that only overlays reference
            for kind, t in oan.refs_out(exe.base, exe.end):
                if kind == "jal":
                    an.extra_seeds.add(t)
                else:
                    first = exe.insn(t)
                    if first.name == "addiu" and first.rs == 29 and first.rt == 29 and first.simm < 0:
                        an.extra_seeds.add(t)

    funcs = an.run()
    g = Gen(exe, funcs)
    g.overrides = {}
    ov_overrides = {}  # "addr@OVERLAY.NAME hook ..." applies to that overlay only
    if "--overrides" in sys.argv:
        for line in open(sys.argv[sys.argv.index("--overrides") + 1]):
            parts = line.split("#")[0].split()
            if len(parts) >= 2:
                if "@" in parts[0]:
                    addr, ovname = parts[0].split("@", 1)
                    ov_overrides.setdefault(ovname.upper(), {})[int(addr, 16)] = parts[1:]
                else:
                    g.overrides[int(parts[0], 16)] = parts[1:]
    starts = sorted(funcs)
    main_starts = set(starts)

    with open(os.path.join(outdir, "recomp_funcs.h"), "w") as o:
        o.write("/* generated */\n#pragma once\n#include \"recomp.h\"\n")
        for s in starts:
            o.write(f"void {fn(s)}(CPU *c);\n")
        o.write(f"#define RECOMP_ENTRY {fn(exe.pc)}\n")
        o.write(f"#define RECOMP_ENTRY_ADDR 0x{exe.pc:08x}u\n")
        o.write(f"#define RECOMP_NUM_FUNCS {len(starts)}\n")

    files = write_bodies(g, funcs, outdir, "recomp", per, HEADER)
    with open(os.path.join(outdir, "recomp_dispatch.c"), "w") as o:
        o.write(HEADER)
        o.write("const RecompFunc recomp_table[] = {\n")
        for s in starts:
            o.write(f"  {{0x{s:08x}u, {fn(s)}}},\n")
        o.write("};\nconst int recomp_table_size = sizeof(recomp_table)/sizeof(recomp_table[0]);\n")
    files.append("recomp_dispatch.c")
    print(f"main: {len(starts)} functions")

    # overlay code + registry
    sigs = {}
    reg = [HEADER, "#include \"recomp_overlays.h\"\n"]
    entries = []
    with open(os.path.join(outdir, "recomp_overlays.h"), "w") as oh:
        oh.write("/* generated */\n#pragma once\n#include \"recomp.h\"\n")
        for k, name, img, ofuncs in overlays:
            pre = f"O{k:02d}_"
            og = Gen(img, ofuncs, prefix=pre, external=main_starts)
            og.overrides = ov_overrides.get(os.path.basename(name).upper(), {})
            for s in sorted(ofuncs):
                oh.write(f"void {pre}{s:08x}(CPU *c);\n")
            files += write_bodies(og, ofuncs, outdir, f"ov_{k:02d}", per,
                                  HEADER + "#include \"recomp_overlays.h\"\n")
            # signature: 32 code bytes at the first function start no other overlay shares
            sig = None
            for s in sorted(ofuncs):
                off = s - img.base
                b = img.data[off:off + 32]
                if len(b) == 32 and b not in sigs:
                    sig = (off, b)
                    sigs[b] = name
                    break
            assert sig, f"no unique signature for {name}"
            reg.append(f"static const RecompFunc ov{k:02d}_table[] = {{\n")
            for s in sorted(ofuncs):
                reg.append(f"  {{0x{s:08x}u, {pre}{s:08x}}},\n")
            reg.append("};\n")
            sb = ", ".join(f"0x{x:02x}" for x in sig[1])
            entries.append(f"  {{\"{name}\", 0x{img.base:08x}u, 0x{len(img.data):x}u, 0x{sig[0]:x}u, {{{sb}}}, "
                           f"ov{k:02d}_table, {len(ofuncs)}}},\n")
            print(f"overlay {k:02d} {name}: {len(ofuncs)} functions, sig @+0x{sig[0]:x}")
            for w in og.warn[:5]:
                print("  warn:", w)
    reg.append("const RecompOverlay recomp_overlays[] = {\n")
    reg += entries if entries else ["  {0}\n"]
    reg.append(f"}};\nconst int recomp_num_overlays = {len(entries)};\n")
    with open(os.path.join(outdir, "recomp_overlays.c"), "w") as o:
        o.write("".join(reg))
    files.append("recomp_overlays.c")

    with open(os.path.join(outdir, "recomp_files.cmake"), "w") as o:
        o.write("set(RECOMP_SOURCES\n")
        for f in files:
            o.write(f"  ${{CMAKE_CURRENT_LIST_DIR}}/{f}\n")
        o.write(")\n")

    for w in g.warn[:50]:
        print("warn:", w)


if __name__ == "__main__":
    main()
