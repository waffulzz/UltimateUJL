"""MIPS R3000A (PS1) instruction decoder."""
import struct

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav",
           8: "jr", 9: "jalr", 12: "syscall", 13: "break",
           16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo",
           24: "mult", 25: "multu", 26: "div", 27: "divu",
           32: "add", 33: "addu", 34: "sub", 35: "subu",
           36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}

OPS = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz",
       8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi", 13: "ori",
       14: "xori", 15: "lui", 32: "lb", 33: "lh", 34: "lwl", 35: "lw",
       36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl",
       43: "sw", 46: "swr", 50: "lwc2", 58: "swc2"}

BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}
LOADS = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr"}
STORES = {"sb", "sh", "swl", "sw", "swr"}


class Insn:
    __slots__ = ("addr", "word", "op", "rs", "rt", "rd", "sa", "imm", "simm", "target", "name", "funct")

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = None
        self.name = self._decode()

    def _decode(self):
        op = self.op
        if op == 0:
            n = SPECIAL.get(self.funct, "invalid")
            if self.word == 0:
                return "nop"
            return n
        if op == 1:
            n = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(self.rt, "invalid")
            self.target = (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF
            return n
        if op in (2, 3):
            self.target = ((self.addr + 4) & 0xF0000000) | ((self.word & 0x3FFFFFF) << 2)
            return OPS[op]
        if op in (4, 5, 6, 7):
            self.target = (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF
            return OPS[op]
        if op == 16:  # COP0
            if self.rs == 0:
                return "mfc0"
            if self.rs == 4:
                return "mtc0"
            if self.rs == 16 and self.funct == 16:
                return "rfe"
            return "cop0?"
        if op == 18:  # COP2 / GTE
            if self.rs & 0x10:
                return "gte"
            return {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(self.rs, "cop2?")
        return OPS.get(op, "invalid")

    @property
    def is_branch(self):
        return self.name in BRANCHES

    @property
    def is_jump(self):
        return self.name in ("j", "jal", "jr", "jalr")

    @property
    def has_delay(self):
        return self.is_branch or self.is_jump

    def __str__(self):
        n, r = self.name, REG
        if n in ("nop", "syscall", "break", "rfe", "invalid"):
            a = ""
        elif n in ("sll", "srl", "sra"):
            a = f"{r[self.rd]}, {r[self.rt]}, {self.sa}"
        elif n in ("sllv", "srlv", "srav"):
            a = f"{r[self.rd]}, {r[self.rt]}, {r[self.rs]}"
        elif n == "jr":
            a = r[self.rs]
        elif n == "jalr":
            a = f"{r[self.rd]}, {r[self.rs]}"
        elif n in ("mfhi", "mflo"):
            a = r[self.rd]
        elif n in ("mthi", "mtlo"):
            a = r[self.rs]
        elif n in ("mult", "multu", "div", "divu"):
            a = f"{r[self.rs]}, {r[self.rt]}"
        elif self.op == 0:
            a = f"{r[self.rd]}, {r[self.rs]}, {r[self.rt]}"
        elif n in ("j", "jal"):
            a = f"0x{self.target:08x}"
        elif n in ("beq", "bne"):
            a = f"{r[self.rs]}, {r[self.rt]}, 0x{self.target:08x}"
        elif self.is_branch:
            a = f"{r[self.rs]}, 0x{self.target:08x}"
        elif n == "lui":
            a = f"{r[self.rt]}, 0x{self.imm:04x}"
        elif n in ("andi", "ori", "xori"):
            a = f"{r[self.rt]}, {r[self.rs]}, 0x{self.imm:04x}"
        elif n in LOADS or n in STORES:
            a = f"{r[self.rt]}, {self.simm}({r[self.rs]})"
        elif n in ("lwc2", "swc2"):
            a = f"gte{self.rt}, {self.simm}({r[self.rs]})"
        elif n in ("mfc0", "mtc0", "mfc2", "mtc2", "cfc2", "ctc2"):
            a = f"{r[self.rt]}, ${self.rd}"
        elif n == "gte":
            a = f"0x{self.word & 0x1FFFFFF:07x}"
        else:
            a = f"{r[self.rt]}, {r[self.rs]}, {self.simm}"
        return f"{n:8s}{a}"


class Exe:
    def __init__(self, path=None, raw=None, base=None):
        if raw is not None:  # raw overlay image loaded at `base`
            self.pc, self.gp, self.base, self.sp = None, 0, base, 0
            self.data = raw + b"\x00" * (-len(raw) % 4)
            self.tsize = len(self.data)
            self.end = self.base + len(self.data)
            return
        d = open(path, "rb").read()
        assert d[:8] == b"PS-X EXE"
        self.pc, self.gp, self.base, self.tsize = struct.unpack_from("<4I", d, 0x10)
        self.sp = struct.unpack_from("<I", d, 0x30)[0]
        self.data = d[0x800:0x800 + self.tsize]
        self.end = self.base + len(self.data)

    def contains(self, addr):
        return self.base <= addr < self.end

    def word(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def insn(self, addr):
        return Insn(addr, self.word(addr))
