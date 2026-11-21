#!/usr/bin/env python3
"""Run the C sample programs (cross-compiled with clang for RV32I) in rv32emu and
compare their output with programs/expected.txt. Exit code 0 only if all match."""
import subprocess
import sys
from collections import defaultdict


def main(build_dir, expected_path):
    expected = defaultdict(list)
    for line in open(expected_path):
        if line.strip():
            prog, text = line.rstrip("\n").split("|", 1)
            expected[prog].append(text)
    failed = 0
    for prog, lines in expected.items():
        p = subprocess.run([f"{build_dir}/rv32emu", "--stats", f"{build_dir}/{prog}"],
                           capture_output=True, text=True, timeout=60)
        got = p.stdout.splitlines()
        retired = p.stderr.splitlines()[0] if p.stderr else ""
        ok = got == lines and p.returncode == 0
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} {prog:12s} exit={p.returncode} {retired}")
        if not ok:
            print("  expected:", lines)
            print("  got:     ", got)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
