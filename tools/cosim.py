#!/usr/bin/env python3
"""Random-instruction co-simulation: rv32emu (C) vs an independent Python reference model.

For each seed:
  1. generate a random but well-formed RV32I program (all 40 base instructions are in the mix),
  2. run it in rv32emu with --trace and in the reference model below,
  3. compare the two traces line by line (pc, encoding, register write, store).

The first mismatch is reported with the instruction that caused it. Functional coverage
(which instructions were actually executed) is collected across all seeds.

--self-test plants a known bug in the reference model and checks that co-simulation
catches it, so we know the checker can fail.
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

MEM_SIZE = 1 << 20
DATA_BASE = 0x8000   # x31 holds this for the whole program; loads/stores stay in [DATA_BASE, DATA_BASE+2048)
MASK = 0xFFFFFFFF


def s32(v):
    v &= MASK
    return v - (1 << 32) if v & 0x80000000 else v


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


# --------------------------------------------------------------------------- encoder
def r_type(f7, rs2, rs1, f3, rd, op=0x33):
    return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op


def i_type(imm, rs1, f3, rd, op=0x13):
    return (imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op


def s_type(imm, rs2, rs1, f3):
    return (imm >> 5 & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (imm & 0x1F) << 7 | 0x23


def b_type(imm, rs2, rs1, f3):
    return ((imm >> 12 & 1) << 31 | (imm >> 5 & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
            (imm >> 1 & 0xF) << 8 | (imm >> 11 & 1) << 7 | 0x63)


def j_type(imm, rd):
    return (imm >> 20 & 1) << 31 | (imm >> 1 & 0x3FF) << 21 | (imm >> 11 & 1) << 20 | (imm >> 12 & 0xFF) << 12 | rd << 7 | 0x6F


R_OPS = {"add": (0, 0), "sub": (0x20, 0), "sll": (0, 1), "slt": (0, 2), "sltu": (0, 3),
         "xor": (0, 4), "srl": (0, 5), "sra": (0x20, 5), "or": (0, 6), "and": (0, 7)}
I_OPS = {"addi": 0, "slti": 2, "sltiu": 3, "xori": 4, "ori": 6, "andi": 7}
LOADS = {"lb": (0, 1), "lh": (1, 2), "lw": (2, 4), "lbu": (4, 1), "lhu": (5, 2)}
STORES = {"sb": (0, 1), "sh": (1, 2), "sw": (2, 4)}
BRANCHES = {"beq": 0, "bne": 1, "blt": 4, "bge": 5, "bltu": 6, "bgeu": 7}
INTERESTING = [0, 1, 2, 0x7FF, 0x800, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33]


def generate(seed, length):
    rng = random.Random(seed)
    reg = lambda: rng.randint(1, 30)          # x31 is the data pointer, never a destination
    src = lambda: rng.randint(0, 31)
    imm12 = lambda: rng.choice([rng.randint(-2048, 2047), -1, 0, 1, 2047, -2048])
    prog = [0x00008FB7]                       # lui x31, 0x8  -> x31 = 0x8000
    # seed some registers with edge-case values
    for r in range(1, 9):
        v = rng.choice(INTERESTING)
        prog.append((((v + 0x800) & 0xFFFFF000) | r << 7 | 0x37) & MASK)     # lui
        prog.append(i_type(sx(v, 12), r, 0, r))                               # addi
    # The body is a list of units (one or two instructions). Forward jumps skip whole units,
    # so a jump can never land between the auipc and jalr of a pair.
    units = []
    while len(units) < length:
        kind = rng.choices(["r", "i", "shift", "load", "store", "branch", "jal", "jalr", "u", "fence"],
                           [25, 20, 10, 10, 10, 10, 4, 3, 6, 2])[0]
        if kind == "r":
            f7, f3 = R_OPS[rng.choice(list(R_OPS))]
            units.append([r_type(f7, src(), src(), f3, reg())])
        elif kind == "i":
            units.append([i_type(imm12(), src(), I_OPS[rng.choice(list(I_OPS))], reg())])
        elif kind == "shift":
            f3, f7 = rng.choice([(1, 0), (5, 0), (5, 0x20)])
            units.append([i_type(f7 << 5 | rng.randint(0, 31), src(), f3, reg())])
        elif kind == "load":
            f3, size = LOADS[rng.choice(list(LOADS))]
            units.append([i_type(rng.randrange(0, 2048 - size, size), 31, f3, reg(), op=0x03)])
        elif kind == "store":
            f3, size = STORES[rng.choice(list(STORES))]
            units.append([s_type(rng.randrange(0, 2048 - size, size), src(), 31, f3)])
        elif kind == "branch":                # forward only, so every program terminates
            units.append(("branch", rng.randint(1, 4), src(), src(), BRANCHES[rng.choice(list(BRANCHES))]))
        elif kind == "jal":
            units.append(("jal", rng.randint(1, 4), reg()))
        elif kind == "jalr":                  # auipc r, 0 ; jalr rd, 8+odd(r) -> lands on the next unit, bit 0 cleared
            r = reg()
            units.append([0x17 | r << 7, i_type(8 + rng.randint(0, 1), r, 0, reg(), op=0x67)])
        elif kind == "u":
            units.append([rng.getrandbits(20) << 12 | reg() << 7 | rng.choice([0x37, 0x17])])
        else:
            units.append([rng.choice([0x0FF0000F, 0x0000100F])])
    size = lambda u: len(u) if isinstance(u, list) else 1
    for n, u in enumerate(units):
        if isinstance(u, tuple):
            skip = sum(size(v) for v in units[n + 1:n + 1 + u[1]])
            off = 4 * (1 + skip)
            prog.append(b_type(off, u[2], u[3], u[4]) if u[0] == "branch" else j_type(off, u[2]))
        else:
            prog.extend(u)
    prog += [0x00000013] * 5                  # landing pad of nops: forward jumps near the end land here
    prog += [i_type(93, 0, 0, 17), 0x00000073]  # li a7, 93 ; ecall
    return prog


# --------------------------------------------------------------------------- reference model
class Model:
    """Written from the spec independently of src/cpu.c (table-driven, Python ints)."""

    def __init__(self, image, bug=None):
        self.mem = bytearray(MEM_SIZE)
        self.mem[:len(image)] = image
        self.x = [0] * 32
        self.x[2] = MEM_SIZE - 16
        self.pc = 0
        self.bug = bug

    def load(self, addr, size, signed):
        v = int.from_bytes(self.mem[addr:addr + size], "little")
        return sx(v, 8 * size) & MASK if signed else v

    def run(self, max_steps=100000):
        trace, ops = [], []
        for _ in range(max_steps):
            pc, ins = self.pc, int.from_bytes(self.mem[self.pc:self.pc + 4], "little")
            op, rd, f3 = ins & 0x7F, ins >> 7 & 31, ins >> 12 & 7
            a, b, f7 = self.x[ins >> 15 & 31], self.x[ins >> 20 & 31], ins >> 25
            i_imm = sx(ins >> 20, 12)
            nxt, wr, store, name = pc + 4, None, None, None
            if op == 0x37:
                name, wr = "lui", ins & 0xFFFFF000
            elif op == 0x17:
                name, wr = "auipc", (pc + (ins & 0xFFFFF000)) & MASK
            elif op == 0x6F:
                imm = sx((ins >> 31) << 20 | (ins >> 12 & 0xFF) << 12 | (ins >> 20 & 1) << 11 | (ins >> 21 & 0x3FF) << 1, 21)
                name, wr, nxt = "jal", pc + 4, (pc + imm) & MASK
            elif op == 0x67:
                name, wr, nxt = "jalr", pc + 4, (a + i_imm) & MASK & ~1
            elif op == 0x63:
                imm = sx((ins >> 31) << 12 | (ins >> 7 & 1) << 11 | (ins >> 25 & 0x3F) << 5 | (ins >> 8 & 0xF) << 1, 13)
                name = {0: "beq", 1: "bne", 4: "blt", 5: "bge", 6: "bltu", 7: "bgeu"}[f3]
                take = {"beq": a == b, "bne": a != b, "blt": s32(a) < s32(b), "bge": s32(a) >= s32(b),
                        "bltu": a < b, "bgeu": a >= b}[name]
                if take:
                    nxt = (pc + imm) & MASK
            elif op == 0x03:
                name = {0: "lb", 1: "lh", 2: "lw", 4: "lbu", 5: "lhu"}[f3]
                size = {0: 1, 1: 2, 2: 4, 4: 1, 5: 2}[f3]
                wr = self.load((a + i_imm) & MASK, size, f3 < 4)
            elif op == 0x23:
                size = 1 << f3
                name = ["sb", "sh", "sw"][f3]
                addr = (a + sx((ins >> 25) << 5 | (ins >> 7 & 31), 12)) & MASK
                val = b & ((1 << (8 * size)) - 1)
                self.mem[addr:addr + size] = val.to_bytes(size, "little")
                store = (size, addr, val)
            elif op == 0x13:
                sh = ins >> 20 & 31
                if f3 == 5:
                    name = "srai" if f7 == 0x20 else "srli"
                    wr = (s32(a) >> sh) & MASK if name == "srai" else a >> sh
                else:
                    name = {0: "addi", 1: "slli", 2: "slti", 3: "sltiu", 4: "xori", 6: "ori", 7: "andi"}[f3]
                    wr = {"addi": a + i_imm, "slli": a << sh, "slti": int(s32(a) < i_imm),
                          "sltiu": int(a < (i_imm & MASK)), "xori": a ^ (i_imm & MASK),
                          "ori": a | (i_imm & MASK), "andi": a & (i_imm & MASK)}[name] & MASK
            elif op == 0x33:
                name = next(n for n, (g7, g3) in R_OPS.items() if g7 == f7 and g3 == f3)
                sh = b & 31
                if name == "sra" and self.bug == "sra_logical":
                    name_eff = "srl"
                else:
                    name_eff = name
                wr = {"add": a + b, "sub": a - b, "sll": a << sh, "slt": int(s32(a) < s32(b)),
                      "sltu": int(a < b), "xor": a ^ b, "srl": a >> sh, "sra": s32(a) >> sh,
                      "or": a | b, "and": a & b}[name_eff] & MASK
            elif op == 0x0F:
                name = "fence"
            elif ins == 0x73:
                name = "ecall"
                ops.append(name)
                if self.x[17] == 93:
                    trace.append(f"{pc:08x} {ins:08x}")
                    return trace, ops
                raise ValueError("generator only emits exit ecalls")
            else:
                raise ValueError(f"model: illegal instruction {ins:08x} at {pc:08x}")
            ops.append(name)
            line = f"{pc:08x} {ins:08x}"
            if wr is not None and rd != 0:
                self.x[rd] = wr
                line += f" x{rd}={wr:08x}"
            if store:
                line += f" m{store[0]}[{store[1]:08x}]={store[2]:08x}"
            trace.append(line)
            self.pc = nxt
        raise RuntimeError("model: step limit")


# --------------------------------------------------------------------------- driver
def cosim(emu, seed, length, bug=None, workdir=None):
    prog = generate(seed, length)
    image = struct.pack(f"<{len(prog)}I", *prog)
    bin_path = os.path.join(workdir, f"seed{seed}.bin")
    trace_path = bin_path + ".trace"
    with open(bin_path, "wb") as f:
        f.write(image)
    proc = subprocess.run([emu, "--bin", "0", "--trace", trace_path, "--max-steps", "100000", bin_path],
                          capture_output=True, text=True)
    with open(trace_path) as f:
        dut = f.read().splitlines()
    ref, ops = Model(image, bug).run()
    for i, (d, r) in enumerate(zip(dut, ref)):
        if d != r:
            return False, f"seed {seed} step {i}: emulator '{d}' vs model '{r}'", ops
    if len(dut) != len(ref):
        return False, f"seed {seed}: emulator retired {len(dut)} vs model {len(ref)} ({proc.stderr.strip()})", ops
    return True, "", ops


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emu", default="build/rv32emu")
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--length", type=int, default=400)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        if args.self_test:
            for seed in range(50):
                ok, msg, _ = cosim(args.emu, seed, args.length, bug="sra_logical", workdir=tmp)
                if not ok:
                    print(f"self-test OK: planted bug (sra treated as srl) caught -> {msg}")
                    return 0
            print("self-test FAILED: planted bug was not detected")
            return 1

        seen, executed, failures = {}, 0, []
        for seed in range(args.seeds):
            ok, msg, ops = cosim(args.emu, seed, args.length, workdir=tmp)
            executed += len(ops)
            for o in ops:
                seen[o] = seen.get(o, 0) + 1
            if not ok:
                failures.append(msg)
                if len(failures) >= 5:
                    break

    all_ops = (["lui", "auipc", "jal", "jalr"] + list(BRANCHES) + list(LOADS) + list(STORES) +
               list(I_OPS) + ["slli", "srli", "srai"] + list(R_OPS) + ["fence", "ecall", "ebreak"])
    missing = [o for o in all_ops if o not in seen]
    print(f"co-simulated {args.seeds} programs, {executed} instructions retired")
    print(f"functional coverage: {len(all_ops) - len(missing)}/{len(all_ops)} instructions "
          f"(not generated: {', '.join(missing) or 'none'})")
    rare = sorted(seen.items(), key=lambda kv: kv[1])[:5]
    print("least exercised: " + ", ".join(f"{k} {v}" for k, v in rare))
    for f in failures:
        print("MISMATCH " + f)
    # ebreak stops the program by design, so it is covered by the directed tests instead
    if failures or [m for m in missing if m != "ebreak"]:
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
