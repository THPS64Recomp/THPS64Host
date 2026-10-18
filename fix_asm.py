"""Post-process splat/spimdisasm output so modern GNU as reassembles it 1:1.

THPS1's linker filled inter-object alignment padding with *garbage* bytes
(not zeros/nops). spimdisasm disassembles that garbage as instructions, some
of which GNU as rejects for vr4300 (`pref`) or would re-encode differently
("handwritten instruction": non-canonical fields). This rewrites any such line
as a raw `.word` taken from spimdisasm's own `/* off vram word */` comment.

usage: python -I decomp/common/fix_asm.py --game thps2 FILE.s [...]   (edits in place)
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops it
import thpsgame  # noqa: E402

LINE = re.compile(r"^(\s*/\* [0-9A-F]+ [0-9A-F]{8} ([0-9A-F]{8}) \*/\s+)(\S+)(.*)$")
# mnemonics GNU as refuses for -march=vr4300, or that are never compiler output
FORCE_WORD = {"pref", "sdc2", "ldc2", "lwc2", "swc2", "lld", "scd",
              "mfc2", "mtc2", "cfc2", "ctc2"}
# jal/j into nowhere (outside boot+main code) can only be padding garbage;
# set from the game table in main
CODE_LO, CODE_HI = 0x80000400, 0x800F6460


def fix(path):
    out, n = [], 0
    lines = open(path).readlines()
    defined = {m.group(1) for m in (re.match(r"^\s*(?:[a-z]label\s+)?(\.L[0-9A-Za-z_]+):?\s*$", l) for l in lines) if m}
    for line in lines:
        m = LINE.match(line)
        if m:
            prefix, word, mnem, rest = m.groups()
            mn = mnem.lstrip(".")
            bad_jal = False
            if mn in ("jal", "j"):
                t = re.search(r"func_([0-9A-F]{8})", rest)
                bad_jal = t is not None and not CODE_LO <= int(t.group(1), 16) < CODE_HI
            lab = re.search(r"(\.L[0-9A-F]{8}\w*)", rest)
            bad_label = lab is not None and lab.group(1) not in defined and not mnem.startswith(".")  # garbage branch across files
            if mn in FORCE_WORD or bad_jal or bad_label:
                line = f"{prefix}.word 0x{word}  /* {mnem}{rest.split('/*')[0].rstrip()} */\n"
                n += 1
        out.append(line)
    open(path, "w").writelines(out)
    return n


if __name__ == "__main__":
    g, files = thpsgame.from_argv(sys.argv[1:])
    CODE_LO, CODE_HI = g.boot_vram, g.main_end
    total = 0
    for p in files:
        total += fix(p)
    print(f"fix_asm: {total} instruction(s) rewritten as .word")
